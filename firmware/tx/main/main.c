/*
 * PS5 wireless audio link - TRANSMITTER (ESP32-S3, plugged into the PS5).
 *
 *   PS5 --USB Audio Class 1 (16-bit stereo)--> S3 --ESP-NOW v2 broadcast--> receiver
 *
 * Audio is cut into 3 ms frames sent as raw PCM; each packet also carries an
 * IMA-ADPCM copy of the previous frame (single-loss recovery, no retransmission).
 */
#include <string.h>
#include <math.h>
#include <stddef.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include <stdio.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_private/usb_phy.h"
#include "tusb.h"
#include "usb_descriptors.h"
#include "link_proto.h"
#include "link_codec.h"

static const char *TAG = "link_tx";
static const uint8_t BCAST[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* ---- USB audio state (written from TinyUSB callbacks) ---- */
static usb_phy_handle_t s_phy;
static volatile uint32_t s_sample_rate = 48000;
static uint8_t s_mute[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1];
static int16_t s_volume[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1];   /* 1/256 dB */

#if defined(LINK_MIC)
/* Mic: 16 kHz samples from the receiver (WiFi task) -> USB task, which upsamples to 48 kHz */
static StreamBufferHandle_t s_mic;
static volatile bool s_mic_active;                 /* host opened the mic streaming interface */
static uint8_t s_mic_mute[2];
static int16_t s_mic_volume[2];                    /* 1/256 dB */
static volatile uint32_t s_mic_rx, s_mic_lost, s_mic_ovf;
#define MIC_VOL_MIN (-24 * 256)
#define MIC_VOL_MAX (24 * 256)
#endif

/* PCM handoff USB task -> TX task (48 kHz interleaved stereo int16) */
static StreamBufferHandle_t s_pcm;

/* ---- stats ---- */
static volatile uint32_t s_pkts_sent, s_pkts_fail, s_pcm_overflow;
static volatile uint32_t s_cb_ok, s_cb_fail, s_busy_skip;
static volatile uint64_t s_send_t0, s_air_us_sum;
static volatile uint32_t s_air_us_max, s_air_n;
static SemaphoreHandle_t s_send_done;

/* Packet backlog between frame builder and radio sender */
#define TXQ_DEPTH 1                     /* hold only the newest frame: after a stall, stale frames arrive too late and delay fresh ones */
#define TXQ_SLOTS (TXQ_DEPTH + 2)       /* storage ring: queue depth + one being sent + one being built */
typedef struct { uint16_t len; link_packet_t pkt __attribute__((aligned(4))); } tx_item_t;
static tx_item_t s_items[TXQ_SLOTS];
static uint32_t s_item_idx;
static QueueHandle_t s_txq;
static volatile uint32_t s_backlog_max;

/* Receiver log lines arrive over ESP-NOW; print them from a task, not the WiFi callback */
typedef struct { uint8_t len; char txt[LINK_LOG_MAX + 1]; } rlog_t;
static QueueHandle_t s_rlogq;
static volatile uint32_t s_rlog_rx, s_other_rx;

static void espnow_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    (void)info;
    uint32_t magic;
    if (len < 6) return;
    memcpy(&magic, data, 4);
#if defined(LINK_MIC)
    if (magic == LINK_MIC_MAGIC && len == (int)sizeof(link_mic_packet_t)) {
        if (!s_mic) return;                                 /* radio starts before the USB side */
        static uint32_t last_seq;
        static bool have_seq;
        static const int16_t zeros[LINK_MIC_FRAME];
        link_mic_packet_t p;
        memcpy(&p, data, sizeof(p));
        if (have_seq) {
            int32_t gap = (int32_t)(p.seq - last_seq) - 1;
            if (gap < 0) return;                           /* duplicate / reordered: drop */
            if (gap > 0) {
                s_mic_lost += (uint32_t)gap;
                for (int32_t i = 0; i < gap && i < 4; i++) {   /* keep timing: fill short gaps with silence */
                    xStreamBufferSend(s_mic, zeros, sizeof(zeros), 0);
                }
            }
        }
        last_seq = p.seq;
        have_seq = true;
        s_mic_rx++;
        if (xStreamBufferSend(s_mic, p.pcm, sizeof(p.pcm), 0) != sizeof(p.pcm)) s_mic_ovf++;
        return;
    }
#endif
    if (magic != LINK_LOG_MAGIC) { s_other_rx++; return; }
    s_rlog_rx++;
    rlog_t m;
    m.len = (uint8_t)(len - 5 > LINK_LOG_MAX ? LINK_LOG_MAX : len - 5);
    if (m.len > data[4]) m.len = data[4];
    memcpy(m.txt, data + 5, m.len);
    m.txt[m.len] = 0;
    xQueueSend(s_rlogq, &m, 0);
}

static void rlog_task(void *arg)
{
    (void)arg;
    rlog_t m;
    while (1) {
        if (xQueueReceive(s_rlogq, &m, portMAX_DELAY)) {
            printf("[nano] %s", m.txt);
            if (m.len && m.txt[m.len - 1] != '\n') printf("\n");
        }
    }
}

/* Completion of the previous ESP-NOW send. Espressif: send the next packet only after this. */
static void espnow_send_cb(const uint8_t *mac, esp_now_send_status_t status)
{
    (void)mac;
    uint32_t dt = (uint32_t)((uint64_t)esp_timer_get_time() - s_send_t0);
    s_air_us_sum += dt;
    s_air_n++;
    if (dt > s_air_us_max) {
        s_air_us_max = dt;
    }
    if (status == ESP_NOW_SEND_SUCCESS) {
        s_cb_ok++;
    } else {
        s_cb_fail++;
    }
    xSemaphoreGive(s_send_done);
}

/* ------------------------------------------------------------------ */
/* 44.1 kHz -> 48 kHz linear resampler (only used if the host picks 44.1) */
typedef struct {
    int32_t phase;           /* Q16 position between prev and cur input sample */
    int16_t prev[2];
    bool primed;
} resampler_t;

static resampler_t s_rs;

/* in: n stereo frames at 44.1k; out: returns number of stereo frames written */
static size_t resample_44k1_to_48k(resampler_t *r, const int16_t *in, size_t n, int16_t *out, size_t out_cap)
{
    const int32_t step = (int32_t)(((uint64_t)44100 << 16) / 48000);   /* < 1.0 in Q16 */
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        const int16_t *cur = &in[2 * i];
        if (!r->primed) {
            r->prev[0] = cur[0];
            r->prev[1] = cur[1];
            r->primed = true;
            continue;
        }
        while (r->phase < 65536 && o < out_cap) {
            for (int c = 0; c < 2; c++) {
                int32_t a = r->prev[c], b = cur[c];
                out[2 * o + c] = (int16_t)(a + (((b - a) * (int32_t)r->phase) >> 16));
            }
            o++;
            r->phase += step;
        }
        r->phase -= 65536;
        r->prev[0] = cur[0];
        r->prev[1] = cur[1];
    }
    return o;
}

/* ------------------------------------------------------------------ */
static void usb_task(void *arg)
{
    (void)arg;
    static int16_t rx[CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ / 2];
    static int16_t rs[CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ];      /* room for upsampled output */

    tusb_rhport_init_t dev_init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL };
    tusb_init(0, &dev_init);

#if defined(LINK_MIC)
    static int16_t mic_in[LINK_MIC_FRAME];
    static int16_t mic_out[LINK_MIC_FRAME * 3];
    int16_t mic_prev = 0;
    int16_t gain_for = 0x7FFF;
    int32_t gain_q12 = 4096;
#endif
    while (1) {
        tud_task_ext(1, false);
#if defined(LINK_MIC)
        size_t mb;
        while ((mb = xStreamBufferReceive(s_mic, mic_in, sizeof(mic_in), 0)) >= 2) {
            if (!s_mic_active) {
                continue;                                   /* host isn't recording: discard */
            }
            if (s_mic_volume[0] != gain_for) {
                gain_for = s_mic_volume[0];
                gain_q12 = (int32_t)(powf(10.0f, (float)gain_for / 256.0f / 20.0f) * 4096.0f + 0.5f);
            }
            size_t n = mb / 2, o = 0;
            for (size_t i = 0; i < n; i++) {                /* 16 -> 48 kHz, linear interpolation */
                int32_t a = mic_prev, b = mic_in[i];
                for (int k = 1; k <= 3; k++) {
                    int32_t v = a + (b - a) * k / 3;
                    v = (v * gain_q12) >> 12;
                    if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
                    mic_out[o++] = (s_mic_mute[0] || s_mic_mute[1]) ? 0 : (int16_t)v;
                }
                mic_prev = mic_in[i];
            }
            tud_audio_write(mic_out, (uint16_t)(o * 2));
        }
#endif
        uint32_t avail;
        while ((avail = tud_audio_available()) >= 4) {
            uint32_t n = tud_audio_read(rx, avail > sizeof(rx) ? sizeof(rx) : avail);
            n &= ~3u;                                   /* whole stereo frames */
            const void *p = rx;
            size_t bytes = n;
            if (s_sample_rate == 44100) {
                size_t frames = resample_44k1_to_48k(&s_rs, rx, n / 4, rs, sizeof(rs) / 4);
                p = rs;
                bytes = frames * 4;
            }
            if (bytes && xStreamBufferSend(s_pcm, p, bytes, 0) != bytes) {
                s_pcm_overflow++;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
static void tx_task(void *arg)
{
    (void)arg;
    static link_packet_t pkt __attribute__((aligned(4)));   /* keeps pcm[] (offset 12) aligned */
    link_encoder_t enc = {0};
    link_frame_t prev_backup;
    bool have_prev = false;
    uint32_t seq = 0;
    size_t fill = 0;

    while (1) {
        size_t got = xStreamBufferReceive(s_pcm, (uint8_t *)(void *)pkt.pcm + fill, LINK_FRAME_PCM_BYTES - fill,
                                          pdMS_TO_TICKS(20));
        if (got == 0) {
            /* Host stopped streaming: drop partial frame, restart codec cleanly */
            fill = 0;
            have_prev = false;
            memset(&enc, 0, sizeof(enc));
            s_rs.primed = false;
            s_rs.phase = 0;
            continue;
        }
        fill += got;
        if (fill < LINK_FRAME_PCM_BYTES) {
            continue;
        }
        fill = 0;

        pkt.magic = LINK_MAGIC;
        pkt.version = LINK_VERSION;
        pkt.seq = seq++;
        pkt.volume_q8 = s_volume[0];
        pkt.flags = s_mute[0] ? LINK_FLAG_MUTE : 0;
        size_t len = LINK_PACKET_LEN_NO_BACKUP;
        if (have_prev) {
            pkt.backup = prev_backup;           /* ADPCM copy of frame seq-1 */
            pkt.flags |= LINK_FLAG_HAS_BACKUP;
            len = LINK_PACKET_LEN_BACKUP;
        }
        link_encode_frame(&enc, (const int16_t *)((uint8_t *)&pkt + offsetof(link_packet_t, pcm)), &prev_backup);   /* becomes the next packet's backup */
        have_prev = true;

        /* Hand the frame to the sender. Never discard it because the radio is busy:
         * after a channel-access stall the backlog goes out back-to-back (~0.75 ms each),
         * well within the receiver's 12 ms buffer. Only drop the oldest if the backlog
         * exceeds what the receiver could still use. */
        tx_item_t *it = &s_items[s_item_idx];
        s_item_idx = (s_item_idx + 1) % TXQ_SLOTS;
        it->len = (uint16_t)len;
        memcpy(&it->pkt, &pkt, len);
        if (xQueueSend(s_txq, &it, 0) != pdTRUE) {
            tx_item_t *old;
            xQueueReceive(s_txq, &old, 0);
            xQueueSend(s_txq, &it, 0);
            s_busy_skip++;
        }
    }
}

static void send_task(void *arg)
{
    (void)arg;
    while (1) {
        tx_item_t *it;
        xSemaphoreTake(s_send_done, pdMS_TO_TICKS(30));   /* previous packet off the air */
        xQueueReceive(s_txq, &it, portMAX_DELAY);          /* then take the oldest frame and send at once,
                                                              so its slot is never held while waiting */
        UBaseType_t backlog = uxQueueMessagesWaiting(s_txq) + 1;
        if (backlog > s_backlog_max) {
            s_backlog_max = backlog;
        }
        s_send_t0 = (uint64_t)esp_timer_get_time();
        if (esp_now_send(BCAST, (const uint8_t *)&it->pkt, it->len) == ESP_OK) {
            s_pkts_sent++;
        } else {
            s_pkts_fail++;
            xSemaphoreGive(s_send_done);
        }
    }
}

/* ------------------------------------------------------------------ */
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
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_channel(LINK_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_now_init());
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, BCAST, ESP_NOW_ETH_ALEN);
    peer.channel = LINK_WIFI_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    /* 802.11b 11 Mbit/s DSSS: measured ~100% delivery for 740-byte packets on this hardware,
     * vs 41-79% at 24 Mbit/s OFDM and 0% at 54 Mbit/s. ~0.73 ms on air per packet. */
    esp_now_rate_config_t rate = {
        .phymode = WIFI_PHY_MODE_11B,
        .rate = WIFI_PHY_RATE_11M_L,
        .ersu = false,
        .dcm = false,
    };
#if defined(LINK_RATE_LEGACY_API)
    (void)rate;
    ESP_ERROR_CHECK(esp_wifi_config_espnow_rate(WIFI_IF_STA, WIFI_PHY_RATE_11M_L));
#else
    ESP_ERROR_CHECK(esp_now_set_peer_rate_config(BCAST, &rate));
#endif
    ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_cb));
    s_rlogq = xQueueCreate(16, sizeof(rlog_t));
    xTaskCreatePinnedToCore(rlog_task, "rlog", 3072, NULL, 2, NULL, 0);
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));

    uint32_t ver = 0;
    esp_now_get_version(&ver);
    ESP_LOGI(TAG, "ESP-NOW v%" PRIu32 " on channel %d", ver, LINK_WIFI_CHANNEL);
}

void app_main(void)
{
    radio_init();

    s_pcm = xStreamBufferCreate(8 * LINK_FRAME_PCM_BYTES, 4);
#if defined(LINK_MIC)
    s_mic = xStreamBufferCreate(16 * sizeof(((link_mic_packet_t *)0)->pcm), 2);
#endif
    s_send_done = xSemaphoreCreateBinary();
    xSemaphoreGive(s_send_done);

    usb_phy_config_t phy = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_DEVICE,
    };
    ESP_ERROR_CHECK(usb_new_phy(&phy, &s_phy));

    xTaskCreatePinnedToCore(usb_task, "tusb", 4096, NULL, 12, NULL, 1);
    s_txq = xQueueCreate(TXQ_DEPTH, sizeof(tx_item_t *));
    xTaskCreatePinnedToCore(tx_task, "link_tx", 4096, NULL, 11, NULL, 1);
    xTaskCreatePinnedToCore(send_task, "link_send", 3072, NULL, 13, NULL, 0);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        uint32_t n = s_air_n;
        ESP_LOGI(TAG, "rate=%" PRIu32 " sent=%" PRIu32 " done ok/fail=%" PRIu32 "/%" PRIu32 " q_drop=%" PRIu32 " backlog_max=%" PRIu32
                 " send->done avg %" PRIu32 " us max %" PRIu32 " us | pcm_ovf=%" PRIu32 " vol=%d/256dB | from nano: %" PRIu32 " log, %" PRIu32 " other",
                 s_sample_rate, s_pkts_sent, s_cb_ok, s_cb_fail, s_busy_skip, s_backlog_max,
                 n ? (uint32_t)(s_air_us_sum / n) : 0, s_air_us_max, s_pcm_overflow, s_volume[0], s_rlog_rx, s_other_rx);
#if defined(LINK_MIC)
        ESP_LOGI(TAG, "mic: %s | rx %" PRIu32 " lost %" PRIu32 " ovf %" PRIu32 " | host mute %d vol %d/256dB",
                 s_mic_active ? "host recording" : "idle", s_mic_rx, s_mic_lost, s_mic_ovf,
                 s_mic_mute[0] | s_mic_mute[1], s_mic_volume[0]);
#endif
        s_air_us_max = 0;
    }
}

/* ------------------------------------------------------------------ */
/* UAC1 class requests (same handling as the PS5-verified test firmware) */

bool tud_audio_set_req_ep_cb(uint8_t rhport, tusb_control_request_t const *req, uint8_t *buf)
{
    (void)rhport;
    if (TU_U16_HIGH(req->wValue) == AUDIO10_EP_CTRL_SAMPLING_FREQ &&
        req->bRequest == AUDIO10_CS_REQ_SET_CUR) {
        TU_VERIFY(req->wLength == 3);
        uint32_t f = tu_unaligned_read32(buf) & 0x00FFFFFF;
        if (TU_U16_LOW(req->wIndex) == EPNUM_AUDIO_OUT && (f == 44100 || f == 48000)) {
            s_sample_rate = f;
        }
        return true;                                        /* mic endpoint: 48 kHz only */
    }
    return false;
}

bool tud_audio_get_req_ep_cb(uint8_t rhport, tusb_control_request_t const *req)
{
    if (TU_U16_HIGH(req->wValue) == AUDIO10_EP_CTRL_SAMPLING_FREQ &&
        req->bRequest == AUDIO10_CS_REQ_GET_CUR) {
        uint32_t f = TU_U16_LOW(req->wIndex) == EPNUM_AUDIO_OUT ? s_sample_rate : 48000;
        uint8_t b[3] = { (uint8_t)f, (uint8_t)(f >> 8), (uint8_t)(f >> 16) };
        return tud_audio_buffer_and_schedule_control_xfer(rhport, req, b, sizeof(b));
    }
    return false;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *req, uint8_t *buf)
{
    (void)rhport;
    uint8_t ch = TU_U16_LOW(req->wValue);
    uint8_t sel = TU_U16_HIGH(req->wValue);
    uint8_t ent = TU_U16_HIGH(req->wIndex);
    TU_VERIFY(req->bRequest == AUDIO10_CS_REQ_SET_CUR);
#if defined(LINK_MIC)
    if (ent == UAC1_ENTITY_MIC_FU) {
        TU_VERIFY(ch <= 1);
        if (sel == AUDIO10_FU_CTRL_MUTE) {
            TU_VERIFY(req->wLength == 1);
            s_mic_mute[ch] = buf[0];
            return true;
        }
        if (sel == AUDIO10_FU_CTRL_VOLUME) {
            TU_VERIFY(req->wLength == 2);
            int16_t v = (int16_t)tu_unaligned_read16(buf);
            s_mic_volume[ch] = v < MIC_VOL_MIN ? MIC_VOL_MIN : (v > MIC_VOL_MAX ? MIC_VOL_MAX : v);
            return true;
        }
        return false;
    }
#endif
    TU_VERIFY(ent == UAC1_ENTITY_FEATURE_UNIT);
    TU_VERIFY(ch <= CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX);
    if (sel == AUDIO10_FU_CTRL_MUTE) {
        TU_VERIFY(req->wLength == 1);
        s_mute[ch] = buf[0];
        return true;
    }
    if (sel == AUDIO10_FU_CTRL_VOLUME) {
        TU_VERIFY(req->wLength == 2);
        s_volume[ch] = (int16_t)tu_unaligned_read16(buf);
        return true;
    }
    return false;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *req)
{
    uint8_t ch = TU_U16_LOW(req->wValue);
    uint8_t sel = TU_U16_HIGH(req->wValue);
    uint8_t ent = TU_U16_HIGH(req->wIndex);
#if defined(LINK_MIC)
    if (ent == UAC1_ENTITY_MIC_FU) {
        TU_VERIFY(ch <= 1);
        if (sel == AUDIO10_FU_CTRL_MUTE) {
            return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &s_mic_mute[ch], 1);
        }
        if (sel == AUDIO10_FU_CTRL_VOLUME) {
            int16_t v;
            switch (req->bRequest) {
            case AUDIO10_CS_REQ_GET_CUR: v = s_mic_volume[ch]; break;
            case AUDIO10_CS_REQ_GET_MIN: v = MIC_VOL_MIN;      break;
            case AUDIO10_CS_REQ_GET_MAX: v = MIC_VOL_MAX;      break;
            case AUDIO10_CS_REQ_GET_RES: v = 256;              break;
            default: return false;
            }
            return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &v, sizeof(v));
        }
        return false;
    }
#endif
    TU_VERIFY(ent == UAC1_ENTITY_FEATURE_UNIT);
    TU_VERIFY(ch <= CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX);
    if (sel == AUDIO10_FU_CTRL_MUTE) {
        return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &s_mute[ch], 1);
    }
    if (sel == AUDIO10_FU_CTRL_VOLUME) {
        int16_t v;
        switch (req->bRequest) {
        case AUDIO10_CS_REQ_GET_CUR: v = s_volume[ch]; break;
        case AUDIO10_CS_REQ_GET_MIN: v = -60 * 256;    break;
        case AUDIO10_CS_REQ_GET_MAX: v = 0;            break;
        case AUDIO10_CS_REQ_GET_RES: v = 256;          break;
        default: return false;
        }
        return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &v, sizeof(v));
    }
    return false;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *req)
{
    (void)rhport;
#if defined(LINK_MIC)
    if (TU_U16_LOW(req->wIndex) == ITF_NUM_AUDIO_STREAMING_MIC) {
        s_mic_active = TU_U16_LOW(req->wValue) != 0;
    }
#else
    (void)req;
#endif
    return true;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *req)
{
    (void)rhport;
#if defined(LINK_MIC)
    if (TU_U16_LOW(req->wIndex) == ITF_NUM_AUDIO_STREAMING_MIC) {
        s_mic_active = false;
    }
#else
    (void)req;
#endif
    return true;
}
