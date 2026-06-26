#define _GNU_SOURCE
#include "engine.h"
#include "rtp.h"
#include "audio.h"
#include "modem_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

#include <sofia-sip/nua.h>
#include <sofia-sip/sip_status.h>
#include <sofia-sip/su_log.h>
#include <sofia-sip/msg_header.h>

#define FRAME           160     /* 20 ms @ 8 kHz                                */
/* Answer tone before the data carrier (answer side). 0 = straight to the
 * v22bis handshake, which is what trains cleanly modem-to-modem; a real Bell
 * 212A payphone may want the 2225 Hz tone first - tune during HW bring-up. */
#define ANSWER_TONE_MS  0

enum { EV_RING = 1, EV_CONNECT, EV_NO_CARRIER };

struct engine {
    const softmodem_config_t *cfg;

    /* sofia */
    su_root_t      *root;
    nua_t          *nua;
    nua_handle_t   *reg_nh;     /* registration handle (registrar mode) */
    su_timer_t     *timer;
    pthread_t    su_thread;
    int          su_running;
    volatile int stop;
    int          shutdown_initiated;
    int          shutdown_done;
    int          shutdown_ticks;

    /* control requests: main thread -> su thread */
    pthread_mutex_t ctl;
    int   req_answer, req_dial, req_hangup;
    char  dial_number[128];

    /* call / media state (su thread) */
    nua_handle_t   *nh;
    int             calling;
    modem_core_t   *modem;
    rtp_endpoint_t *rtp;
    codec_t         codec;
    int             media_active;
    uint32_t        tx_ts, play_ts;
    char            remote_ip[64];
    int             remote_port;

    /* events: su thread -> main thread */
    pthread_mutex_t evlock;
    int   evq[128];
    int   evhead, evtail;
    int   evpipe[2];

    pthread_mutex_t media_lock;  /* guards modem/rtp pointer lifetime */
};

/* ---- event queue ---------------------------------------------------------- */

static void push_event(engine_t *e, int code) {
    pthread_mutex_lock(&e->evlock);
    int next = (e->evhead + 1) % (int)(sizeof(e->evq) / sizeof(e->evq[0]));
    if (next != e->evtail) { e->evq[e->evhead] = code; e->evhead = next; }
    pthread_mutex_unlock(&e->evlock);
    char b = 1;
    if (write(e->evpipe[1], &b, 1) < 0) { /* pipe full is harmless */ }
}

int engine_event_fd(engine_t *e) { return e->evpipe[0]; }

void engine_dispatch_events(engine_t *e, at_engine_t *at) {
    char drain[64];
    while (read(e->evpipe[0], drain, sizeof(drain)) > 0) { }
    for (;;) {
        int code = 0;
        pthread_mutex_lock(&e->evlock);
        if (e->evtail != e->evhead) {
            code = e->evq[e->evtail];
            e->evtail = (e->evtail + 1) % (int)(sizeof(e->evq) / sizeof(e->evq[0]));
        }
        pthread_mutex_unlock(&e->evlock);
        if (!code) break;
        switch (code) {
        case EV_RING:       at_engine_on_ring(at);            break;
        case EV_CONNECT:    at_engine_on_connect(at, 1200);   break;
        case EV_NO_CARRIER: at_engine_on_no_carrier(at);      break;
        }
    }
}

/* ---- modem event (su thread) --------------------------------------------- */

static void modem_event_cb(void *ctx, int connected, int rate) {
    engine_t *e = ctx;
    (void)rate;
    push_event(e, connected ? EV_CONNECT : EV_NO_CARRIER);
}

/* ---- SDP (minimal, G.711 only) ------------------------------------------- */

/* Pick the first codec listed in cfg->codecs that the peer offered. */
static codec_t choose_codec(engine_t *e, int has_pcma, int has_pcmu) {
    const char *c = e->cfg->codecs;
    for (const char *p = c; *p; p++) {
        if (!strncasecmp(p, "PCMA", 4) && has_pcma) return CODEC_PCMA;
        if (!strncasecmp(p, "PCMU", 4) && has_pcmu) return CODEC_PCMU;
    }
    return has_pcma ? CODEC_PCMA : CODEC_PCMU;
}

/* Parse c=/m= from an SDP body; fill remote_ip/remote_port and pick codec. */
static int parse_sdp(engine_t *e, const char *data, size_t len) {
    char buf[2048];
    size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, data, n);
    buf[n] = '\0';

    int got_port = 0, has_a = 0, has_u = 0;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        if (!strncmp(line, "c=IN IP4 ", 9)) {
            sscanf(line + 9, "%63s", e->remote_ip);
        } else if (!strncmp(line, "m=audio ", 8)) {
            char pts[256] = { 0 };
            if (sscanf(line + 8, "%d RTP/AVP %255[0-9 ]", &e->remote_port, pts) >= 1)
                got_port = 1;
            /* scan the payload-type list for the static G.711 PTs */
            char *s2 = NULL;
            for (char *tok = strtok_r(pts, " ", &s2); tok; tok = strtok_r(NULL, " ", &s2)) {
                int pt = atoi(tok);
                if (pt == 8) has_a = 1;
                if (pt == 0) has_u = 1;
            }
        }
    }
    if (!got_port || !e->remote_ip[0]) return -1;
    e->codec = choose_codec(e, has_a, has_u);
    return 0;
}

static int build_sdp(engine_t *e, char *out, size_t cap, int offer) {
    const char *ip = e->cfg->media_ip;
    int port = e->cfg->media_port;
    if (offer) {
        return snprintf(out, cap,
            "v=0\r\no=- 0 0 IN IP4 %s\r\ns=softmodem\r\nc=IN IP4 %s\r\nt=0 0\r\n"
            "m=audio %d RTP/AVP 8 0\r\n"
            "a=rtpmap:8 PCMA/8000\r\na=rtpmap:0 PCMU/8000\r\n"
            "a=ptime:20\r\na=sendrecv\r\n", ip, ip, port);
    }
    int pt = (e->codec == CODEC_PCMA) ? 8 : 0;
    return snprintf(out, cap,
        "v=0\r\no=- 0 0 IN IP4 %s\r\ns=softmodem\r\nc=IN IP4 %s\r\nt=0 0\r\n"
        "m=audio %d RTP/AVP %d\r\na=rtpmap:%d %s/8000\r\n"
        "a=ptime:20\r\na=sendrecv\r\n",
        ip, ip, port, pt, pt, audio_codec_name(e->codec));
}

/* ---- media lifecycle (su thread) ----------------------------------------- */

static int open_media(engine_t *e, int calling) {
    pthread_mutex_lock(&e->media_lock);
    e->rtp = rtp_open(e->cfg->media_ip, e->cfg->media_port,
                      e->codec == CODEC_PCMA ? 8 : 0);
    if (e->rtp)
        e->modem = modem_core_create(calling, 1200,
                                     calling ? 0 : ANSWER_TONE_MS,
                                     modem_event_cb, e);
    int ok = (e->rtp && e->modem);
    pthread_mutex_unlock(&e->media_lock);
    if (!ok) { fprintf(stderr, "engine: media setup failed\n"); return -1; }
    e->calling = calling;
    e->tx_ts = e->play_ts = 0;
    return 0;
}

static void teardown_media(engine_t *e) {
    e->media_active = 0;
    pthread_mutex_lock(&e->media_lock);
    if (e->modem) { modem_core_destroy(e->modem); e->modem = NULL; }
    if (e->rtp)   { rtp_close(e->rtp);            e->rtp   = NULL; }
    pthread_mutex_unlock(&e->media_lock);
}

static void handle_answer(engine_t *e) {
    if (!e->nh || e->media_active) return;
    if (open_media(e, 0) != 0) {
        nua_respond(e->nh, SIP_500_INTERNAL_SERVER_ERROR, TAG_END());
        e->nh = NULL;
        return;
    }
    rtp_set_remote(e->rtp, e->remote_ip, e->remote_port);
    char sdp[512];
    build_sdp(e, sdp, sizeof(sdp), 0);
    nua_respond(e->nh, SIP_200_OK,
                SIPTAG_CONTENT_TYPE_STR("application/sdp"),
                SIPTAG_PAYLOAD_STR(sdp), TAG_END());
    e->media_active = 1;
    printf("engine: answered, media to %s:%d codec %s\n",
           e->remote_ip, e->remote_port, audio_codec_name(e->codec));
}

static void handle_dial(engine_t *e, const char *number) {
    if (e->nh) return;  /* already on a call */
    const char *peer = e->cfg->sip_peer[0] ? e->cfg->sip_peer : "127.0.0.1";
    char to[320];
    if (number && number[0] && !strchr(number, '@'))
        snprintf(to, sizeof(to), "sip:%s@%s", number, peer);
    else if (number && strchr(number, '@'))
        snprintf(to, sizeof(to), "sip:%s", number);
    else
        snprintf(to, sizeof(to), "sip:%s", peer);

    e->codec = choose_codec(e, 1, 1);   /* offer-side default; answer may narrow */
    if (open_media(e, 1) != 0) return;

    e->nh = nua_handle(e->nua, NULL, SIPTAG_TO_STR(to), TAG_END());
    char sdp[512];
    build_sdp(e, sdp, sizeof(sdp), 1);
    nua_invite(e->nh, SIPTAG_CONTENT_TYPE_STR("application/sdp"),
               SIPTAG_PAYLOAD_STR(sdp), TAG_END());
    printf("engine: dialling %s\n", to);
}

static void handle_hangup(engine_t *e) {
    if (e->nh) { nua_bye(e->nh, TAG_END()); e->nh = NULL; }
    teardown_media(e);
}

/* ---- digest auth (registrar / proxy challenges) -------------------------- */

static void authenticate(engine_t *e, nua_handle_t *nh, sip_t const *sip) {
    if (!sip) return;
    msg_auth_t const *au = sip->sip_www_authenticate
                         ? sip->sip_www_authenticate : sip->sip_proxy_authenticate;
    if (!au || !au->au_params) return;
    /* realm comes back already quoted, e.g. realm="asterisk" */
    msg_param_t realm = msg_params_find(au->au_params, "realm=");
    char auth[256];
    snprintf(auth, sizeof(auth), "%s:%s:%s:%s",
             au->au_scheme ? au->au_scheme : "Digest",
             realm ? realm : "\"\"", e->cfg->sip_user, e->cfg->sip_pass);
    nua_authenticate(nh, NUTAG_AUTH(auth), TAG_END());
}

/* ---- sofia callback (su thread) ------------------------------------------ */

static void nua_cb(nua_event_t event, int status, char const *phrase,
                   nua_t *nua, nua_magic_t *magic, nua_handle_t *nh,
                   nua_hmagic_t *hmagic, sip_t const *sip, tagi_t tags[]) {
    engine_t *e = (engine_t *)magic;
    (void)nua; (void)hmagic; (void)tags; (void)phrase;
    if (e->cfg->log_level >= 2)
        fprintf(stderr, "engine: nua event=%d status=%d\n", event, status);

    switch (event) {
    case nua_i_invite:
        if (e->nh) { nua_respond(nh, SIP_486_BUSY_HERE, TAG_END()); break; }
        e->nh = nh;
        if (!sip || !sip->sip_payload ||
            parse_sdp(e, sip->sip_payload->pl_data, sip->sip_payload->pl_len) != 0) {
            nua_respond(nh, SIP_488_NOT_ACCEPTABLE, TAG_END());
            e->nh = NULL;
            break;
        }
        nua_respond(nh, SIP_180_RINGING, TAG_END());
        push_event(e, EV_RING);
        break;

    case nua_r_invite:           /* response to our outbound INVITE */
        if (status == 401 || status == 407) { authenticate(e, nh, sip); break; }
        if (status >= 200 && status < 300) {
            if (sip && sip->sip_payload &&
                parse_sdp(e, sip->sip_payload->pl_data, sip->sip_payload->pl_len) == 0) {
                rtp_set_remote(e->rtp, e->remote_ip, e->remote_port);
                rtp_ep_set_payload_type(e->rtp, e->codec == CODEC_PCMA ? 8 : 0);
                e->tx_ts = e->play_ts = 0;
                e->media_active = 1;
                printf("engine: call answered, media to %s:%d codec %s\n",
                       e->remote_ip, e->remote_port, audio_codec_name(e->codec));
            }
        } else if (status >= 300) {
            teardown_media(e);
            e->nh = NULL;
            push_event(e, EV_NO_CARRIER);
        }
        break;

    case nua_i_bye:
    case nua_i_cancel:
        teardown_media(e);
        e->nh = NULL;
        push_event(e, EV_NO_CARRIER);
        break;

    case nua_r_register:
        if (status == 401 || status == 407) authenticate(e, nh, sip);
        else if (status == 200) printf("engine: registered as %s\n", e->cfg->sip_user);
        else if (status >= 300) fprintf(stderr, "engine: REGISTER failed (%d)\n", status);
        break;

    case nua_r_shutdown:
        if (status >= 200) e->shutdown_done = 1;     /* shutdown complete */
        break;

    case nua_i_state:
    default:
        break;
    }
}

/* ---- 20 ms media + control timer (su thread) ----------------------------- */

static void timer_cb(su_root_magic_t *magic, su_timer_t *t, su_timer_arg_t *arg) {
    engine_t *e = (engine_t *)arg;
    (void)magic; (void)t;

    /* drain control requests */
    int do_answer, do_dial, do_hangup;
    char num[128];
    pthread_mutex_lock(&e->ctl);
    do_answer = e->req_answer; e->req_answer = 0;
    do_dial   = e->req_dial;   e->req_dial   = 0;
    do_hangup = e->req_hangup; e->req_hangup = 0;
    snprintf(num, sizeof(num), "%s", e->dial_number);
    pthread_mutex_unlock(&e->ctl);

    if (do_hangup) handle_hangup(e);
    if (do_answer) handle_answer(e);
    if (do_dial)   handle_dial(e, num);

    /* Graceful shutdown, driven from inside the live su loop so the
     * nua_r_shutdown reply is actually delivered to nua_cb. */
    if (e->stop) {
        if (!e->shutdown_initiated) {
            e->shutdown_initiated = 1;
            if (e->nh) { nua_bye(e->nh, TAG_END()); e->nh = NULL; }
            teardown_media(e);
            nua_shutdown(e->nua);
        }
        if (e->shutdown_done || ++e->shutdown_ticks > 100) /* or ~2s timeout */
            su_root_break(e->root);
        return;
    }

    if (e->media_active && e->modem && e->rtp) {
        int16_t tx[FRAME], dec[FRAME];
        uint8_t enc[FRAME], rxb[FRAME];
        modem_core_tx_samples(e->modem, tx, FRAME);
        audio_encode(e->codec, tx, enc, FRAME);
        rtp_send(e->rtp, enc, FRAME, e->tx_ts);
        e->tx_ts += FRAME;

        int n = rtp_recv(e->rtp, rxb, FRAME, e->play_ts);
        e->play_ts += FRAME;
        if (n >= FRAME) {
            audio_decode(e->codec, rxb, dec, FRAME);
            modem_core_rx_samples(e->modem, dec, FRAME);
        } else {
            memset(dec, 0, sizeof(dec));   /* keep the modem's clock running */
            modem_core_rx_samples(e->modem, dec, FRAME);
        }
    }
}

/* ---- su thread ------------------------------------------------------------ */

static void *su_thread_fn(void *arg) {
    engine_t *e = arg;
    su_init();
    su_log_set_level(NULL, e->cfg->log_level >= 2 ? 5 : 1);

    e->root = su_root_create(e);
    char url[96];
    /* UDP only: avoids leaving TCP listeners in TIME_WAIT between restarts. */
    snprintf(url, sizeof(url), "sip:%s;transport=udp", e->cfg->sip_bind);
    e->nua = nua_create(e->root, nua_cb, e,
                        NUTAG_URL(url),
                        NUTAG_MEDIA_ENABLE(0),
                        NUTAG_AUTOACK(1),
                        TAG_END());
    if (!e->nua) {
        fprintf(stderr, "engine: nua_create failed (bind %s)\n", url);
        su_root_destroy(e->root); e->root = NULL;
        su_deinit();
        return NULL;
    }
    e->timer = su_timer_create(su_root_task(e->root), 20);
    su_timer_set_for_ever(e->timer, timer_cb, e);
    e->su_running = 1;
    printf("engine: SIP UA listening on %s\n", url);

    /* Registrar mode: REGISTER our AOR so the PBX can route calls to us. */
    if (e->cfg->sip_mode == SIP_MODE_REGISTRAR && e->cfg->sip_registrar[0]) {
        char aor[256], reg[160];
        snprintf(aor, sizeof(aor), "sip:%s@%s", e->cfg->sip_user, e->cfg->sip_registrar);
        snprintf(reg, sizeof(reg), "sip:%s", e->cfg->sip_registrar);
        e->reg_nh = nua_handle(e->nua, NULL,
                               SIPTAG_TO_STR(aor), SIPTAG_FROM_STR(aor), TAG_END());
        nua_register(e->reg_nh, NUTAG_REGISTRAR(reg), TAG_END());
        printf("engine: registering %s at %s\n", aor, reg);
    }

    /* Single run loop. The timer initiates nua_shutdown() on stop and breaks
     * the loop once nua_r_shutdown arrives (or after a ~2s timeout). */
    su_root_run(e->root);

    su_timer_destroy(e->timer); e->timer = NULL;
    if (e->shutdown_done) {
        nua_destroy(e->nua);              /* safe: shutdown completed */
    } else {
        fprintf(stderr, "engine: nua shutdown timed out; skipping destroy\n");
    }
    su_root_destroy(e->root);
    su_deinit();
    return NULL;
}

/* ---- backend (main thread) ------------------------------------------------ */

static int be_answer(void *ctx) {
    engine_t *e = ctx;
    pthread_mutex_lock(&e->ctl); e->req_answer = 1; pthread_mutex_unlock(&e->ctl);
    return 0;
}
static int be_dial(void *ctx, const char *number) {
    engine_t *e = ctx;
    pthread_mutex_lock(&e->ctl);
    e->req_dial = 1;
    snprintf(e->dial_number, sizeof(e->dial_number), "%s", number ? number : "");
    pthread_mutex_unlock(&e->ctl);
    return 0;
}
static int be_hangup(void *ctx) {
    engine_t *e = ctx;
    pthread_mutex_lock(&e->ctl); e->req_hangup = 1; pthread_mutex_unlock(&e->ctl);
    return 0;
}

at_backend_t engine_backend(engine_t *e) {
    at_backend_t be = { .ctx = e, .answer = be_answer, .dial = be_dial, .hangup = be_hangup };
    return be;
}

/* ---- DTE data proxy (main thread) ---------------------------------------- */

size_t engine_modem_send(engine_t *e, const uint8_t *data, size_t len) {
    size_t r = 0;
    pthread_mutex_lock(&e->media_lock);
    if (e->modem) r = modem_core_send(e->modem, data, len);
    pthread_mutex_unlock(&e->media_lock);
    return r;
}
size_t engine_modem_recv(engine_t *e, uint8_t *data, size_t len) {
    size_t r = 0;
    pthread_mutex_lock(&e->media_lock);
    if (e->modem) r = modem_core_recv(e->modem, data, len);
    pthread_mutex_unlock(&e->media_lock);
    return r;
}

/* ---- lifecycle ------------------------------------------------------------ */

engine_t *engine_create(const softmodem_config_t *cfg) {
    engine_t *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->cfg = cfg;
    pthread_mutex_init(&e->ctl, NULL);
    pthread_mutex_init(&e->evlock, NULL);
    pthread_mutex_init(&e->media_lock, NULL);
    if (pipe(e->evpipe) != 0) { free(e); return NULL; }
    fcntl(e->evpipe[0], F_SETFL, O_NONBLOCK);
    fcntl(e->evpipe[1], F_SETFL, O_NONBLOCK);
    return e;
}

int engine_start(engine_t *e) {
    if (pthread_create(&e->su_thread, NULL, su_thread_fn, e) != 0) return -1;
    for (int i = 0; i < 200 && !e->su_running; i++)
        usleep(5000);
    return e->su_running ? 0 : -1;
}

void engine_destroy(engine_t *e) {
    if (!e) return;
    e->stop = 1;
    if (e->su_running) pthread_join(e->su_thread, NULL);
    teardown_media(e);
    close(e->evpipe[0]);
    close(e->evpipe[1]);
    pthread_mutex_destroy(&e->ctl);
    pthread_mutex_destroy(&e->evlock);
    pthread_mutex_destroy(&e->media_lock);
    free(e);
}
