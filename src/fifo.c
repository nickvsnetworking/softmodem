#include "fifo.h"

#include <stdlib.h>
#include <string.h>

int fifo_init(fifo_t *f, size_t capacity) {
    f->buf = malloc(capacity);
    if (!f->buf) return -1;
    f->cap   = capacity;
    f->head  = f->tail = f->count = 0;
    pthread_mutex_init(&f->lock, NULL);
    return 0;
}

void fifo_free(fifo_t *f) {
    if (!f->buf) return;
    free(f->buf);
    f->buf = NULL;
    pthread_mutex_destroy(&f->lock);
}

size_t fifo_write(fifo_t *f, const uint8_t *data, size_t len) {
    pthread_mutex_lock(&f->lock);
    size_t space = f->cap - f->count;
    if (len > space) len = space;
    for (size_t i = 0; i < len; i++) {
        f->buf[f->head] = data[i];
        f->head = (f->head + 1) % f->cap;
    }
    f->count += len;
    pthread_mutex_unlock(&f->lock);
    return len;
}

size_t fifo_read(fifo_t *f, uint8_t *data, size_t len) {
    pthread_mutex_lock(&f->lock);
    if (len > f->count) len = f->count;
    for (size_t i = 0; i < len; i++) {
        data[i] = f->buf[f->tail];
        f->tail = (f->tail + 1) % f->cap;
    }
    f->count -= len;
    pthread_mutex_unlock(&f->lock);
    return len;
}

size_t fifo_count(fifo_t *f) {
    pthread_mutex_lock(&f->lock);
    size_t c = f->count;
    pthread_mutex_unlock(&f->lock);
    return c;
}

void fifo_clear(fifo_t *f) {
    pthread_mutex_lock(&f->lock);
    f->head = f->tail = f->count = 0;
    pthread_mutex_unlock(&f->lock);
}
