#include "iomp.h"

#if defined(__linux__)

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include "iomp_queue.h"

struct iomp_queue {
    int epfd;
    int intr[2];
    int nevents;
    struct epoll_event evs[];
};

static void on_read(iomp_queue_t q, iomp_aio_t aio);
static void on_write(iomp_queue_t q, iomp_aio_t aio);

iomp_queue_t iomp_queue_new(int nevents) {
    if (nevents <= 0) {
        errno = EINVAL;
        return NULL;
    }
    iomp_queue_t q = (iomp_queue_t)malloc(
            sizeof(*q) + sizeof(struct epoll_event) * nevents);
    if (!q) {
        return NULL;
    }
    q->nevents = nevents;
    q->epfd = epoll_create(1);
    if (q->epfd == -1) {
        IOMP_LOG(ERROR, "epoll_create fail: %s", strerror(errno));
        free(q);
        return NULL;
    }
    if (socketpair(AF_LOCAL, SOCK_STREAM | SOCK_NONBLOCK, 0, q->intr) == -1) {
        IOMP_LOG(ERROR, "socketpair fail: %s", strerror(errno));
        close(q->epfd);
        free(q);
        return NULL;
    }
    //fcntl(q->intr[0], F_SETFL, fcntl(q->intr[0], F_GETFL, 0) | O_NONBLOCK);
    int sndbuf = sizeof(int);
    setsockopt(q->intr[1], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    struct epoll_event epev = { EPOLLIN | EPOLLONESHOT, { NULL } };
    if (epoll_ctl(q->epfd, EPOLL_CTL_ADD, q->intr[0], &epev) == -1) {
        IOMP_LOG(ERROR, "epoll_event fail: %s", strerror(errno));
        close(q->intr[1]);
        close(q->intr[0]);
        close(q->epfd);
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
    close(q->epfd);
    free(q);
}

int iomp_queue_read(iomp_queue_t q, iomp_aio_t aio) {
    if (!q || !aio || !aio->complete || !aio->buf) {
        errno = EINVAL;
        return -1;
    }
    struct epoll_event epev = { EPOLLIN | EPOLLET, { aio } };
    return epoll_ctl(q->epfd, EPOLL_CTL_ADD, aio->fildes, &epev);
}

int iomp_queue_write(iomp_queue_t q, iomp_aio_t aio) {
    if (!q || !aio || !aio->complete || !aio->buf) {
        errno = EINVAL;
        return -1;
    }
    struct epoll_event epev = { EPOLLOUT | EPOLLET, { aio } };
    return epoll_ctl(q->epfd, EPOLL_CTL_ADD, aio->fildes, &epev);
}

int iomp_queue_accept(iomp_queue_t q, struct iomp_aio* aio) {
    if (!q || !aio || !aio->complete || aio->buf) {
        errno = EINVAL;
        return -1;
    }
    struct epoll_event epev = { EPOLLIN, { aio } };
    return epoll_ctl(q->epfd, EPOLL_CTL_ADD, aio->fildes, &epev);
}

int iomp_queue_run(iomp_queue_t q, int timeout) {
    if (!q) {
        errno = EINVAL;
        return -1;
    }
    int rv = epoll_wait(q->epfd, q->evs, q->nevents, timeout);
    if (rv == -1) {
        return rv;
    }
    for (int i = 0; i < rv; i++) {
        struct epoll_event* epev = q->evs + i;
        if (epev->data.ptr == NULL) {
            int buf = 0;
            read(q->intr[0], &buf, sizeof(buf));
            //IOMP_LOG(DEBUG, "interrupted %d", q->epfd);
            epev->events = EPOLLIN | EPOLLONESHOT;
            epoll_ctl(q->epfd, EPOLL_CTL_MOD, q->intr[0], epev);
            continue;
        }
        iomp_aio_t aio = (iomp_aio_t)epev->data.ptr;
        if (!aio->buf) {
            aio->complete(aio, 0);
            continue;
        }
        if (epev->events & EPOLLIN) {
            on_read(q, aio);
        }
        if (epev->events & EPOLLOUT) {
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
            struct epoll_event epev = { EPOLLIN, { NULL } };
            epoll_ctl(q->epfd, EPOLL_CTL_DEL, aio->fildes, &epev);
            aio->complete(aio, len == -1 ? errno : -1);
            return;
        }
    }
    aio->offset = aio->nbytes;
    struct epoll_event epev = { EPOLLIN, { NULL } };
    epoll_ctl(q->epfd, EPOLL_CTL_DEL, aio->fildes, &epev);
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
            struct epoll_event epev = { EPOLLOUT, { NULL } };
            epoll_ctl(q->epfd, EPOLL_CTL_DEL, aio->fildes, &epev);
            aio->complete(aio, len == -1 ? errno : -1);
            return;
        }
    }
    aio->offset = aio->nbytes;
    struct epoll_event epev = { EPOLLOUT, { NULL } };
    epoll_ctl(q->epfd, EPOLL_CTL_DEL, aio->fildes, &epev);
    aio->complete(aio, 0);
}

#endif /* __linux__ */

