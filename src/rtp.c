#include "rtp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ortp/ortp.h>

struct rtp_endpoint {
    RtpSession *session;
    int         local_port;
};

void rtp_global_init(int log_level) {
    ortp_init();
    ortp_scheduler_init();
    ortp_set_log_level_mask(NULL, log_level >= 2 ? ORTP_MESSAGE | ORTP_WARNING | ORTP_ERROR
                                                 : ORTP_ERROR | ORTP_FATAL);
}

void rtp_global_exit(void) {
    ortp_exit();
}

rtp_endpoint_t *rtp_open(const char *local_ip, int local_port, int payload_type) {
    rtp_endpoint_t *e = calloc(1, sizeof(*e));
    if (!e) return NULL;

    e->session = rtp_session_new(RTP_SESSION_SENDRECV);
    if (!e->session) { free(e); return NULL; }

    /* We pace the stream ourselves: no oRTP scheduler, non-blocking I/O. */
    rtp_session_set_scheduling_mode(e->session, 0);
    rtp_session_set_blocking_mode(e->session, 0);
    rtp_session_set_symmetric_rtp(e->session, TRUE);

    if (rtp_session_set_local_addr(e->session, local_ip ? local_ip : "0.0.0.0",
                                   local_port, local_port + 1) < 0) {
        fprintf(stderr, "rtp: bind %s:%d failed\n", local_ip, local_port);
        rtp_session_destroy(e->session);
        free(e);
        return NULL;
    }
    rtp_session_set_payload_type(e->session, payload_type);
    /* Jitter buffer: small and fixed - modems hate latency, and we run on-LAN. */
    rtp_session_enable_adaptive_jitter_compensation(e->session, FALSE);
    rtp_session_set_jitter_compensation(e->session, 40 /* ms */);

    e->local_port = local_port;
    return e;
}

int rtp_set_remote(rtp_endpoint_t *e, const char *ip, int port) {
    return rtp_session_set_remote_addr(e->session, ip, port);
}

void rtp_ep_set_payload_type(rtp_endpoint_t *e, int pt) {
    rtp_session_set_payload_type(e->session, pt);
}

int rtp_send(rtp_endpoint_t *e, const uint8_t *payload, int len, uint32_t ts) {
    return rtp_session_send_with_ts(e->session, payload, len, ts);
}

int rtp_recv(rtp_endpoint_t *e, uint8_t *payload, int maxlen, uint32_t ts) {
    int have_more = 0;
    int n = rtp_session_recv_with_ts(e->session, payload, maxlen, ts, &have_more);
    return n;
}

int rtp_local_port(const rtp_endpoint_t *e) {
    return e->local_port;
}

void rtp_close(rtp_endpoint_t *e) {
    if (!e) return;
    if (e->session) rtp_session_destroy(e->session);
    free(e);
}
