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

int main(int argc, char **argv) {
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
