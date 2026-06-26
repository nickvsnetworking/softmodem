#define _GNU_SOURCE
#include "vtty.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>

int vtty_open(vtty_t *v, const char *link_path) {
    memset(v, 0, sizeof(*v));
    v->master_fd = -1;
    v->slave_keepalive_fd = -1;

    v->master_fd = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (v->master_fd < 0) {
        perror("posix_openpt");
        return -1;
    }
    if (grantpt(v->master_fd) != 0 || unlockpt(v->master_fd) != 0) {
        perror("grantpt/unlockpt");
        goto fail;
    }
    if (ptsname_r(v->master_fd, v->slave_path, sizeof(v->slave_path)) != 0) {
        perror("ptsname_r");
        goto fail;
    }

    /* Configure the slave for raw 8N1 - no echo, no canonical processing, so
     * AT commands and binary modem data pass through untouched. The DTE may
     * re-set this (mm_manager calls cfmakeraw itself), which is fine. */
    int slave = open(v->slave_path, O_RDWR | O_NOCTTY);
    if (slave < 0) {
        perror("open slave");
        goto fail;
    }
    struct termios t;
    if (tcgetattr(slave, &t) == 0) {
        cfmakeraw(&t);
        t.c_cflag |= (CS8 | CLOCAL | CREAD);
        t.c_cc[VMIN]  = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(slave, TCSANOW, &t);
    }
    /* Keep one slave fd open so the master end never gets EIO/POLLHUP while the
     * DTE is between opens. */
    v->slave_keepalive_fd = slave;

    /* Best-effort stable symlink. Needs write access to the target dir; if it
     * fails (e.g. /dev without root) we carry on and report the pts path. */
    if (link_path && link_path[0]) {
        unlink(link_path); /* clear stale link */
        if (symlink(v->slave_path, link_path) == 0) {
            snprintf(v->link_path, sizeof(v->link_path), "%s", link_path);
        } else {
            fprintf(stderr,
                    "vtty: could not symlink %s -> %s (%s); use the pts path directly\n",
                    link_path, v->slave_path, strerror(errno));
        }
    }

    printf("vtty: virtual modem at %s%s%s\n",
           v->slave_path,
           v->link_path[0] ? " (also " : "",
           v->link_path[0] ? v->link_path : "");
    if (v->link_path[0]) printf(")\n");
    return 0;

fail:
    vtty_close(v);
    return -1;
}

long vtty_read(vtty_t *v, void *buf, size_t len) {
    long n = read(v->master_fd, buf, len);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EIO))
        return 0; /* EIO transiently when slave has no opener - treat as idle */
    return n;
}

long vtty_write(vtty_t *v, const void *buf, size_t len) {
    return write(v->master_fd, buf, len);
}

int vtty_get_dtr(vtty_t *v) {
    int status = 0;
    if (ioctl(v->master_fd, TIOCMGET, &status) != 0)
        return -1; /* PTYs generally don't report this */
    return (status & TIOCM_DTR) ? 1 : 0;
}

void vtty_close(vtty_t *v) {
    if (v->link_path[0]) {
        unlink(v->link_path);
        v->link_path[0] = '\0';
    }
    if (v->slave_keepalive_fd >= 0) {
        close(v->slave_keepalive_fd);
        v->slave_keepalive_fd = -1;
    }
    if (v->master_fd >= 0) {
        close(v->master_fd);
        v->master_fd = -1;
    }
}
