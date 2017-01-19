#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include "iomp_atomic.h"
#include "iomp_event.h"
#include "iomp.h"

#define IOMP_LOG(fmt, ...) \
    fprintf(stderr, "[libiomp] " __FILE__ ":%d " fmt "\n", \
            __LINE__, ##__VA_ARGS__)

#define IOMP_EVENT_LIMIT 1024

#define IOMP_COMPLETE(aio, succ, err) \
    do { \
        (aio)->error = (err); \
        (aio)->complete((aio), (succ)); \
        if (iomp_release(&(aio)->refcnt) == 0) { \
            (aio)->release((aio)); \
        } \
    } while (0)

struct iomp_core {
    pthread_mutex_t lock;
    pthread_t* threads;
    int nthread;
    iomp_queue_t queue;
    uint64_t blocked;
    int intr[2];
    unsigned stop:1;
};

static void* do_work(void* arg);
static void do_wait(iomp_t iomp, iomp_evlist_t evs);
static void do_stop_withlock(iomp_t iomp);
static void do_interrupt(iomp_t iomp);

iomp_t iomp_new(int nthread) {
    iomp_t iomp = (iomp_t)malloc(sizeof(*iomp));
    if (!iomp) {
        IOMP_LOG("malloc fail: %s", strerror(errno));
        return NULL;
    }
    iomp->blocked = 0;
    iomp->stop = 0;
    iomp->queue = iomp_queue_new();
    if (!iomp->queue) {
        IOMP_LOG("iomp_queue_new fail: %s", strerror(errno));
        free(iomp);
        return NULL;
    }
    int rv = pipe(iomp->intr);
    if (rv != 0) {
        IOMP_LOG("pipe fail: %s", strerror(errno));
        iomp_queue_drop(iomp->queue);
        free(iomp);
        return NULL;
    }
    fcntl(iomp->intr[0], F_SETFL, fcntl(iomp->intr[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(iomp->intr[1], F_SETFL, fcntl(iomp->intr[1], F_GETFL, 0) | O_NONBLOCK);
    struct iomp_event ev;
    IOMP_EVENT_SET(&ev, iomp->intr[0], IOMP_EVENT_READ | IOMP_EVENT_EDGE, 0, iomp);
    if (iomp_queue_post(iomp->queue, &ev) == -1) {
        IOMP_LOG("iomp_queue_post fail: %s", strerror(errno));
        close(iomp->intr[1]);
        close(iomp->intr[0]);
        iomp_queue_drop(iomp->queue);
        free(iomp);
        return NULL;
    }
    rv = pthread_mutex_init(&iomp->lock, NULL);
    if (rv != 0) {
        IOMP_LOG("pthread_mutex_init fail: %s", strerror(rv));
        close(iomp->intr[1]);
        close(iomp->intr[0]);
        iomp_queue_drop(iomp->queue);
        free(iomp);
        return NULL;
    }
    pthread_mutex_lock(&iomp->lock);
    if (nthread <= 0) {
#if defined(__APPLE__) || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__)
        int mib[2] = { CTL_HW, HW_NCPU };
        int ncpu = 0;
        size_t len = sizeof(ncpu);
        rv = sysctl(mib, 2, &ncpu, &len, NULL, 0);
        if (rv != 0) {
            IOMP_LOG("sysctlbyname fail: %s", strerror(rv));
            close(iomp->intr[1]);
            close(iomp->intr[0]);
            iomp_queue_drop(iomp->queue);
            free(iomp);
            return NULL;
        }
        nthread = ncpu;
#elif defined(__linux__)
        nthread = sysconf(_SC_NPROCESSORS_ONLN);
        if (nthread == -1) {
            IOMP_LOG("sysconf fail: %s", strerror(rv));
            close(iomp->intr[1]);
            close(iomp->intr[0]);
            iomp_queue_drop(iomp->queue);
            free(iomp);
            return NULL;
        }
#else
        IOMP_LOG("require positive nthread");
        close(iomp->intr[1]);
        close(iomp->intr[0]);
        iomp_queue_drop(iomp->queue);
        free(iomp);
        return NULL;
#endif
    }
    iomp->nthread = 0;
    iomp->threads = (pthread_t*)malloc(sizeof(*iomp->threads) * nthread);
    if (!iomp->threads) {
        IOMP_LOG("malloc fail: %s", strerror(errno));
        pthread_mutex_destroy(&iomp->lock);
        close(iomp->intr[1]);
        close(iomp->intr[0]);
        iomp_queue_drop(iomp->queue);
        free(iomp);
        return NULL;
    }
    for (int i = 0; i < nthread; i++) {
        rv = pthread_create(iomp->threads + i, NULL, do_work, iomp);
        if (rv != 0) {
            IOMP_LOG("pthread_create fail: %s", strerror(rv));
        } else {
            iomp->nthread++;
        }
    }
    pthread_mutex_unlock(&iomp->lock);
    return iomp;
}

void iomp_drop(iomp_t iomp) {
    if (!iomp) {
        return;
    }
    pthread_mutex_lock(&iomp->lock);
    do_stop_withlock(iomp);
    pthread_mutex_unlock(&iomp->lock);
    for (int i = 0; i < iomp->nthread; i++) {
        pthread_join(iomp->threads[i], NULL);
    }
    free(iomp->threads);
    pthread_mutex_destroy(&iomp->lock);
    close(iomp->intr[1]);
    close(iomp->intr[0]);
    iomp_queue_drop(iomp->queue);
    free(iomp);
}

#if 0
int iomp_signal(iomp_t iomp, const iomp_signal_t sig) {
    if (!iomp || !sig) {
        IOMP_LOG("invalid argument");
        return -1;
    }
    if (sig->ready) {
        struct kevent ev;
        EV_SET(&ev, sig->signal, EVFILT_SIGNAL, EV_ADD, 0, 0, sig);
        return do_change(iomp, &ev);
    } else {
        struct kevent ev;
        EV_SET(&ev, sig->signal, EVFILT_SIGNAL, EV_DELETE, 0, 0, NULL);
        return do_change(iomp, &ev);
    }
}
#endif

int iomp_read(iomp_t iomp, iomp_aio_t aio) {
    if (!aio || !aio->complete || !aio->release) {
        errno = EINVAL;
        return -1;
    }
    iomp_addref(&aio->refcnt);
    if (!iomp) {
        IOMP_COMPLETE(aio, 0, EINVAL);
        return 0;
    }
#if 0
    int val = fcntl(aio->fildes, F_GETFL, 0);
    if (val == -1) {
        IOMP_COMPLETE(aio, 0, errno);
        return 0;
    }
    if (fcntl(aio->fildes, F_SETFL, val | O_NONBLOCK) == -1) {
        IOMP_COMPLETE(aio, 0, errno);
        return 0;
    }
#endif
    struct iomp_event ev;
    IOMP_EVENT_SET(&ev,
            aio->fildes, IOMP_EVENT_READ | IOMP_EVENT_ONCE, aio->nbytes, aio);
    if (iomp_queue_post(iomp->queue, &ev) == -1) {
        IOMP_COMPLETE(aio, 0, errno);
        return 0;
    }
    return 0;
}

void* do_work(void* arg) {
    iomp_t iomp = (iomp_t)arg;
    iomp_evlist_t evs = iomp_evlist_new(128);
    if (!evs) {
        IOMP_LOG("iomp_evlist_new fail: %s", strerror(errno));
        return NULL;
    }
    pthread_mutex_lock(&iomp->lock);
    while (!iomp->stop) {
        do_wait(iomp, evs);
    }
    if (iomp->blocked) {
        do_interrupt(iomp);
    }
    pthread_mutex_unlock(&iomp->lock);
    iomp_evlist_drop(evs);
    return NULL;
}

void do_wait(iomp_t iomp, iomp_evlist_t evs) {
    iomp->blocked++;
    pthread_mutex_unlock(&iomp->lock);
    int rv = iomp_queue_wait(iomp->queue, evs, -1);
    if (rv == 0) {
        struct iomp_event ev;
        while (iomp_evlist_next(evs, &ev)) {
            if (ev.udata == iomp) {
                int buf = 0;
                read(iomp->intr[0], &buf, sizeof(buf));
                IOMP_LOG("interrupted");
            } else if (ev.flags & IOMP_EVENT_READ) {
                iomp_aio_t aio = (iomp_aio_t)ev.udata;
                ssize_t len = read(aio->fildes, aio->buf, aio->nbytes);
                if (len == aio->nbytes) {
                    IOMP_COMPLETE(aio, 1, 0);
                } else {
                    IOMP_COMPLETE(aio, 0, (len == -1 ? errno : 0));
                }
            }
        }
    }
    pthread_mutex_lock(&iomp->lock);
    iomp->blocked--;
    if (rv != 0) {
        do_stop_withlock(iomp);
    }
}

void do_stop_withlock(iomp_t iomp) {
    IOMP_LOG("begin stop [blocked=%zu]", (size_t)iomp->blocked);
    iomp->stop = 1;
    if (iomp->blocked) {
        do_interrupt(iomp);
    }
}

void do_interrupt(iomp_t iomp) {
    int data = 0;
    write(iomp->intr[1], &data, sizeof(data));
}

