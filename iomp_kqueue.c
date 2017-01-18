#if defined(__APPLE__) || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__)

#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/event.h>
#include <sys/types.h>
#include <sys/socket.h>
#include "iomp_event.h"

struct iomp_queue {
    int kqfd;
};

struct iomp_evlist {
    int nevents;
    int ready;
    int offset;
    struct kevent evs[];
};

iomp_queue_t iomp_queue_new() {
    iomp_queue_t q = (iomp_queue_t)malloc(sizeof(*q));
    if (!q) {
        return NULL;
    }
    q->kqfd = kqueue();
    if (q->kqfd == -1) {
        free(q);
        return NULL;
    }
    return q;
}

void iomp_queue_drop(iomp_queue_t q) {
    if (!q) {
        return;
    }
    close(q->kqfd);
    free(q);
}

iomp_evlist_t iomp_evlist_new(int nevents) {
    if (nevents < 1) {
        errno = EINVAL;
        return NULL;
    }
    iomp_evlist_t evs = (iomp_evlist_t)malloc(
            sizeof(*evs) + sizeof(struct kevent) * nevents);
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
    struct kevent* kqev = evs->evs + evs->offset;
    ev->flags = 0;
    if (kqev->filter & EVFILT_READ) {
        ev->flags |= IOMP_EVENT_READ;
    }
    if (kqev->filter & EVFILT_WRITE) {
        ev->flags |= IOMP_EVENT_WRITE;
    }
    ev->udata = kqev->udata;
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
    struct kevent kqev;
    int16_t filter = 0;
    uint16_t flags = EV_ADD;
    if (ev->flags & IOMP_EVENT_READ) {
        filter = EVFILT_READ;
    } else if (ev->flags & IOMP_EVENT_WRITE) {
        filter = EVFILT_WRITE;
    }
    if (ev->flags & IOMP_EVENT_EDGE) {
        flags |= EV_CLEAR;
    }
    if (ev->flags & IOMP_EVENT_ONCE) {
        flags |= EV_ONESHOT;
    }
    EV_SET(&kqev, ev->ident, filter, flags, 0, ev->lowat, ev->udata);
    return kevent(q->kqfd, &kqev, 1, NULL, 0, NULL);
}

int iomp_queue_wait(iomp_queue_t q, iomp_evlist_t evs, int timeout) {
    if (!q || !evs) {
        errno = EINVAL;
        return -1;
    }
    struct timespec ts = { timeout / 1000, (timeout % 1000) * 1000 };
    int rv = kevent(q->kqfd, NULL, 0, evs->evs, evs->nevents,
            timeout >= 0 ? &ts : NULL);
    if (rv == -1) {
        return rv;
    }
    evs->ready = rv;
    evs->offset = 0;
    return 0;
}

#endif /* BSD based */

