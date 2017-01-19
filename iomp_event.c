#include "iomp_event.h"

#if defined(__linux__)

#include <stdlib.h>
#include <errno.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>

int kqueue() {
    return epoll_create(1);
}

int kevent(int epfd, const struct kevent *changelist, int nchanges,
        struct kevent *eventlist, int nevents,
        const struct timespec *timeout) {
    const struct kevent* kev = NULL;
    struct epoll_event epev;
    if (nchanges && !changelist) {
        errno = EINVAL;
        return -1;
    }
    for (int i = 0; i < nchanges; i++) {
        kev = changelist + i;
        epev.events = 0;
        if (kev->filter == EVFILT_READ) {
            epev.events |= EPOLLIN;
            if (kev->data > 0) {
                int rv = setsockopt(
                        kev->ident, SOL_SOCKET, SO_RCVLOWAT,
                        &kev->data, sizeof(kev->data));
                if (rv == -1) {
                    return -1;
                }
            }
        }
        if (kev->flags & EV_CLEAR) {
            epev.events |= EPOLLET;
        }
        if (kev->flags & EV_ONESHOT) {
            epev.events |= EPOLLONESHOT;
        }
        epev.data.ptr = kev->udata;
        int op = 0;
        if (kev->flags & EV_ADD) {
            op = EPOLL_CTL_ADD;
        } else if (kev->flags & EV_DELETE) {
            op = EPOLL_CTL_DEL;
        }
        int rv = epoll_ctl(epfd, op, kev->ident, &epev);
        if (rv == -1 && op == EPOLL_CTL_ADD && errno == EEXIST) {
            rv = epoll_ctl(epfd, EPOLL_CTL_MOD, kev->ident, &epev);
        }
        if (rv != 0) {
            return -1;
        }
    }
    if (nevents <= 0) {
        return 0;
    }
    if (!eventlist) {
        errno = EINVAL;
        return -1;
    }
    struct epoll_event* evs = (struct epoll_event*)malloc(sizeof(*evs) * nevents);
    if (!evs) {
        return -1;
    }
    int timeout_ms = -1;
    if (timeout) {
        timeout_ms = timeout->tv_sec * 1000 + timeout->tv_nsec / 1000;
    }
    int rv = epoll_wait(epfd, evs, nevents, timeout_ms);
    if (rv == -1) {
        free(evs);
        return -1;
    }
    for (int i = 0; i < rv; i++) {
        struct epoll_event* epev = evs + i;
        struct kevent* kev = eventlist + i;
        kev->filter = 0;
        if (epev->events & EPOLLIN) {
            kev->filter = EVFILT_READ;
        }
        kev->udata = epev->data.ptr;
    }
    free(evs);
    return rv;
}

#endif /* Linux */

