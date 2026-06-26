/*
 * Soft Bell 212A / V.22 modem (1200 bps) built on spandsp's v22bis modem with
 * spandsp async<->sync converters for the 8N1 DTE byte stream.
 *
 * The DTE side is two byte FIFOs (send = DTE->line, recv = line->DTE). The line
 * side is 8 kHz linear16 samples that the media layer carries as G.711/RTP.
 *
 *   DTE bytes --send--> [tx fifo] --get_byte--> async_tx --get_bit--> v22bis_tx --> samples
 *   samples --> v22bis_rx --put_bit--> async_rx --put_byte--> [rx fifo] --recv--> DTE bytes
 *
 * Training success / carrier loss are reported via the event callback (invoked
 * from whatever thread calls modem_core_rx_samples()).
 */
#ifndef SOFTMODEM_MODEM_CORE_H_
#define SOFTMODEM_MODEM_CORE_H_

#include <stdint.h>
#include <stddef.h>

typedef struct modem_core modem_core_t;

/* connected = 1 on training success, 0 on carrier loss / training failure. */
typedef void (*modem_event_fn)(void *ctx, int connected, int rate_bps);

/* calling_party: 1 = we placed the call (caller), 0 = we answered.
 * answer_tone_ms: on the answering side, play the Bell answer tone (2225 Hz)
 *   for this many ms before starting the data carrier (0 = none). Ignored when
 *   calling_party = 1. */
modem_core_t *modem_core_create(int calling_party, int bit_rate,
                                int answer_tone_ms,
                                modem_event_fn on_event, void *ctx);
void          modem_core_destroy(modem_core_t *m);

/* DTE <-> modem byte streams (thread-safe). Return count actually moved. */
size_t modem_core_send(modem_core_t *m, const uint8_t *data, size_t len);
size_t modem_core_recv(modem_core_t *m, uint8_t *data, size_t len);

/* Media path: fill n samples to transmit / consume n received samples. */
void   modem_core_tx_samples(modem_core_t *m, int16_t *out, int n);
void   modem_core_rx_samples(modem_core_t *m, const int16_t *in, int n);

int    modem_core_is_connected(modem_core_t *m);

#endif /* SOFTMODEM_MODEM_CORE_H_ */
