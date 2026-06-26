/*
 * Thread-safe byte ring buffer (single producer / single consumer is the
 * typical use, but a mutex guards all access so any pairing is safe).
 *
 * Used for the two DTE byte streams between the AT engine and the modem core.
 */
#ifndef SOFTMODEM_FIFO_H_
#define SOFTMODEM_FIFO_H_

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

typedef struct {
    uint8_t        *buf;
    size_t          cap;     /* capacity (power of two not required)        */
    size_t          head;    /* write index                                 */
    size_t          tail;    /* read index                                  */
    size_t          count;   /* bytes currently stored                      */
    pthread_mutex_t lock;
} fifo_t;

int    fifo_init(fifo_t *f, size_t capacity);
void   fifo_free(fifo_t *f);

/* Returns number of bytes actually written (may be < len if full). */
size_t fifo_write(fifo_t *f, const uint8_t *data, size_t len);

/* Returns number of bytes actually read (may be < len if near-empty). */
size_t fifo_read(fifo_t *f, uint8_t *data, size_t len);

size_t fifo_count(fifo_t *f);
void   fifo_clear(fifo_t *f);

#endif /* SOFTMODEM_FIFO_H_ */
