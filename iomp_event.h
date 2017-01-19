#ifndef IOMP_EVENT_H
#define IOMP_EVENT_H

#include <stdint.h>
#include <stddef.h>

struct iomp_event {
    int ident;
    uint16_t flags;
    size_t lowat;
    void* udata;
};
typedef struct iomp_event* iomp_event_t;
#define IOMP_EVENT_SET(ev, a, b, c, d) \
    do { \
        (ev)->ident = (a); \
        (ev)->flags = (b); \
        (ev)->lowat = (c); \
        (ev)->udata = (d); \
    } while (0)

#define IOMP_EVENT_READ     0x0001
#define IOMP_EVENT_WRITE    0x0002
#define IOMP_EVENT_EDGE     0x0004
#define IOMP_EVENT_ONCE     0x0008

struct iomp_queue;
typedef struct iomp_queue* iomp_queue_t;

iomp_queue_t iomp_queue_new();
void iomp_queue_drop(iomp_queue_t q);

struct iomp_evlist;
typedef struct iomp_evlist* iomp_evlist_t;

iomp_evlist_t iomp_evlist_new(int nevents);
void iomp_evlist_drop(iomp_evlist_t evs);
int iomp_evlist_next(iomp_evlist_t evs, iomp_event_t ev);

int iomp_queue_post(iomp_queue_t q, iomp_event_t ev);
int iomp_queue_wait(iomp_queue_t q, iomp_evlist_t evs, int timeout);

#if defined(__APPLE__) || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <sys/event.h>
#include <sys/time.h>
#endif /* BSD based */

#if defined(__linux__)
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>
struct kevent {
    uintptr_t   ident;  /* identifier for this event */
    int16_t     filter; /* filter for event */
    uint16_t    flags;  /* general flags */
    uint32_t    fflags; /* filter-specific flags */
    intptr_t    data;   /* filter-specific data */
    void*       udata;  /* opaque user data identifier */
};

#define EVFILT_READ (-1)
/* actions */
#define EV_ADD      0x0001      /* add event to kq (implies enable) */
#define EV_DELETE   0x0002      /* delete event from kq */

/* flags */
#define EV_ONESHOT  0x0010      /* only report one occurrence */
#define EV_CLEAR    0x0020      /* clear event state after reporting */

#define EV_SET(kev, kev_ident, kev_filter, kev_flags, kev_fflags, kev_data, kev_udata) \
    do { \
        (kev)->ident = (kev_ident); \
        (kev)->filter = (kev_filter); \
        (kev)->flags = (kev_flags); \
        (kev)->fflags = (kev_fflags); \
        (kev)->data = (kev_data); \
        (kev)->udata = (kev_udata); \
    } while (0)

int kqueue();

int kevent(int kq, const struct kevent *changelist, int nchanges,
        struct kevent *eventlist, int nevents,
        const struct timespec *timeout);
#endif /* Linux */

#endif /* IOMP_EVENT_H */

