/*
 * Host-side tests for the link:
 *  1. IMA-ADPCM frame codec quality (SNR) on test signals.
 *  2. End-to-end jitter-buffer simulation: TX/RX clock offset, packet loss,
 *     network jitter and bursts. Reports latency, loss recovery and drift control.
 *
 * Build: gcc -O2 -I../common/linkproto/include -I../rx/main sim.c ../rx/main/jbuf.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "link_codec.h"
#include "jbuf.h"

#define FS LINK_SAMPLE_RATE
#define FR LINK_FRAME_SAMPLES

/* ---------------- deterministic RNG ---------------- */
static uint64_t rng = 0x12345678abcdefULL;
static double urand(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (double)(rng >> 11) / (double)(1ULL << 53);
}
static double erand(double mean) { return -mean * log(1.0 - urand()); }

/* ---------------- 1. codec quality ---------------- */
typedef double (*sigfn)(long n, int ch);
static double sig_sine1k_m6(long n, int ch)  { (void)ch; return 0.5 * sin(2 * M_PI * 1000.0 * n / FS); }
static double sig_sine10k_m6(long n, int ch) { (void)ch; return 0.5 * sin(2 * M_PI * 10000.0 * n / FS); }
static double sig_sine1k_m40(long n, int ch) { (void)ch; return 0.01 * sin(2 * M_PI * 1000.0 * n / FS); }
static double sig_multi(long n, int ch)
{
    double t = (double)n / FS;
    return 0.15 * sin(2 * M_PI * 110 * t + ch) + 0.12 * sin(2 * M_PI * 440 * t) +
           0.08 * sin(2 * M_PI * 1760 * t + 0.3 * ch) + 0.05 * sin(2 * M_PI * 5200 * t) +
           0.03 * sin(2 * M_PI * 9100 * t);
}
static double sig_noise(long n, int ch) { (void)n; (void)ch; return 0.3 * (2 * urand() - 1); }

static void codec_test(const char *name, sigfn f)
{
    link_encoder_t enc = {0};
    double se = 0, sn = 0;
    int16_t in[FR * 2], outp[FR * 2];
    link_frame_t fr;
    for (int k = 0; k < 500; k++) {                          /* 2 s */
        for (int i = 0; i < FR; i++) {
            for (int c = 0; c < 2; c++) {
                double v = f((long)k * FR + i, c);
                in[i * 2 + c] = (int16_t)lrint(v * 32767.0);
            }
        }
        link_encode_frame(&enc, in, &fr);
        link_decode_frame(&fr, outp);
        if (k < 5) continue;                                /* skip adaptation start */
        for (int i = 0; i < FR * 2; i++) {
            double e = (double)outp[i] - in[i];
            se += (double)in[i] * in[i];
            sn += e * e;
        }
    }
    printf("  %-26s SNR %5.1f dB\n", name, 10 * log10(se / (sn + 1e-9)));
}

/* ---------------- 2. link simulation ---------------- */
typedef struct {
    double t_arrive;
    link_packet_t pkt;
    size_t len;
} inflight_t;

static int cmp_inflight(const void *a, const void *b)
{
    double d = ((const inflight_t *)a)->t_arrive - ((const inflight_t *)b)->t_arrive;
    return d < 0 ? -1 : d > 0;
}

typedef struct {
    const char *name;
    double ppm;          /* RX clock relative to TX clock */
    double loss;         /* independent packet loss probability */
    double jitter_ms;    /* mean of exponential extra delay */
    double burst_prob;   /* per packet: chance of a long stall */
    double burst_ms;
    double seconds;
} scenario_t;

static void run(const scenario_t *sc)
{
    const int N = (int)(sc->seconds * FS / FR);
    inflight_t *q = calloc((size_t)N, sizeof(*q));
    link_encoder_t enc = {0};
    link_frame_t prev;
    int16_t pcm[FR * 2];
    double *cap_t = calloc((size_t)N, sizeof(double));

    /* TX: frame k complete at (k+1)*FR/FS seconds of TX clock (= true time) */
    for (int k = 0; k < N; k++) {
        for (int i = 0; i < FR; i++) {
            int16_t v = (int16_t)lrint(8000 * sin(2 * M_PI * 1000.0 * ((double)k * FR + i) / FS));
            pcm[i * 2] = pcm[i * 2 + 1] = v;
        }
        link_packet_t *p = &q[k].pkt;
        p->magic = LINK_MAGIC;
        p->version = LINK_VERSION;
        p->seq = (uint32_t)k;
        memcpy(p->pcm, pcm, sizeof(p->pcm));
        link_frame_t cur;
        link_encode_frame(&enc, pcm, &cur);
        if (k) p->backup = prev;
        prev = cur;
        q[k].len = k ? LINK_PACKET_LEN_BACKUP : LINK_PACKET_LEN_NO_BACKUP;
        cap_t[k] = (double)k * FR / FS;                        /* first sample of frame k */
        double send_t = (double)(k + 1) * FR / FS + 0.0003;    /* + encode/USB */
        double delay = 0.0002 + erand(sc->jitter_ms / 1000.0);
        if (urand() < sc->burst_prob) delay += sc->burst_ms / 1000.0;
        q[k].t_arrive = urand() < sc->loss ? 1e9 : send_t + delay;
    }
    qsort(q, (size_t)N, sizeof(*q), cmp_inflight);

    jbuf_t *jb = calloc(1, sizeof(jbuf_t));
    jb_init(jb);
    int16_t out[JB_OUT_MAX * 2];
    double t = 0.050;                                          /* RX starts pulling at 50 ms */
    const double rx_rate = FS * (1.0 + sc->ppm * 1e-6);
    int qi = 0;
    double lat_sum = 0, lat_max = 0, lat_min = 1e9;
    long lat_n = 0;
    double end_t = sc->seconds - 0.1;

    while (t < end_t) {
        while (qi < N && q[qi].t_arrive <= t) {
            jb_push(jb, &q[qi].pkt, q[qi].len, (uint64_t)(q[qi].t_arrive * 1e6));
            qi++;
        }
        uint32_t seq_before = jb->play_seq;
        bool was_playing = jb->playing;
        size_t n = jb_pull(jb, out, (uint64_t)(t * 1e6));
        if (was_playing || jb->playing) {
            uint32_t played = was_playing ? seq_before : jb->play_seq - 1;
            if (played < (uint32_t)N && t > 2.0) {             /* steady state */
                double lat = t - cap_t[played];
                lat_sum += lat; lat_n++;
                if (lat > lat_max) lat_max = lat;
                if (lat < lat_min) lat_min = lat;
            }
        }
        t += (double)n / rx_rate;                              /* dongle consumes at RX clock */
    }

    jb_stats_t *s = &jb->st;
    printf("  %-34s lat avg %5.1f ms [%4.1f..%4.1f]  played %6u  recovered %5u  concealed %4u (%.3f%%)  late %4u  skips %u  drop/ins %u/%u  buf %4.1f ms\n",
           sc->name, 1000 * lat_sum / lat_n, 1000 * lat_min, 1000 * lat_max, s->frames_played,
           s->frames_recovered, s->frames_concealed, 100.0 * s->frames_concealed / s->frames_played,
           s->frames_late, s->skips, s->drops, s->inserts, jb_buffered_us(jb) / 1000.0);
    free(q); free(cap_t); free(jb);
}

int main(void)
{
    printf("IMA-ADPCM backup codec (used only for frames whose own packet was lost):\n");
    codec_test("1 kHz sine, -6 dBFS", sig_sine1k_m6);
    codec_test("10 kHz sine, -6 dBFS", sig_sine10k_m6);
    codec_test("1 kHz sine, -40 dBFS", sig_sine1k_m40);
    codec_test("5-tone mix", sig_multi);
    codec_test("white noise, -10 dBFS", sig_noise);
    printf("  primary stream is raw 16-bit PCM: bit-exact\n");

    printf("\nJitter buffer (latency = capture of frame start -> start of playout, excl. USB host ring + dongle):\n");
    scenario_t sc[] = {
        {"ideal",                             0,   0.000, 0.1, 0,     0,  60},
        {"+50 ppm RX clock",                 50,   0.000, 0.3, 0,     0, 120},
        {"-50 ppm RX clock",                -50,   0.000, 0.3, 0,     0, 120},
        {"1% loss, 0.5 ms jitter",           30,   0.010, 0.5, 0,     0, 120},
        {"5% loss, 1 ms jitter",            -30,   0.050, 1.0, 0,     0, 120},
        {"1% loss + 0.5% 15 ms stalls",      20,   0.010, 0.5, 0.005, 15, 120},
        {"10% loss (bad link)",              20,   0.100, 1.0, 0,     0, 120},
    };
    for (size_t i = 0; i < sizeof(sc) / sizeof(sc[0]); i++) run(&sc[i]);
    return 0;
}
