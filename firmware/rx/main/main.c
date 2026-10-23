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
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/pulse_cnt.h"
#include "driver/gpio.h"
#include "usb/usb_host.h"
#include "usb/uac_host.h"
#include "link_proto.h"
#include "jbuf.h"
#if defined(LINK_MIC)
#include "esp_adc/adc_continuous.h"
#endif

static const char *TAG = "link_rx";

/* ---- tuning ---- */
/* ---- volume (rotary encoder) ----
 * Level 0..100   = dongle hardware volume (0..100 %)
 * Level 101..124 = dongle at 100 % plus digital boost of 0.5 dB per level (max +12 dB),
 *                  with a soft limiter so loud peaks round off instead of clipping hard. */
#define ENC_GPIO_A           8       /* D5 */
#define ENC_GPIO_B           9       /* D6 */
#define ENC_COUNTS_PER_DETENT 4      /* typical EC11 / KY-040: one full quadrature cycle per click */
#define ENC_REVERSE          0       /* set to 1 if clockwise turns the volume down */
#define VOL_STEP             2       /* level change per click */
#define VOL_HW_MAX           100
#define VOL_MAX              124
#define VOL_DEFAULT          100
#define USB_RING_BYTES       768     /* 4 ms of 48 kHz stereo 16-bit queued for the dongle */
#define USB_CHUNK_SAMPLES    48      /* write 1 ms at a time so the ring stays shallow */

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static jbuf_t s_jb;
static volatile uac_host_device_handle_t s_spk;
static volatile int8_t s_rssi;
static QueueHandle_t s_evq;
static volatile uint32_t s_wr_ok, s_wr_err, s_wr_last_err;
#if defined(LINK_MIC)
static SemaphoreHandle_t s_mic_done;
static void espnow_send_cb(const uint8_t *mac, esp_now_send_status_t st);
#endif

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
#if defined(LINK_MIC)
    /* Return path for the mic: same 11 Mbit/s DSSS rate as the audio link */
    esp_now_rate_config_t rate = { .phymode = WIFI_PHY_MODE_11B, .rate = WIFI_PHY_RATE_11M_L };
    ESP_ERROR_CHECK(esp_now_set_peer_rate_config(BCAST, &rate));
    s_mic_done = xSemaphoreCreateBinary();
    xSemaphoreGive(s_mic_done);
    ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_cb));
#endif

    uint32_t ver = 0;
    esp_now_get_version(&ver);
    ESP_LOGI(TAG, "ESP-NOW v%" PRIu32 " listening on channel %d", ver, LINK_WIFI_CHANNEL);
}

/* ------------------------------------------------------------------ */
/* Microphone (LINK_MIC builds): MAX9814 analog out -> ADC1 -> ESP-NOW -> transmitter */
#if defined(LINK_MIC)

#define MIC_ADC_CHANNEL   ADC_CHANNEL_3      /* GPIO4 on ESP32-S3 */
#define MIC_OVERSAMPLE    5                  /* sample at 80 kHz, average 5 -> 16 kHz (~7 dB less ADC noise) */
#define MIC_HPF_POLE      0.995f             /* DC blocker, ~13 Hz corner: removes the MAX9814's 1.25 V bias */

static volatile uint32_t s_mic_sent, s_mic_busy, s_mic_peak, s_mic_clip;

static void espnow_send_cb(const uint8_t *mac, esp_now_send_status_t st)
{
    (void)mac;
    (void)st;
    xSemaphoreGive(s_mic_done);
}

static void mic_task(void *arg)
{
    (void)arg;
    adc_continuous_handle_t h;
    const uint32_t frame_bytes = LINK_MIC_FRAME * MIC_OVERSAMPLE * SOC_ADC_DIGI_RESULT_BYTES;
    adc_continuous_handle_cfg_t hc = {
        .max_store_buf_size = frame_bytes * 4,
        .conv_frame_size = frame_bytes,
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&hc, &h));
    adc_digi_pattern_config_t pat = {
        .atten = ADC_ATTEN_DB_12,            /* ~0-3.1 V; MAX9814 swings 0.25-2.25 V */
        .channel = MIC_ADC_CHANNEL,
        .unit = ADC_UNIT_1,
        .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH,
    };
    adc_continuous_config_t cc = {
        .pattern_num = 1,
        .adc_pattern = &pat,
        .sample_freq_hz = LINK_MIC_RATE * MIC_OVERSAMPLE,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
    };
    ESP_ERROR_CHECK(adc_continuous_config(h, &cc));
    ESP_ERROR_CHECK(adc_continuous_start(h));

    static uint8_t raw[LINK_MIC_FRAME * MIC_OVERSAMPLE * SOC_ADC_DIGI_RESULT_BYTES];
    static link_mic_packet_t pkt;
    pkt.magic = LINK_MIC_MAGIC;
    uint32_t acc = 0, nacc = 0, n = 0, peak = 0;
    float x_prev = -1.0f, y = 0.0f;

    while (1) {
        uint32_t got = 0;
        if (adc_continuous_read(h, raw, sizeof(raw), &got, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        for (uint32_t i = 0; i + SOC_ADC_DIGI_RESULT_BYTES <= got; i += SOC_ADC_DIGI_RESULT_BYTES) {
            const adc_digi_output_data_t *d = (const adc_digi_output_data_t *)&raw[i];
            if (d->type2.channel != MIC_ADC_CHANNEL) {
                continue;
            }
            acc += d->type2.data;
            if (++nacc < MIC_OVERSAMPLE) {
                continue;
            }
            float x = (float)acc * (16.0f / MIC_OVERSAMPLE);   /* 12-bit -> 16-bit scale */
            acc = 0;
            nacc = 0;
            if (x_prev < 0.0f) {
                x_prev = x;
            }
            y = x - x_prev + MIC_HPF_POLE * y;
            x_prev = x;
            int32_t v = (int32_t)y;
            if (v > 32767) { v = 32767; s_mic_clip++; }
            else if (v < -32768) { v = -32768; s_mic_clip++; }
#if defined(LINK_MIC_DIAG)
            /* Diagnostic: 10 s real mic, 10 s digital silence, 10 s 440 Hz test tone, repeat */
            {
                static uint32_t t;
                static float ph;
                uint32_t phase = (t++ / (LINK_MIC_RATE * 10)) % 3;
                if (phase == 1) {
                    v = 0;
                } else if (phase == 2) {
                    ph += 2.0f * 3.14159265f * 440.0f / LINK_MIC_RATE;
                    if (ph > 6.2831853f) ph -= 6.2831853f;
                    v = (int32_t)(3277.0f * sinf(ph));          /* -20 dBFS */
                }
            }
#endif
            uint32_t a = (uint32_t)(v < 0 ? -v : v);
            if (a > peak) peak = a;
            pkt.pcm[n++] = (int16_t)v;
            if (n == LINK_MIC_FRAME) {
                n = 0;
                pkt.seq++;
                if (xSemaphoreTake(s_mic_done, pdMS_TO_TICKS(4)) == pdTRUE) {
                    if (esp_now_send(BCAST, (const uint8_t *)&pkt, sizeof(pkt)) == ESP_OK) {
                        s_mic_sent++;
                    } else {
                        xSemaphoreGive(s_mic_done);
                    }
                } else {
                    s_mic_busy++;
                }
                if (peak > s_mic_peak) s_mic_peak = peak;
                peak = 0;
            }
        }
    }
}
#endif

/* ------------------------------------------------------------------ */
/* Volume: dongle hardware volume + optional digital boost, set by the encoder */

static volatile int s_vol = VOL_DEFAULT;
static volatile int32_t s_boost_q12 = 4096;   /* digital gain, 4096 = 0 dB */
static volatile int s_tick;                   /* samples left of the "limit reached" tick */

static void apply_volume(int level)
{
    float boost_db = level > VOL_HW_MAX ? (float)(level - VOL_HW_MAX) * 0.5f : 0.0f;
    s_boost_q12 = (int32_t)(powf(10.0f, boost_db / 20.0f) * 4096.0f + 0.5f);
    uac_host_device_handle_t spk = s_spk;
    if (spk) {
        uac_host_device_set_volume(spk, (uint8_t)(level > VOL_HW_MAX ? VOL_HW_MAX : level));
    }
}

/* Soft limiter: linear below the knee, then rounds off asymptotically towards full scale */
static inline int16_t soft_clip(int32_t v)
{
    const int32_t knee = 24576;               /* -2.5 dBFS */
    const int32_t room = 32767 - knee;
    int32_t a = v < 0 ? -v : v;
    if (a > knee) {
        int32_t over = a - knee;
        a = knee + (int32_t)(((int64_t)over * room) / (over + room));
    }
    return (int16_t)(v < 0 ? -a : a);
}

/* Post-process a block before it goes to the dongle: boost + limiter + limit tick */
static void process_block(int16_t *pcm, size_t frames)
{
    int32_t g = s_boost_q12;
    int tick = s_tick;
    if (g == 4096 && tick == 0) {
        return;
    }
    static uint32_t tick_ph;
    for (size_t i = 0; i < frames; i++) {
        int32_t add = 0;
        if (tick > 0) {
            /* 1.5 kHz square-ish tick at about -20 dBFS, 30 ms */
            add = ((tick_ph++ / 16) & 1) ? 3000 : -3000;
            tick--;
        }
        for (int c = 0; c < LINK_CHANNELS; c++) {
            int32_t v = ((int32_t)pcm[i * LINK_CHANNELS + c] * g) >> 12;
            pcm[i * LINK_CHANNELS + c] = soft_clip(v + add);
        }
    }
    s_tick = tick;
}

static void vol_load(void)
{
    nvs_handle_t h;
    uint8_t v;
    if (nvs_open("link", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "vol", &v) == ESP_OK && v <= VOL_MAX) {
            s_vol = v;
        }
        nvs_close(h);
    }
}

static void vol_save(int level)
{
    nvs_handle_t h;
    if (nvs_open("link", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "vol", (uint8_t)level);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* Quadrature decoding in the PCNT peripheral (hardware, with glitch filter), polled every 10 ms */
static void enc_task(void *arg)
{
    (void)arg;
    pcnt_unit_config_t uc = { .low_limit = -1000, .high_limit = 1000 };
    pcnt_unit_handle_t unit;
    ESP_ERROR_CHECK(pcnt_new_unit(&uc, &unit));
    pcnt_glitch_filter_config_t fc = { .max_glitch_ns = 1000 };
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(unit, &fc));
    pcnt_chan_config_t ca = { .edge_gpio_num = ENC_GPIO_A, .level_gpio_num = ENC_GPIO_B };
    pcnt_chan_config_t cb = { .edge_gpio_num = ENC_GPIO_B, .level_gpio_num = ENC_GPIO_A };
    pcnt_channel_handle_t cha, chb;
    ESP_ERROR_CHECK(pcnt_new_channel(unit, &ca, &cha));
    ESP_ERROR_CHECK(pcnt_new_channel(unit, &cb, &chb));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(cha, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(cha, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(chb, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(chb, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
    /* bare encoders switch to GND: use the internal pull-ups */
    gpio_set_pull_mode(ENC_GPIO_A, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(ENC_GPIO_B, GPIO_PULLUP_ONLY);
    ESP_ERROR_CHECK(pcnt_unit_enable(unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(unit));
    ESP_ERROR_CHECK(pcnt_unit_start(unit));

    int last = 0;
    int64_t dirty_since = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
        int c = 0;
        pcnt_unit_get_count(unit, &c);
        int detents = (c - last) / ENC_COUNTS_PER_DETENT;
        if (detents != 0) {
            last += detents * ENC_COUNTS_PER_DETENT;
#if ENC_REVERSE
            detents = -detents;
#endif
            int want = s_vol + detents * VOL_STEP;
            int lvl = want < 0 ? 0 : (want > VOL_MAX ? VOL_MAX : want);
            if (lvl != want || (lvl == s_vol && (lvl == 0 || lvl == VOL_MAX))) {
                s_tick = 1440;                 /* 30 ms tick: limit reached */
            }
            if (lvl != s_vol) {
                s_vol = lvl;
                apply_volume(lvl);
                dirty_since = esp_timer_get_time();
            }
        }
        if (c > 800 || c < -800) {             /* keep the counter well inside its limits */
            pcnt_unit_clear_count(unit);
            last -= c;
        }
        if (dirty_since && esp_timer_get_time() - dirty_since > 2000000) {
            vol_save(s_vol);                   /* remember it, but don't wear the flash on every click */
            dirty_since = 0;
        }
    }
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

        process_block(out, n);

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
    int lvl = s_vol;
    uac_host_device_set_volume(h, (uint8_t)(lvl > VOL_HW_MAX ? VOL_HW_MAX : lvl));
    apply_volume(lvl);
    s_spk = h;
    rlog("dongle streaming: 48 kHz stereo 16-bit, volume level %d", lvl);
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

    vol_load();
    apply_volume(s_vol);
    xTaskCreatePinnedToCore(enc_task, "enc", 3072, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(play_task, "play", 4096, NULL, 15, NULL, 1);
#if defined(LINK_MIC)
    xTaskCreatePinnedToCore(mic_task, "mic", 4096, NULL, 12, NULL, 1);
#endif
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
                 " | buffer %.1f ms | dongle %s vol %d | usb writes ok %" PRIu32 " err %" PRIu32 " (last %s)",
                 (uint32_t)(esp_timer_get_time() / 1000000), s_rssi, s.packets - prev.packets, s.frames_played - prev.frames_played,
                 s.frames_recovered - prev.frames_recovered, s.frames_concealed - prev.frames_concealed,
                 s.frames_late - prev.frames_late, s.drops - prev.drops, s.inserts - prev.inserts,
                 buf_us / 1000.0, s_spk ? "on" : "off", s_vol, s_wr_ok, s_wr_err,
                 s_wr_err ? esp_err_to_name((esp_err_t)s_wr_last_err) : "-");
        prev = s;
#if defined(LINK_MIC)
        static uint32_t mic_prev_sent;
        uint32_t ms = s_mic_sent;
        uint32_t pk = s_mic_peak;
        s_mic_peak = 0;
        rlog("mic: sent %" PRIu32 " (expect ~833) busy %" PRIu32 " | peak %.1f%% full scale | clipped %" PRIu32,
             ms - mic_prev_sent, s_mic_busy, pk * 100.0 / 32768.0, s_mic_clip);
        mic_prev_sent = ms;
#endif
    }
}
