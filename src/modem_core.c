#include "modem_core.h"
#include "fifo.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "spandsp.h"

#define FIFO_CAP 4096

/* RX UART deframer state. */
enum { RX_IDLE = 0, RX_DATA };

struct modem_core {
    v22bis_state_t                 *v22;
    modem_connect_tones_tx_state_t *ans;   /* answer tone generator (answerer) */

    int          calling;
    int          bit_rate;
    int          ans_samples_left;         /* remaining answer-tone samples    */
    int          connected;

    fifo_t       tx;   /* DTE -> line */
    fifo_t       rx;   /* line -> DTE */

    /* TX 8N1 framer: a 10-bit frame [start=0][d0..d7][stop=1], shifted LSB first */
    uint16_t     tx_frame;
    int          tx_bitcount;

    /* RX 8N1 deframer */
    int          rx_state;
    int          rx_bitcount;
    uint8_t      rx_byte;

    modem_event_fn on_event;
    void          *ctx;

    pthread_mutex_t lock;  /* serialises v22bis_tx/rx (defensive)              */
};

/* ---- spandsp glue: our own async UART framing -----------------------------
 * spandsp's async_tx/async_rx idle semantics differ between the 0.0.6 (Ubuntu)
 * and 3.0.0 (FreeSWITCH) ABIs, so we frame 8N1 ourselves at the v22bis bit
 * boundary. These bits are the *descrambled* UART-level stream; v22bis handles
 * scrambling and the line carrier. Idle = continuous marking (1) bits, which
 * the far deframer correctly ignores - no stray bytes between packets. */

/* v22bis pulls the next bit to transmit. */
static int mc_get_bit(void *ud) {
    modem_core_t *m = ud;
    if (m->tx_bitcount == 0) {
        uint8_t b;
        if (fifo_read(&m->tx, &b, 1) != 1)
            return 1;                        /* nothing to send -> mark idle  */
        /* start bit (0) at pos0, data LSB-first at pos1..8, stop bit (1) pos9 */
        m->tx_frame    = (uint16_t)((b << 1) | (1u << 9));
        m->tx_bitcount = 10;
    }
    int bit = m->tx_frame & 1;
    m->tx_frame >>= 1;
    m->tx_bitcount--;
    return bit;
}

/* v22bis hands us demodulated bits and, as negative values, link status. */
static void mc_put_bit(void *ud, int bit) {
    modem_core_t *m = ud;
    if (bit < 0) {
        switch (bit) {
        case SIG_STATUS_TRAINING_SUCCEEDED:
            m->rx_state = RX_IDLE;
            if (!m->connected) {
                m->connected = 1;
                if (m->on_event) m->on_event(m->ctx, 1, m->bit_rate);
            }
            break;
        case SIG_STATUS_CARRIER_DOWN:
        case SIG_STATUS_TRAINING_FAILED:
            m->rx_state = RX_IDLE;
            if (m->connected) {
                m->connected = 0;
                if (m->on_event) m->on_event(m->ctx, 0, 0);
            }
            break;
        default:
            break;
        }
        return;
    }

    /* 8N1 deframe */
    bit &= 1;
    if (m->rx_state == RX_IDLE) {
        if (bit == 0) {                      /* start bit */
            m->rx_state    = RX_DATA;
            m->rx_bitcount = 0;
            m->rx_byte     = 0;
        }
        /* else still marking idle */
    } else { /* RX_DATA */
        if (m->rx_bitcount < 8) {
            m->rx_byte |= (uint8_t)(bit << m->rx_bitcount);  /* LSB first */
            m->rx_bitcount++;
        } else {
            if (bit == 1)                    /* valid stop bit -> emit byte   */
                fifo_write(&m->rx, &m->rx_byte, 1);
            /* framing error (bit==0): drop the byte */
            m->rx_state = RX_IDLE;
        }
    }
}

/* ---- lifecycle ------------------------------------------------------------ */

modem_core_t *modem_core_create(int calling_party, int bit_rate,
                                int answer_tone_ms,
                                modem_event_fn on_event, void *ctx) {
    modem_core_t *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->calling   = calling_party;
    m->bit_rate  = bit_rate ? bit_rate : 1200;
    m->on_event  = on_event;
    m->ctx       = ctx;
    pthread_mutex_init(&m->lock, NULL);

    if (fifo_init(&m->tx, FIFO_CAP) != 0) goto fail;
    if (fifo_init(&m->rx, FIFO_CAP) != 0) goto fail;

    /* Bell 212A: no guard tone. We supply/consume UART-framed bits directly. */
    m->v22 = v22bis_init(NULL, m->bit_rate, 0, calling_party,
                         mc_get_bit, m,
                         mc_put_bit, m);
    if (!m->v22) goto fail;

    if (!calling_party && answer_tone_ms > 0) {
        m->ans = modem_connect_tones_tx_init(NULL, MODEM_CONNECT_TONES_BELL_ANS);
        if (m->ans) m->ans_samples_left = answer_tone_ms * 8; /* 8 samples/ms */
    }
    return m;

fail:
    modem_core_destroy(m);
    return NULL;
}

void modem_core_destroy(modem_core_t *m) {
    if (!m) return;
    if (m->v22) v22bis_free(m->v22);
    if (m->ans) modem_connect_tones_tx_free(m->ans);
    fifo_free(&m->tx);
    fifo_free(&m->rx);
    pthread_mutex_destroy(&m->lock);
    free(m);
}

/* ---- DTE byte path -------------------------------------------------------- */

size_t modem_core_send(modem_core_t *m, const uint8_t *data, size_t len) {
    return fifo_write(&m->tx, data, len);
}

size_t modem_core_recv(modem_core_t *m, uint8_t *data, size_t len) {
    return fifo_read(&m->rx, data, len);
}

int modem_core_is_connected(modem_core_t *m) {
    return m->connected;
}

/* ---- media sample path ---------------------------------------------------- */

void modem_core_tx_samples(modem_core_t *m, int16_t *out, int n) {
    pthread_mutex_lock(&m->lock);
    int done = 0;
    if (m->ans_samples_left > 0) {
        int want = n < m->ans_samples_left ? n : m->ans_samples_left;
        int got  = modem_connect_tones_tx(m->ans, out, want);
        m->ans_samples_left -= got;
        done = got;
    }
    if (done < n) {
        int got = v22bis_tx(m->v22, out + done, n - done);
        if (got < n - done)
            memset(out + done + got, 0, (size_t)(n - done - got) * sizeof(int16_t));
    }
    pthread_mutex_unlock(&m->lock);
}

void modem_core_rx_samples(modem_core_t *m, const int16_t *in, int n) {
    pthread_mutex_lock(&m->lock);
    v22bis_rx(m->v22, in, n);
    pthread_mutex_unlock(&m->lock);
}
