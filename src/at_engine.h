/*
 * Hayes "AT" command engine + modem call state machine.
 *
 * It is deliberately decoupled from how calls are actually carried: it talks to
 * a backend (answer / dial / hangup) via function pointers, and receives
 * asynchronous call events (ring / connect / carrier-loss) from that backend.
 * In milestone 1 the backend is an in-process loopback; later it becomes the
 * SIP + spandsp media path. The engine never knows the difference.
 */
#ifndef SOFTMODEM_AT_ENGINE_H_
#define SOFTMODEM_AT_ENGINE_H_

#include <stdint.h>
#include <stddef.h>

#include "vtty.h"

typedef enum {
    CALL_IDLE = 0,
    CALL_RINGING,
    CALL_CONNECTING,
    CALL_CONNECTED
} call_state_t;

typedef enum {
    MODE_COMMAND = 0,   /* parsing AT commands              */
    MODE_ONLINE         /* passing data to/from the line    */
} at_mode_t;

/* Backend the engine drives to actually start/stop calls. Each returns 0 on
 * success. answer()/dial() are expected to lead (asynchronously) to an
 * at_engine_on_connect() or at_engine_on_no_carrier() event. */
typedef struct {
    void *ctx;
    int (*answer)(void *ctx);
    int (*dial)(void *ctx, const char *number);
    int (*hangup)(void *ctx);
} at_backend_t;

typedef struct {
    vtty_t      *vtty;
    at_backend_t be;

    uint8_t      S[256];      /* S-registers (S0 = auto-answer ring count)   */
    int          echo;        /* ATE                                          */
    int          verbose;     /* ATV (1 = word results, 0 = numeric)          */
    int          quiet;       /* ATQ (1 = suppress result codes)              */

    at_mode_t    mode;
    call_state_t call;
    int          ring_count;  /* rings delivered for the current inbound call */

    char         line[256];   /* command line accumulator                     */
    size_t       line_len;

    /* +++ escape detection (online -> command) */
    int          plus_count;
    long         last_rx_ms;
    long         last_plus_ms;
} at_engine_t;

void at_engine_init(at_engine_t *e, vtty_t *vtty, const at_backend_t *be);

/* Feed a byte that arrived from the DTE. In command mode it is parsed; in
 * online mode it is checked for the +++ escape and otherwise returned to the
 * caller (via *forward) to be sent to the modem TX. Returns 1 if the byte
 * should be forwarded to the line, 0 if consumed by the engine. */
int  at_engine_dte_byte(at_engine_t *e, uint8_t byte, long now_ms);

/* Periodic tick (drives the +++ guard timer and ring cadence). */
void at_engine_tick(at_engine_t *e, long now_ms);

/* Asynchronous events from the backend. */
void at_engine_on_ring(at_engine_t *e);        /* inbound call alerting       */
void at_engine_on_connect(at_engine_t *e, int rate_bps);
void at_engine_on_no_carrier(at_engine_t *e);

/* True when data bytes from the line should be delivered to the DTE. */
int  at_engine_is_online(const at_engine_t *e);

#endif /* SOFTMODEM_AT_ENGINE_H_ */
