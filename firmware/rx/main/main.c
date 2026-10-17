/*
 * PS5 wireless audio link - RECEIVER (ESP32-S3 USB host, worn with the headphones).
 *
 *   ESP-NOW v2 --> jitter buffer (loss recovery, drift correction) --> USB host
 *   --> USB Audio Class dongle --> 3.5 mm --> WH-1000XM4
 */
#include <string.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "usb/usb_host.h"
#include "usb/uac_host.h"
#include "link_proto.h"
#include "jbuf.h"

static const char *TAG = "link_rx";

/* ---- tuning ---- */
#define DONGLE_VOLUME        60      /* 0..100 on the dongle; PS5 volume is applied digitally on top */
#define USB_RING_BYTES       768     /* 4 ms of 48 kHz stereo 16-bit queued for the dongle */
#define USB_CHUNK_SAMPLES    48      /* write 1 ms at a time so the ring stays shallow */

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static jbuf_t s_jb;
static volatile uac_host_device_handle_t s_spk;
static volatile int8_t s_rssi;
static QueueHandle_t s_evq;
static volatile uint32_t s_wr_ok, s_wr_err, s_wr_last_err;

/* ------------------------------------------------------------------ */
/* Remote log: every ESP_LOG line is also broadcast over ESP-NOW, and the
 * transmitter prints it on its serial port (the Nano's USB is busy hosting). */

typedef struct {
    uint32_t magic;
    uint8_t len;
    char txt[LINK_LOG_MAX];
} __attribute__((packed)) remote_log_t;

static QueueHandle_t s_logq;
static const uint8_t BCAST[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* Log a line locally and queue it for the transmitter. Called only from this file's tasks
 * (all have >= 4 KB stacks), never hooked into the global logger. */
static void rlog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    remote_log_t m = { .magic = LINK_LOG_MAGIC };
    int n = vsnprintf(m.txt, sizeof(m.txt), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    m.len = (uint8_t)(n < (int)sizeof(m.txt) ? n : (int)sizeof(m.txt) - 1);
    ESP_LOGI(TAG, "%s", m.txt);
    if (s_logq) xQueueSend(s_logq, &m, 0);
}

static void log_task(void *arg)
{
    (void)arg;
    remote_log_t m;
    while (1) {
        if (xQueueReceive(s_logq, &m, portMAX_DELAY)) {
            esp_now_send(BCAST, (const uint8_t *)&m, 5 + m.len);
            vTaskDelay(pdMS_TO_TICKS(3));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Radio */

static void espnow_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    static link_packet_t pkt __attribute__((aligned(4)));   /* WiFi task only */
    if (len <= 0 || (size_t)len > sizeof(pkt)) {
        return;
    }
    memcpy(&pkt, data, (size_t)len);
    if (info && info->rx_ctrl) {
        s_rssi = info->rx_ctrl->rssi;
    }
    uint64_t now = (uint64_t)esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    jb_push(&s_jb, &pkt, (size_t)len, now);
    portEXIT_CRITICAL(&s_lock);
}

static void radio_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));          /* radio always listening */
#if defined(LINK_LOW_POWER_TX)
    /* Receiver only listens: cap transmit power at ~8 dBm (units of 0.25 dBm) to cut current peaks */
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(32));
#endif
    ESP_ERROR_CHECK(esp_wifi_set_channel(LINK_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));
    esp_now_peer_info_t peer = { .channel = LINK_WIFI_CHANNEL, .ifidx = WIFI_IF_STA };
    memcpy(peer.peer_addr, BCAST, ESP_NOW_ETH_ALEN);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    /* remote log relay disabled: logs go to the UART TX pin only */

    uint32_t ver = 0;
    esp_now_get_version(&ver);
    ESP_LOGI(TAG, "ESP-NOW v%" PRIu32 " listening on channel %d", ver, LINK_WIFI_CHANNEL);
}

/* ------------------------------------------------------------------ */
/* Playback: pulls from the jitter buffer, paced by the dongle's consumption */

static void play_task(void *arg)
{
    (void)arg;
    static int16_t out[JB_OUT_MAX * LINK_CHANNELS];

    while (1) {
        uac_host_device_handle_t spk = s_spk;
        uint64_t now = (uint64_t)esp_timer_get_time();

#if defined(LINK_DIAG_TONE)
        /* Diagnostic: ignore the radio, play a continuous 1 kHz tone at -20 dBFS */
        static uint32_t ph;
        size_t n = LINK_FRAME_SAMPLES;
        for (size_t i = 0; i < n; i++, ph++) {
            int16_t v = (int16_t)(3277.0f * sinf(2.0f * (float)M_PI * 1000.0f * (float)(ph % 48) / 48000.0f));
            out[2 * i] = out[2 * i + 1] = v;
        }
        (void)now;
#else
        portENTER_CRITICAL(&s_lock);
        size_t n = jb_pull(&s_jb, out, now);
        portEXIT_CRITICAL(&s_lock);
#endif

        if (spk == NULL) {
            /* No dongle: keep the buffer moving in real time so it is fresh when one appears */
            vTaskDelay(pdMS_TO_TICKS(3));
            continue;
        }

        for (size_t off = 0; off < n;) {
            size_t chunk = n - off > USB_CHUNK_SAMPLES ? USB_CHUNK_SAMPLES : n - off;
            esp_err_t err = uac_host_device_write(spk, (uint8_t *)&out[off * LINK_CHANNELS],
                                                  chunk * LINK_CHANNELS * sizeof(int16_t), 100);
            if (err != ESP_OK) {
                s_wr_err++;
                s_wr_last_err = (uint32_t)err;
                vTaskDelay(pdMS_TO_TICKS(2));
                break;   /* dongle unplugged or stream stopped; outer loop re-checks s_spk */
            }
            s_wr_ok++;
            off += chunk;
        }
    }
}

/* ------------------------------------------------------------------ */
/* USB host + UAC driver */

typedef struct {
    bool is_driver_evt;
    uint8_t addr, iface;
    int event;
    uac_host_device_handle_t handle;
} uac_evt_t;

static void uac_device_cb(uac_host_device_handle_t h, const uac_host_device_event_t event, void *arg)
{
    (void)arg;
    uac_evt_t e = { .is_driver_evt = false, .event = event, .handle = h };
    xQueueSend(s_evq, &e, 0);
}

static void uac_driver_cb(uint8_t addr, uint8_t iface, const uac_host_driver_event_t event, void *arg)
{
    (void)arg;
    uac_evt_t e = { .is_driver_evt = true, .addr = addr, .iface = iface, .event = event };
    xQueueSend(s_evq, &e, 0);
}

static TaskHandle_t s_probe_h;
static void usb_lib_task(void *arg)
{
    const usb_host_config_t cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    ESP_ERROR_CHECK(usb_host_install(&cfg));
    rlog("USB host installed");
    xTaskNotifyGive((TaskHandle_t)arg);
    if (s_probe_h) xTaskNotifyGive(s_probe_h);
    while (1) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static usb_host_client_handle_t s_cli;
static volatile uint8_t s_new_addr;

static void cli_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)arg;
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        s_new_addr = msg->new_dev.address;
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        rlog("USB device removed");
    }
}

static void usb_probe_task(void *arg)
{
    (void)arg;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    const usb_host_client_config_t cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = { .client_event_callback = cli_event_cb, .callback_arg = NULL },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&cfg, &s_cli));
    while (1) {
        usb_host_client_handle_events(s_cli, pdMS_TO_TICKS(100));
        uint8_t a = s_new_addr;
        if (a) {
            s_new_addr = 0;
            usb_device_handle_t d;
            if (usb_host_device_open(s_cli, a, &d) == ESP_OK) {
                const usb_device_desc_t *dd;
                const usb_config_desc_t *cd;
                usb_host_get_device_descriptor(d, &dd);
                int n_audio = 0;
                if (usb_host_get_active_config_descriptor(d, &cd) == ESP_OK) {
                    int off = 0;
                    const usb_standard_desc_t *p = (const usb_standard_desc_t *)cd;
                    while ((p = usb_parse_next_descriptor_of_type(p, cd->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE, &off))) {
                        if (((const usb_intf_desc_t *)p)->bInterfaceClass == USB_CLASS_AUDIO) n_audio++;
                    }
                }
                rlog("USB device enumerated: addr %u VID %04x PID %04x class %02x, %d audio interface(s)",
                         a, dd->idVendor, dd->idProduct, dd->bDeviceClass, n_audio);
                usb_host_device_close(s_cli, d);
            } else {
                rlog("USB device at addr %u could not be opened", a);
            }
        }
    }
}

static bool pick_48k_stereo(uac_host_device_handle_t h, uac_host_dev_alt_param_t *out)
{
    uac_host_dev_info_t info;
    if (uac_host_get_device_info(h, &info) != ESP_OK) {
        return false;
    }
    for (uint8_t alt = 1; alt <= info.iface_alt_num; alt++) {
        uac_host_dev_alt_param_t p;
        if (uac_host_get_device_alt_param(h, alt, &p) != ESP_OK) {
            continue;
        }
        if (p.channels != 2 || p.bit_resolution != 16) {
            continue;
        }
        bool ok = false;
        if (p.sample_freq_type > 0) {
            for (int i = 0; i < p.sample_freq_type && i < UAC_FREQ_NUM_MAX; i++) {
                ok |= (p.sample_freq[i] == LINK_SAMPLE_RATE);
            }
        } else {
            ok = LINK_SAMPLE_RATE >= p.sample_freq_lower && LINK_SAMPLE_RATE <= p.sample_freq_upper;
        }
        if (ok) {
            *out = p;
            return true;
        }
    }
    return false;
}

static void open_speaker(uint8_t addr, uint8_t iface)
{
    const uac_host_device_config_t dev_cfg = {
        .addr = addr,
        .iface_num = iface,
        .buffer_size = USB_RING_BYTES,
        .buffer_threshold = USB_CHUNK_SAMPLES * LINK_CHANNELS * sizeof(int16_t),
        .callback = uac_device_cb,
        .callback_arg = NULL,
    };
    uac_host_device_handle_t h = NULL;
    if (uac_host_device_open(&dev_cfg, &h) != ESP_OK) {
        rlog("ERROR: uac_host_device_open failed");
        return;
    }
    uac_host_printf_device_param(h);
    uac_host_dev_alt_param_t p;
    if (!pick_48k_stereo(h, &p)) {
        rlog("ERROR: dongle has no 48 kHz / 16-bit / stereo output");
        uac_host_device_close(h);
        return;
    }
    const uac_host_stream_config_t stm = {
        .channels = 2,
        .bit_resolution = 16,
        .sample_freq = LINK_SAMPLE_RATE,
    };
    if (uac_host_device_start(h, &stm) != ESP_OK) {
        rlog("ERROR: uac_host_device_start failed");
        uac_host_device_close(h);
        return;
    }
    uac_host_device_set_mute(h, false);
    uac_host_device_set_volume(h, DONGLE_VOLUME);
    s_spk = h;
    rlog("dongle streaming: 48 kHz stereo 16-bit, volume %d", DONGLE_VOLUME);
}

static void uac_task(void *arg)
{
    (void)arg;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    const uac_host_driver_config_t cfg = {
        .create_background_task = true,
        .task_priority = 16,
        .stack_size = 4096,
        .core_id = 0,
        .callback = uac_driver_cb,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(uac_host_install(&cfg));
    rlog("UAC driver ready, waiting for dongle");

    uac_evt_t e;
    while (1) {
        if (!xQueueReceive(s_evq, &e, portMAX_DELAY)) {
            continue;
        }
        if (e.is_driver_evt) {
            if (e.event == UAC_HOST_DRIVER_EVENT_TX_CONNECTED && s_spk == NULL) {
                open_speaker(e.addr, e.iface);
            }
        } else if (e.event == UAC_HOST_DRIVER_EVENT_DISCONNECTED) {
            if (e.handle == s_spk) {
                s_spk = NULL;
                rlog("dongle disconnected");
            }
            uac_host_device_close(e.handle);
        } else if (e.event == UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR) {
            rlog("USB transfer error event");
        }
    }
}

/* ------------------------------------------------------------------ */

void app_main(void)
{
    jb_init(&s_jb);
    s_evq = xQueueCreate(16, sizeof(uac_evt_t));
    radio_init();
    rlog("BOOT: receiver %s, last reset reason %d (1=power-on 3=sw 4=panic 6/7=watchdog 9=brownout)",
#if defined(LINK_DIAG_TONE)
         "TONE TEST",
#else
         "normal",
#endif
         (int)esp_reset_reason());

#if defined(LINK_DIAG_RADIO)
    /* Diagnostic: no USB host. Logs go to the USB-C port; report reception once a second. */
    ESP_LOGI(TAG, "RADIO DIAGNOSTIC: listening only, no audio output");
    uint32_t last_pk = 0, last_bad = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint32_t pk, bad, seq;
        bool have;
        portENTER_CRITICAL(&s_lock);
        pk = s_jb.st.packets;
        bad = s_jb.st.bad_packets;
        seq = s_jb.latest_seq;
        have = s_jb.have_latest;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "packets/s %" PRIu32 " (expect ~333 while PS5 plays) | bad %" PRIu32 " | rssi %d dBm | last seq %" PRIu32 "%s",
                 pk - last_pk, bad - last_bad, s_rssi, seq, have ? "" : " (none yet)");
        last_pk = pk;
        last_bad = bad;
    }
#endif

    xTaskCreatePinnedToCore(play_task, "play", 4096, NULL, 15, NULL, 1);
    TaskHandle_t uac_h;
    xTaskCreatePinnedToCore(uac_task, "uac", 4096, NULL, 6, &uac_h, 0);
    xTaskCreatePinnedToCore(usb_probe_task, "usb_probe", 4096, NULL, 5, &s_probe_h, 0);
    xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, (void *)uac_h, 17, NULL, 0);

    jb_stats_t prev = {0};
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        jb_stats_t s;
        uint32_t buf_us;
        portENTER_CRITICAL(&s_lock);
        s = s_jb.st;
        buf_us = jb_buffered_us(&s_jb);
        portEXIT_CRITICAL(&s_lock);
        rlog("up %" PRIu32 "s | rssi %d dBm | pkts %" PRIu32 " | played %" PRIu32 " backup %" PRIu32
                 " lost %" PRIu32 " late %" PRIu32 " | drift -%" PRIu32 "/+%" PRIu32
                 " | buffer %.1f ms | dongle %s | usb writes ok %" PRIu32 " err %" PRIu32 " (last %s)",
                 (uint32_t)(esp_timer_get_time() / 1000000), s_rssi, s.packets - prev.packets, s.frames_played - prev.frames_played,
                 s.frames_recovered - prev.frames_recovered, s.frames_concealed - prev.frames_concealed,
                 s.frames_late - prev.frames_late, s.drops - prev.drops, s.inserts - prev.inserts,
                 buf_us / 1000.0, s_spk ? "on" : "off", s_wr_ok, s_wr_err,
                 s_wr_err ? esp_err_to_name((esp_err_t)s_wr_last_err) : "-");
        prev = s;
    }
}
