#include "iomp.h"

#if defined(__BSD__)

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include "iomp_queue.h"

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
    if (socketpair(AF_LOCAL, SOCK_STREAM, 0, q->intr) == -1) {
        IOMP_LOG(ERROR, "socketpair fail: %s", strerror(errno));
        close(q->kqfd);
        free(q);
        return NULL;
    }
    fcntl(q->intr[0], F_SETFL, fcntl(q->intr[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(q->intr[1], F_SETFL, fcntl(q->intr[1], F_GETFL, 0) | O_NONBLOCK);
    int sndbuf = sizeof(int);
    setsockopt(q->intr[1], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    struct kevent kqev;
    EV_SET(&kqev, q->intr[0], EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, NULL);
    if (kevent(q->kqfd, &kqev, 1, NULL, 0, NULL) == -1) {
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
    if (!q || !aio || !aio->complete || !aio->buf) {
        errno = EINVAL;
        return -1;
    }
    struct kevent kqev;
    EV_SET(&kqev, aio->fildes, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, aio);
    return kevent(q->kqfd, &kqev, 1, NULL, 0, NULL);
}

int iomp_queue_write(iomp_queue_t q, iomp_aio_t aio) {
    if (!q || !aio || !aio->complete || !aio->buf) {
        errno = EINVAL;
        return -1;
    }
    struct kevent kqev;
    EV_SET(&kqev, aio->fildes, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, aio);
    return kevent(q->kqfd, &kqev, 1, NULL, 0, NULL);
}

int iomp_queue_accept(iomp_queue_t q, struct iomp_aio* aio) {
    if (!q || !aio || !aio->complete || aio->buf) {
        errno = EINVAL;
        return -1;
    }
    struct kevent kqev;
    EV_SET(&kqev, aio->fildes, EVFILT_READ, EV_ADD, 0, 0, aio);
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
        if (kqev->udata == NULL) {
            int buf = 0;
            read(q->intr[0], &buf, sizeof(buf));
            //IOMP_LOG(DEBUG, "interrupted %d", q->kqfd);
            EV_SET(kqev, q->intr[0], EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, NULL);
            kevent(q->kqfd, kqev, 1, NULL, 0, NULL);
            continue;
        }
        iomp_aio_t aio = (iomp_aio_t)kqev->udata;
        if (!aio->buf) {
            aio->complete(aio, 0);
            continue;
        }
        if (kqev->filter == EVFILT_READ) {
            on_read(q, aio);
        }
        if (kqev->filter == EVFILT_WRITE) {
            on_write(q, aio);
        }
    }
    return 0;
}

void iomp_queue_interrupt(iomp_queue_t q) {
    if (!q) {
        return;
    }
    int buf = 0;
    write(q->intr[1], &buf, sizeof(buf));
}

void on_read(iomp_queue_t q, iomp_aio_t aio) {
    void* buf = aio->buf + aio->offset;
    size_t todo = aio->nbytes - aio->offset;
    while (todo > 0) {
        ssize_t len = read(aio->fildes, buf, todo);
        if (len > 0) {
            buf += len;
            todo -= len;
        } else if (len == -1 && errno == EAGAIN) {
            aio->offset = aio->nbytes - todo;
            return;
        } else {
            aio->offset = aio->nbytes - todo;
            struct kevent kqev;
            EV_SET(&kqev, aio->fildes, EVFILT_READ, EV_DELETE, 0, 0, NULL);
            kevent(q->kqfd, &kqev, 1, NULL, 0, NULL);
            aio->complete(aio, len == -1 ? errno : -1);
            return;
        }
    }
    aio->offset = aio->nbytes;
    struct kevent kqev;
    EV_SET(&kqev, aio->fildes, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    kevent(q->kqfd, &kqev, 1, NULL, 0, NULL);
    aio->complete(aio, 0);
}

void on_write(iomp_queue_t q, iomp_aio_t aio) {
    void* buf = aio->buf + aio->offset;
    size_t todo = aio->nbytes - aio->offset;
    while (todo > 0) {
        ssize_t len = write(aio->fildes, buf, todo);
        if (len > 0) {
            buf += len;
            todo -= len;
        } else if (len == -1 && errno == EAGAIN) {
            aio->offset = aio->nbytes - todo;
            return;
        } else {
            aio->offset = aio->nbytes - todo;
            struct kevent kqev;
            EV_SET(&kqev, aio->fildes, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
            kevent(q->kqfd, &kqev, 1, NULL, 0, NULL);
            aio->complete(aio, len == -1 ? errno : -1);
            return;
        }
    }
    aio->offset = aio->nbytes;
    struct kevent kqev;
    EV_SET(&kqev, aio->fildes, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    kevent(q->kqfd, &kqev, 1, NULL, 0, NULL);
    aio->complete(aio, 0);
}

#endif /* __BSD__ */

