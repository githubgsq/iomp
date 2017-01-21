#include "iomp.h"

#if defined(__BSD__)

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/event.h>
#include <sys/types.h>
#include <sys/socket.h>
#include "iomp_atomic.h"
#include "iomp_queue.h"

#define IOMP_COMPLETE(aio, err) \
    do { \
        (aio)->complete((aio), (err)); \
        if (iomp_release(&(aio)->refcnt) == 0) { \
            (aio)->release((aio)); \
        } \
    } while (0)

struct iomp_queue {
    int kqfd;
    int intr[2];
    int nevents;
    struct kevent evs[];
};

static void on_read(iomp_queue_t q, iomp_aio_t aio);
static void on_write(iomp_queue_t q, iomp_aio_t aio);

iomp_queue_t iomp_queue_new(int nevents) {
    if (nevents <= 0) {
        errno = EINVAL;
        return NULL;
    }
    iomp_queue_t q = (iomp_queue_t)malloc(
            sizeof(*q) + sizeof(struct kevent) * nevents);
    if (!q) {
        return NULL;
    }
    q->nevents = nevents;
    q->kqfd = kqueue();
    if (q->kqfd == -1) {
        IOMP_LOG(ERROR, "kqueue fail: %s", strerror(errno));
        free(q);
        return NULL;
    }
    if (pipe(q->intr) == -1) {
        IOMP_LOG(ERROR, "pipe2 fail: %s", strerror(errno));
        close(q->kqfd);
        free(q);
        return NULL;
    }
    fcntl(q->intr[0], F_SETFL, fcntl(q->intr[0], F_GETFL, 0) | O_NONBLOCK);
    int sndbuf = sizeof(int);
    setsockopt(q->intr[1], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    struct kevent ev;
    EV_SET(&ev, q->intr[0], EVFILT_READ, EV_ADD | EV_DISPATCH, NOTE_LOWAT, sndbuf, NULL);
    if (kevent(q->kqfd, &ev, 1, NULL, 0, NULL) == -1) {
        IOMP_LOG(ERROR, "kevent fail: %s", strerror(errno));
        close(q->intr[1]);
        close(q->intr[0]);
        close(q->kqfd);
        free(q);
        return NULL;
    }
    return q;
}

void iomp_queue_drop(iomp_queue_t q) {
    if (!q) {
        return;
    }
    close(q->intr[1]);
    close(q->intr[0]);
    close(q->kqfd);
    free(q);
}

int iomp_queue_read(iomp_queue_t q, iomp_aio_t aio) {
    if (!q || !aio) {
        errno = EINVAL;
        return -1;
    }
    struct kevent kqev;
    EV_SET(&kqev, aio->fildes, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, aio);
    return kevent(q->kqfd, &kqev, 1, NULL, 0, NULL);
}

int iomp_queue_write(iomp_queue_t q, iomp_aio_t aio) {
    if (!q || !aio) {
        errno = EINVAL;
        return -1;
    }
    struct kevent kqev;
    EV_SET(&kqev, aio->fildes, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, aio);
    return kevent(q->kqfd, &kqev, 1, NULL, 0, NULL);
}

int iomp_queue_run(iomp_queue_t q, int timeout) {
    if (!q) {
        errno = EINVAL;
        return -1;
    }
    struct timespec ts = { timeout / 1000, (timeout % 1000) * 1000000 };
    int rv = kevent(q->kqfd, NULL, 0, q->evs, q->nevents,
            timeout >= 0 ? &ts : NULL);
    if (rv == -1) {
        return rv;
    }
    for (int i = 0; i < rv; i++) {
        struct kevent* kqev = q->evs + i;
        if (kqev->ident == q->intr[0]) {
            int buf = 0;
            read(q->intr[0], &buf, sizeof(buf));
            EV_SET(kqev, kqev->ident, EVFILT_READ, EV_ENABLE,
                    NOTE_LOWAT, sizeof(int), NULL);
            kevent(q->kqfd, kqev, 1, NULL, 0, NULL);
            //IOMP_LOG(DEBUG, "interrupted %d", q->kqfd);
            continue;
        }
        switch (kqev->filter) {
        case EVFILT_READ:
            on_read(q, (iomp_aio_t)kqev->udata);
            break;
        case EVFILT_WRITE:
            on_write(q, (iomp_aio_t)kqev->udata);
            break;
        default:
            break;
        }
    }
    return 0;
}

void iomp_queue_interrupt(iomp_queue_t q) {
    if (!q) {
        return;
    }
    int buf = 0;
    int rv = write(q->intr[1], &buf, sizeof(buf));
    if (rv != sizeof(buf)) {
        IOMP_LOG(ERROR, "write fail [errno=%d]: %s", errno, strerror(errno));
    }
}

void on_read(iomp_queue_t q, iomp_aio_t aio) {
    while (1) {
        size_t todo = aio->nbytes - aio->offset;
        ssize_t len = read(aio->fildes, aio->buf + aio->offset, todo);
        if (len > 0) {
            aio->offset += len;
            if (len == todo) {
                struct kevent ev;
                EV_SET(&ev, aio->fildes, EVFILT_READ, EV_DELETE, 0, 0, NULL);
                kevent(q->kqfd, &ev, 1, NULL, 0, NULL);
                IOMP_COMPLETE(aio, 0);
                break;
            }
        } else if (len == -1 && errno == EAGAIN) {
            break;
        } else {
            struct kevent ev;
            EV_SET(&ev, aio->fildes, EVFILT_READ, EV_DELETE, 0, 0, NULL);
            kevent(q->kqfd, &ev, 1, NULL, 0, NULL);
            IOMP_COMPLETE(aio, len == -1 ? errno : -1);
        }
    }
}

void on_write(iomp_queue_t q, iomp_aio_t aio) {
    while (1) {
        size_t todo = aio->nbytes - aio->offset;
        ssize_t len = write(aio->fildes, aio->buf + aio->offset, todo);
        if (len > 0) {
            aio->offset += len;
            if (len == todo) {
                struct kevent ev;
                EV_SET(&ev, aio->fildes, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
                kevent(q->kqfd, &ev, 1, NULL, 0, NULL);
                IOMP_COMPLETE(aio, 0);
                break;
            }
        } else if (len == -1 && errno == EAGAIN) {
            break;
        } else {
            struct kevent ev;
            EV_SET(&ev, aio->fildes, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
            kevent(q->kqfd, &ev, 1, NULL, 0, NULL);
            IOMP_COMPLETE(aio, len == -1 ? errno : -1);
        }
    }
}

#endif /* __BSD__ */

