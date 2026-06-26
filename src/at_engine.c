#include "at_engine.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* ---- result codes --------------------------------------------------------- */
enum { RC_OK = 0, RC_CONNECT, RC_RING, RC_NO_CARRIER, RC_ERROR, RC_NO_DIALTONE, RC_BUSY };

static const char *rc_word[] = {
    "OK", "CONNECT", "RING", "NO CARRIER", "ERROR", "NO DIALTONE", "BUSY"
};

static void at_out(at_engine_t *e, const char *s) {
    vtty_write(e->vtty, s, strlen(s));
}

static void at_result(at_engine_t *e, int rc) {
    if (e->quiet) return;
    char buf[32];
    if (e->verbose)
        snprintf(buf, sizeof(buf), "\r\n%s\r\n", rc_word[rc]);
    else
        snprintf(buf, sizeof(buf), "%d\r", rc);
    at_out(e, buf);
}

/* CONNECT with a rate suffix (CONNECT 1200), as real modems emit. */
static void at_result_connect(at_engine_t *e, int rate) {
    if (e->quiet) return;
    char buf[40];
    if (e->verbose)
        snprintf(buf, sizeof(buf), "\r\nCONNECT %d\r\n", rate);
    else
        snprintf(buf, sizeof(buf), "%d\r", RC_CONNECT);
    at_out(e, buf);
}

void at_engine_init(at_engine_t *e, vtty_t *vtty, const at_backend_t *be) {
    memset(e, 0, sizeof(*e));
    e->vtty    = vtty;
    e->be      = *be;
    e->echo    = 1;
    e->verbose = 1;
    e->quiet   = 0;
    e->mode    = MODE_COMMAND;
    e->call    = CALL_IDLE;
    e->S[0]    = 0;   /* auto-answer off until DTE sets it */
    e->S[7]    = 50;  /* wait-for-carrier seconds          */
}

int at_engine_is_online(const at_engine_t *e) {
    return e->mode == MODE_ONLINE && e->call == CALL_CONNECTED;
}

/* ---- command-line parsing ------------------------------------------------- */

static int read_num(const char **p) {
    int n = 0; int any = 0;
    while (isdigit((unsigned char)**p)) { n = n * 10 + (**p - '0'); (*p)++; any = 1; }
    return any ? n : -1;
}

/* Parse one full command line (already stripped of the trailing CR). Returns
 * the result code to emit, or -1 if the command produced its own async result
 * (ATA / ATD) and no immediate code should be sent. */
static int parse_line(at_engine_t *e, const char *line) {
    const char *p = line;

    /* Skip optional leading "AT"/"at". A bare empty line is OK. */
    while (*p == ' ') p++;
    if (*p == '\0') return RC_OK;
    if ((p[0] == 'A' || p[0] == 'a') && (p[1] == 'T' || p[1] == 't')) {
        p += 2;
    } else {
        return RC_ERROR;
    }

    int async = 0;

    while (*p) {
        if (*p == ' ') { p++; continue; }
        char c = toupper((unsigned char)*p++);
        if (*p == '=') p++;            /* tolerate "E=1", "+MS=..." style */

        switch (c) {
        case 'E': { int n = read_num(&p); e->echo    = (n != 0); break; }
        case 'Q': { int n = read_num(&p); e->quiet   = (n != 0); break; }
        case 'V': { int n = read_num(&p); e->verbose = (n != 0); break; }
        case 'M': case 'L': case 'X': case 'W': case 'N':
        case 'C': case 'B': case 'P': case 'T':
            (void)read_num(&p);        /* speaker/result/dial-option: accept */
            break;
        case 'Z':                       /* reset                            */
            (void)read_num(&p);
            e->echo = 1; e->verbose = 1; e->quiet = 0;
            if (e->call != CALL_IDLE && e->be.hangup) e->be.hangup(e->be.ctx);
            e->call = CALL_IDLE; e->mode = MODE_COMMAND;
            break;
        case 'S': {                     /* S-register read/write            */
            int reg = read_num(&p);
            if (reg < 0 || reg > 255) return RC_ERROR;
            if (*p == '?') { p++;
                char buf[16]; snprintf(buf, sizeof(buf), "\r\n%03d\r\n", e->S[reg]);
                at_out(e, buf);
            } else {
                if (*p == '=') p++;     /* "S0=1": consume '=' after reg num */
                int val = read_num(&p);
                if (val >= 0) e->S[reg] = (uint8_t)val;
            }
            break;
        }
        case '&': {                     /* extended: &F &D &C &K ...        */
            char c2 = toupper((unsigned char)*p++);
            (void)read_num(&p);
            if (c2 == 'F') { e->echo = 1; e->verbose = 1; e->quiet = 0; }
            break;
        }
        case '+': {                     /* +MS=, +FCLASS=, ... accept       */
            while (*p && *p != ' ') p++;
            break;
        }
        case 'I': {                     /* identity                         */
            (void)read_num(&p);
            at_out(e, "\r\nsoftmodem (Bell 212A / V.22 virtual SIP modem)\r\n");
            break;
        }
        case 'A':                       /* answer                           */
            if (e->be.answer && e->be.answer(e->be.ctx) == 0) {
                e->call = CALL_CONNECTING;
                async = 1;
            } else return RC_ERROR;
            break;
        case 'H': {                     /* hang up                          */
            (void)read_num(&p);
            if (e->be.hangup) e->be.hangup(e->be.ctx);
            e->call = CALL_IDLE; e->mode = MODE_COMMAND;
            break;
        }
        case 'O':                       /* return to online data mode       */
            (void)read_num(&p);
            if (e->call == CALL_CONNECTED) { e->mode = MODE_ONLINE; return -1; }
            return RC_ERROR;
        case 'D': {                     /* dial: rest of line is the number */
            char number[128]; size_t n = 0;
            while (*p && n < sizeof(number) - 1) {
                char d = *p++;
                if (d == ' ' || d == '-' || d == 'T' || d == 'P' || d == ',') continue;
                number[n++] = d;
            }
            number[n] = '\0';
            if (e->be.dial && e->be.dial(e->be.ctx, number) == 0) {
                e->call = CALL_CONNECTING;
                async = 1;
            } else return RC_ERROR;
            /* dial consumes the remainder of the line */
            return async ? -1 : RC_ERROR;
        }
        default:
            return RC_ERROR;
        }
    }
    return async ? -1 : RC_OK;
}

/* ---- byte ingress --------------------------------------------------------- */

int at_engine_dte_byte(at_engine_t *e, uint8_t byte, long now_ms) {
    if (e->mode == MODE_ONLINE) {
        /* +++ escape: three '+' bracketed by ~1s of guard time. */
        if (byte == '+') {
            if (e->plus_count == 0 && (now_ms - e->last_rx_ms) < 1000) {
                /* no leading guard time -> not an escape; forward */
                e->last_rx_ms = now_ms;
                return 1;
            }
            e->plus_count++;
            e->last_plus_ms = now_ms;
            e->last_rx_ms = now_ms;
            if (e->plus_count >= 3) return 0; /* swallow, wait for trailing guard */
            return 0;
        }
        e->plus_count = 0;
        e->last_rx_ms = now_ms;
        return 1; /* forward to line */
    }

    /* command mode: echo + line assembly */
    if (e->echo) vtty_write(e->vtty, &byte, 1);

    if (byte == '\r' || byte == '\n') {
        e->line[e->line_len] = '\0';
        int rc = parse_line(e, e->line);
        e->line_len = 0;
        if (rc >= 0) at_result(e, rc);
    } else if (byte == 0x08 || byte == 0x7f) { /* backspace */
        if (e->line_len > 0) e->line_len--;
    } else if (e->line_len < sizeof(e->line) - 1) {
        e->line[e->line_len++] = (char)byte;
    }
    return 0;
}

void at_engine_tick(at_engine_t *e, long now_ms) {
    /* Complete a pending +++ escape once the trailing guard time elapses. */
    if (e->mode == MODE_ONLINE && e->plus_count >= 3 &&
        (now_ms - e->last_plus_ms) >= 1000) {
        e->plus_count = 0;
        e->mode = MODE_COMMAND;
        at_result(e, RC_OK);
    }
}

/* ---- backend events ------------------------------------------------------- */

void at_engine_on_ring(at_engine_t *e) {
    if (e->call == CALL_IDLE) { e->call = CALL_RINGING; e->ring_count = 0; }
    if (e->call != CALL_RINGING) return;
    e->ring_count++;
    at_result(e, RC_RING);

    /* Auto-answer when S0 rings reached. */
    if (e->S[0] != 0 && e->ring_count >= e->S[0]) {
        if (e->be.answer && e->be.answer(e->be.ctx) == 0)
            e->call = CALL_CONNECTING;
    }
}

void at_engine_on_connect(at_engine_t *e, int rate_bps) {
    e->call = CALL_CONNECTED;
    e->mode = MODE_ONLINE;
    at_result_connect(e, rate_bps);
}

void at_engine_on_no_carrier(at_engine_t *e) {
    if (e->call == CALL_IDLE) return;
    e->call = CALL_IDLE;
    e->mode = MODE_COMMAND;
    e->plus_count = 0;
    at_result(e, RC_NO_CARRIER);
}
