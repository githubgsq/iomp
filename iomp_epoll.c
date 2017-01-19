#if defined(__linux__)

#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include "iomp_event.h"

struct iomp_queue {
    int epfd;
};

struct iomp_evlist {
    int nevents;
    int ready;
    int offset;
    struct epoll_event evs[];
};

iomp_queue_t iomp_queue_new() {
    iomp_queue_t q = (iomp_queue_t)malloc(sizeof(*q));
    if (!q) {
        return NULL;
    }
    q->epfd = epoll_create(1);
    if (q->epfd == -1) {
        free(q);
        return NULL;
    }
    return q;
}

void iomp_queue_drop(iomp_queue_t q) {
    if (!q) {
        return;
    }
    close(q->epfd);
    free(q);
}

iomp_evlist_t iomp_evlist_new(int nevents) {
    if (nevents < 1) {
        errno = EINVAL;
        return NULL;
    }
    iomp_evlist_t evs = (iomp_evlist_t)malloc(
            sizeof(*evs) + sizeof(struct epoll_event) * nevents);
    if (!evs) {
        return NULL;
    }
    evs->nevents = nevents;
    evs->ready = 0;
    evs->offset = 0;
    return evs;
}

void iomp_evlist_drop(iomp_evlist_t evs) {
    if (!evs) {
        return;
    }
    free(evs);
}

int iomp_evlist_next(iomp_evlist_t evs, iomp_event_t ev) {
    if (!evs || evs->ready == 0) {
        return 0;
    }
    struct epoll_event* epev = evs->evs + evs->offset;
    ev->flags = 0;
    if (epev->events & EPOLLIN) {
        ev->flags |= IOMP_EVENT_READ;
    }
    if (epev->events & EPOLLOUT) {
        ev->flags |= IOMP_EVENT_WRITE;
    }
    ev->udata = epev->data.ptr;
    if (++evs->offset == evs->ready) {
        evs->ready = 0;
        evs->offset = 0;
    }
    return 1;
}


int iomp_queue_post(iomp_queue_t q, iomp_event_t ev) {
    if (!q || !ev) {
        errno = EINVAL;
        return -1;
    }
    struct epoll_event epev;
    if (ev->lowat > 0) {
        int rv = setsockopt(
            ev->ident, SOL_SOCKET, SO_RCVLOWAT,
            &ev->lowat, sizeof(ev->lowat));
        if (rv == -1) {
            return -1;
        }
    }
    epev.events = 0;
    if (ev->flags & IOMP_EVENT_READ) {
        epev.events |= EPOLLIN;
    }
    if (ev->flags & IOMP_EVENT_WRITE) {
        epev.events |= EPOLLOUT;
    }
    if (ev->flags & IOMP_EVENT_EDGE) {
        epev.events |= EPOLLET;
    }
    if (ev->flags & IOMP_EVENT_ONCE) {
        epev.events |= EPOLLONESHOT;
    }
    epev.data.ptr = ev->udata;
    int rv = epoll_ctl(q->epfd, EPOLL_CTL_ADD, ev->ident, &epev);
    if (rv == -1 && errno == EEXIST) {
        rv = epoll_ctl(q->epfd, EPOLL_CTL_MOD, ev->ident, &epev);
    }
    return rv;
}

int iomp_queue_wait(iomp_queue_t q, iomp_evlist_t evs, int timeout) {
    if (!q || !evs) {
        errno = EINVAL;
        return -1;
    }
    int rv = epoll_wait(q->epfd, evs->evs, evs->nevents, timeout);
    if (rv == -1) {
        return rv;
    }
    evs->ready = rv;
    evs->offset = 0;
    return 0;
}

#endif /* Linux */

