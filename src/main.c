/*
 * softmodem - a virtual SIP modem that presents a serial port + Hayes modem to
 * any software, while carrying the modem signal as G.711 RTP over SIP.
 *
 * Milestone 1: skeleton + virtual tty + AT engine, exercised by an in-process
 * loopback backend. SIP / spandsp DSP / RTP land in later milestones behind the
 * same at_backend_t interface.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <time.h>

#include "config.h"
#include "vtty.h"
#include "at_engine.h"
#ifdef WITH_DSP
#include "modem_core.h"
#include "audio.h"
#endif
#ifdef WITH_RTP
#include "rtp.h"
#endif

static volatile sig_atomic_t g_running = 1;
static void on_signal(int s) { (void)s; g_running = 0; }

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- milestone-1 loopback backend ----------------------------------------
 * Stands in for the real SIP+DSP path: "answering" or "dialling" simply leads
 * to a CONNECT a moment later, and online data is echoed back to the DTE. This
 * makes the AT engine and PTY fully exercisable with no hardware. */
typedef struct {
    at_engine_t *eng;
    int   connect_pending;
    long  connect_at_ms;
} loop_backend_t;

static int lb_answer(void *ctx) {
    loop_backend_t *lb = ctx;
    printf("[backend] answering -> CONNECT in 200ms\n");
    lb->connect_pending = 1;
    lb->connect_at_ms = now_ms() + 200;
    return 0;
}
static int lb_dial(void *ctx, const char *number) {
    loop_backend_t *lb = ctx;
    printf("[backend] dialling '%s' -> CONNECT in 200ms\n", number);
    lb->connect_pending = 1;
    lb->connect_at_ms = now_ms() + 200;
    return 0;
}
static int lb_hangup(void *ctx) {
    loop_backend_t *lb = ctx;
    printf("[backend] hangup\n");
    lb->connect_pending = 0;
    return 0;
}

/* ---- stdin operator console ----------------------------------------------
 * Lets you drive call events by hand during bring-up:
 *   ring | connect | bye | status | quit */
static void handle_console(at_engine_t *eng, loop_backend_t *lb) {
    char buf[64];
    long n = read(STDIN_FILENO, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = '\0';
    char *nl = strpbrk(buf, "\r\n"); if (nl) *nl = '\0';

    if      (!strcmp(buf, "ring"))    at_engine_on_ring(eng);
    else if (!strcmp(buf, "connect")) at_engine_on_connect(eng, 1200);
    else if (!strcmp(buf, "bye"))     { lb->connect_pending = 0; at_engine_on_no_carrier(eng); }
    else if (!strcmp(buf, "status"))  printf("[status] call=%d mode=%d S0=%d echo=%d\n",
                                             eng->call, eng->mode, eng->S[0], eng->echo);
    else if (!strcmp(buf, "quit"))    g_running = 0;
    else if (buf[0])                  printf("[console] commands: ring connect bye status quit\n");
}

#ifdef WITH_DSP
/* ---- milestone-2 DSP self-test -------------------------------------------
 * Cross-connect a caller and an answerer modem through a G.711 round-trip,
 * train them, then push a known string each way and verify it survives. No
 * SIP, no RTP, no hardware - just the spandsp v22bis data path. */
#define FRAME 160  /* 20 ms @ 8 kHz */

static void st_event(void *ctx, int connected, int rate) {
    int *flag = ctx;
    *flag = connected;
    printf("[selftest] %s (%d bps)\n", connected ? "CONNECTED" : "carrier down", rate);
}

/* Move one 20 ms frame from src to dst through codec enc/dec. */
static void st_pump(modem_core_t *src, modem_core_t *dst, codec_t codec) {
    int16_t pcm[FRAME];
    uint8_t g711[FRAME];
    int16_t back[FRAME];
    modem_core_tx_samples(src, pcm, FRAME);
    audio_encode(codec, pcm, g711, FRAME);
    audio_decode(codec, g711, back, FRAME);
    modem_core_rx_samples(dst, back, FRAME);
}

static int run_selftest(void) {
    codec_t codec = CODEC_PCMA;
    int a_conn = 0, b_conn = 0;
    /* No answer tone here - we want a clean v22bis<->v22bis handshake. */
    modem_core_t *ans  = modem_core_create(0 /*answer*/, 1200, 0, st_event, &a_conn);
    modem_core_t *call = modem_core_create(1 /*caller*/, 1200, 0, st_event, &b_conn);
    if (!ans || !call) { fprintf(stderr, "selftest: modem create failed\n"); return 1; }

    printf("[selftest] training...\n");
    int trained_at = -1;
    for (int i = 0; i < 4000; i++) {       /* up to 80 s of audio */
        st_pump(call, ans,  codec);        /* caller -> answerer */
        st_pump(ans,  call, codec);        /* answerer -> caller */
        if (a_conn && b_conn) { trained_at = i; break; }
    }
    if (!(a_conn && b_conn)) {
        fprintf(stderr, "[selftest] FAIL: modems did not train\n");
        return 1;
    }
    printf("[selftest] both trained after %d frames (%d ms of audio)\n",
           trained_at, trained_at * 20);

    /* Let the link settle, then send a known message caller -> answerer. */
    const char *msg = "The quick brown fox 0123456789!\r\n";
    modem_core_send(call, (const uint8_t *)msg, strlen(msg));

    char got[256]; size_t n = 0;
    for (int i = 0; i < 400 && n < strlen(msg) + 4; i++) {
        st_pump(call, ans, codec);
        st_pump(ans, call, codec);
        n += modem_core_recv(ans, (uint8_t *)got + n, sizeof(got) - 1 - n);
    }
    got[n] = '\0';

    /* The payload must arrive intact and contiguous. A real modem can emit a
     * stray byte at the connect boundary, so we look for the message as a
     * substring rather than demanding a pristine stream (the upper-layer
     * packet protocol in mm_manager has framing/CRC for exactly this). */
    int ok = (memmem(got, n, msg, strlen(msg)) != NULL);
    size_t extra = n > strlen(msg) ? n - strlen(msg) : 0;
    printf("[selftest] received %zu bytes (payload intact=%s, connect-artifact bytes=%zu)\n",
           n, ok ? "yes" : "NO", extra);
    if (!ok) printf("[selftest] got: %.*s\n", (int)n, got);

    /* Idle quietness: with no DTE data, the line must not spew bytes at the
     * peer (otherwise a real session would drown in garbage between packets). */
    (void)modem_core_recv(ans,  (uint8_t *)got, sizeof(got)); /* flush */
    (void)modem_core_recv(call, (uint8_t *)got, sizeof(got));
    size_t idle_bytes = 0;
    for (int i = 0; i < 500; i++) {           /* 10 s of idle audio */
        st_pump(call, ans, codec);
        st_pump(ans, call, codec);
        size_t r = modem_core_recv(ans,  (uint8_t *)got, sizeof(got));
        if (idle_bytes == 0 && r > 0) {
            printf("[selftest] idle byte sample:");
            for (size_t k = 0; k < r && k < 8; k++) printf(" %02x", (uint8_t)got[k]);
            printf("\n");
        }
        idle_bytes += r;
        idle_bytes += modem_core_recv(call, (uint8_t *)got, sizeof(got));
    }
    int idle_ok = (idle_bytes == 0);
    printf("[selftest] idle quietness over 10s: %zu stray bytes (%s)\n",
           idle_bytes, idle_ok ? "clean" : "NOISY");

    modem_core_destroy(ans);
    modem_core_destroy(call);
    ok = ok && idle_ok;
    printf("[selftest] %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
#endif /* WITH_DSP */

#ifdef WITH_RTP
/* ---- milestone-3 RTP transport test --------------------------------------
 * Two endpoints on localhost exchange paced 20 ms G.711 frames; verify the
 * payloads arrive intact and in order. Exercises rtp.c without the modem. */
static int run_rtptest(void) {
    const int N = 50, LEN = 160;
    rtp_global_init(1);
    rtp_endpoint_t *a = rtp_open("127.0.0.1", 40100, 8 /*PCMA*/);
    rtp_endpoint_t *b = rtp_open("127.0.0.1", 40102, 8);
    if (!a || !b) { fprintf(stderr, "rtptest: open failed\n"); return 1; }
    rtp_set_remote(a, "127.0.0.1", 40102);
    rtp_set_remote(b, "127.0.0.1", 40100);

    int received = 0, corrupt = 0, disordered = 0, prev = -1;
    uint8_t frame[LEN], rx[LEN];
    /* send_ts advances per transmitted frame; play_ts is an independent
     * monotonic playout clock so the jitter buffer can release frames. Each
     * frame k is filled with the constant byte k+1, so we can check both
     * intra-frame integrity and inter-frame ordering without caring about the
     * jitter buffer's absolute timestamp offset. */
    uint32_t send_ts = 0, play_ts = 0;
    for (int i = 0; i < N + 8; i++) {        /* a few extra ticks to drain */
        if (i < N) {
            memset(frame, (uint8_t)((i + 1) & 0xFF), LEN);
            rtp_send(a, frame, LEN, send_ts);
            send_ts += LEN;
        }
        nanosleep((const struct timespec[]){{0, 20 * 1000000L}}, NULL);

        int n = rtp_recv(b, rx, LEN, play_ts);
        if (n > 0) {
            uint8_t val = rx[0];
            for (int k = 1; k < n; k++) if (rx[k] != val) { corrupt++; break; }
            if (prev >= 0 && val != (uint8_t)((prev + 1) & 0xFF)) disordered++;
            prev = val;
            received++;
        }
        play_ts += LEN;
    }

    printf("[rtptest] sent %d frames, received %d, corrupt %d, out-of-order %d\n",
           N, received, corrupt, disordered);
    int mismatched = corrupt + disordered;
    rtp_close(a); rtp_close(b); rtp_global_exit();
    int ok = (received >= N - 2) && (mismatched == 0);  /* allow tail slack */
    printf("[rtptest] %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
#endif /* WITH_RTP */

int main(int argc, char **argv) {
#ifdef WITH_DSP
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--selftest")) return run_selftest();
#endif
#ifdef WITH_RTP
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--rtptest")) return run_rtptest();
#endif
    softmodem_config_t cfg;
    config_defaults(&cfg);
    int rc = config_parse_args(&cfg, argc, argv);
    if (rc > 0) return 0;       /* --help */
    if (rc < 0) return 2;
    config_dump(&cfg);

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    vtty_t vtty;
    if (vtty_open(&vtty, cfg.tty_device) != 0) {
        fprintf(stderr, "failed to open virtual tty\n");
        return 1;
    }

    loop_backend_t lb = { 0 };
    at_backend_t be = { .ctx = &lb, .answer = lb_answer, .dial = lb_dial, .hangup = lb_hangup };
    at_engine_t eng;
    at_engine_init(&eng, &vtty, &be);
    lb.eng = &eng;

    printf("softmodem ready. Console: ring | connect | bye | status | quit\n");

    struct pollfd fds[2];
    fds[0].fd = vtty.master_fd; fds[0].events = POLLIN;
    fds[1].fd = STDIN_FILENO;   fds[1].events = POLLIN;

    while (g_running) {
        int pr = poll(fds, 2, 50);
        long t = now_ms();

        if (pr > 0 && (fds[0].revents & POLLIN)) {
            uint8_t buf[256];
            long n = vtty_read(&vtty, buf, sizeof(buf));
            for (long i = 0; i < n; i++) {
                int forward = at_engine_dte_byte(&eng, buf[i], t);
                if (forward && at_engine_is_online(&eng)) {
                    /* loopback: echo data back to the DTE */
                    vtty_write(&vtty, &buf[i], 1);
                }
            }
        }
        if (pr > 0 && (fds[1].revents & POLLIN)) {
            handle_console(&eng, &lb);
        }

        at_engine_tick(&eng, t);

        /* fire the loopback "connect" once its timer expires */
        if (lb.connect_pending && t >= lb.connect_at_ms) {
            lb.connect_pending = 0;
            at_engine_on_connect(&eng, 1200);
        }
    }

    printf("\nshutting down\n");
    vtty_close(&vtty);
    return 0;
}
