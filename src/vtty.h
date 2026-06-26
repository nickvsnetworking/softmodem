/*
 * Virtual serial port backed by a PTY.
 *
 * Opens a pseudo-terminal master, puts the slave side into raw 8N1 mode, and
 * (best effort) symlinks the slave to a stable, modem-like path such as
 * /dev/ttyMODEM0 so that mm_manager - or any modem software - can open() it
 * exactly like a real serial port.
 */
#ifndef SOFTMODEM_VTTY_H_
#define SOFTMODEM_VTTY_H_

#include <stddef.h>

typedef struct {
    int  master_fd;          /* our side: read = DTE->modem, write = modem->DTE */
    int  slave_keepalive_fd; /* held open so master never sees POLLHUP          */
    char slave_path[64];     /* real /dev/pts/N                                  */
    char link_path[128];     /* stable symlink we created (may be empty)         */
} vtty_t;

/* Create the PTY and symlink. Returns 0 on success, <0 on error. */
int vtty_open(vtty_t *v, const char *link_path);

/* Non-blocking read of up to len bytes the DTE has sent us. >=0 bytes, -1 err. */
long vtty_read(vtty_t *v, void *buf, size_t len);

/* Write bytes toward the DTE (AT responses / inbound data). */
long vtty_write(vtty_t *v, const void *buf, size_t len);

/* Query DTR from the DTE side (1=asserted, 0=dropped, -1=unsupported). */
int  vtty_get_dtr(vtty_t *v);

void vtty_close(vtty_t *v);

#endif /* SOFTMODEM_VTTY_H_ */
