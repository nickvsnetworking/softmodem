/*
 * Orchestrator: ties the SIP signalling (sofia-sip), the RTP media transport
 * (oRTP) and the soft modem (spandsp) together, and exposes an at_backend_t so
 * the AT engine drives calls without knowing any of it exists.
 *
 * Threading:
 *   - one sofia "su" reactor thread runs SIP events AND a 20 ms media timer
 *     that pumps RTP<->modem samples (the modem byte FIFOs are mutex-safe, so
 *     the DTE data path needs no extra locking);
 *   - the main thread owns the PTY + AT engine and consumes events the su
 *     thread posts through a self-pipe.
 */
#ifndef SOFTMODEM_ENGINE_H_
#define SOFTMODEM_ENGINE_H_

#include "config.h"
#include "at_engine.h"

typedef struct engine engine_t;

engine_t *engine_create(const softmodem_config_t *cfg);
int       engine_start(engine_t *e);      /* spins up the sofia su thread */
void      engine_destroy(engine_t *e);

/* Backend handed to at_engine_init(). */
at_backend_t engine_backend(engine_t *e);

/* Poll fd that becomes readable when the su thread posts a call event;
 * add it to the main loop's poll set. */
int  engine_event_fd(engine_t *e);

/* Drain queued call events and deliver them to the AT engine
 * (RING / CONNECT / NO CARRIER). Call from the main thread. */
void engine_dispatch_events(engine_t *e, at_engine_t *at);

/* DTE data path (main thread): bytes to/from the connected modem. */
size_t engine_modem_send(engine_t *e, const uint8_t *data, size_t len);
size_t engine_modem_recv(engine_t *e, uint8_t *data, size_t len);

#endif /* SOFTMODEM_ENGINE_H_ */
