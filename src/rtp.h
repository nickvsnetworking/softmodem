/*
 * Minimal RTP media endpoint over oRTP, carrying 8 kHz G.711 in 20 ms frames.
 *
 * We drive our own 20 ms clock (oRTP scheduler disabled) and read packets
 * non-blocking, so the modem media thread stays in control of timing.
 */
#ifndef SOFTMODEM_RTP_H_
#define SOFTMODEM_RTP_H_

#include <stdint.h>

typedef struct rtp_endpoint rtp_endpoint_t;

void rtp_global_init(int log_level);
void rtp_global_exit(void);

/* Bind local_ip:local_port; payload_type is the RTP PT (0=PCMU, 8=PCMA). */
rtp_endpoint_t *rtp_open(const char *local_ip, int local_port, int payload_type);

int  rtp_set_remote(rtp_endpoint_t *e, const char *ip, int port);
void rtp_ep_set_payload_type(rtp_endpoint_t *e, int pt);

/* Send one frame (len payload bytes) stamped at sample timestamp ts. */
int  rtp_send(rtp_endpoint_t *e, const uint8_t *payload, int len, uint32_t ts);

/* Receive one frame's worth of payload for timestamp ts. Returns bytes copied
 * (0 if nothing available). */
int  rtp_recv(rtp_endpoint_t *e, uint8_t *payload, int maxlen, uint32_t ts);

int  rtp_local_port(const rtp_endpoint_t *e);
void rtp_close(rtp_endpoint_t *e);

#endif /* SOFTMODEM_RTP_H_ */
