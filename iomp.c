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

#define IOMP_EVENT_LIMIT 1024

#define IOMP_COMPLETE(aio, err) \
    do { \
        IOMP_LOG(DEBUG, "aio complete: %d", (err)); \
        (aio)->complete((aio), (err)); \
        if (iomp_release(&(aio)->refcnt) == 0) { \
            (aio)->release((aio)); \
        } \
    } while (0)

struct iomp_core {
    pthread_mutex_t lock;
    STAILQ_HEAD(, iomp_aio) jobs;
    pthread_t* threads;
    struct iomp_aio stop;
    int nthreads;
    iomp_queue_t queue;
    uint64_t blocked;
    int intr[2];
};

static void* do_work(void* arg);
static void do_post(iomp_t iomp, iomp_aio_t aio);
static void do_wait(iomp_t iomp, iomp_evlist_t evs);
static void do_stop(iomp_t iomp, iomp_aio_t aio);
static void do_interrupt(iomp_t iomp);
static void do_read(iomp_t iomp, iomp_aio_t aio);
static void on_read(iomp_t iomp, iomp_event_t ev);
static void do_write(iomp_t iomp, iomp_aio_t aio);
static void on_write(iomp_t iomp, iomp_event_t ev);

iomp_t iomp_new(int nthreads) {
    iomp_t iomp = (iomp_t)malloc(sizeof(*iomp));
    if (!iomp) {
        IOMP_LOG(ERROR, "malloc fail: %s", strerror(errno));
        return NULL;
    }
    STAILQ_INIT(&iomp->jobs);
    iomp->blocked = 0;
    iomp->queue = iomp_queue_new();
    if (!iomp->queue) {
        IOMP_LOG(ERROR, "iomp_queue_new fail: %s", strerror(errno));
        free(iomp);
        return NULL;
    }
    int rv = pipe(iomp->intr);
    if (rv != 0) {
        IOMP_LOG(ERROR, "pipe fail: %s", strerror(errno));
        iomp_queue_drop(iomp->queue);
        free(iomp);
        return NULL;
    }
    fcntl(iomp->intr[0], F_SETFL, fcntl(iomp->intr[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(iomp->intr[1], F_SETFL, fcntl(iomp->intr[1], F_GETFL, 0) | O_NONBLOCK);
    struct iomp_event ev;
    IOMP_EVENT_SET(&ev, iomp->intr[0], IOMP_EVENT_READ | IOMP_EVENT_EDGE, 0, iomp);
    if (iomp_queue_add(iomp->queue, &ev) == -1) {
        IOMP_LOG(ERROR, "iomp_queue_post fail: %s", strerror(errno));
        close(iomp->intr[1]);
        close(iomp->intr[0]);
        iomp_queue_drop(iomp->queue);
        free(iomp);
        return NULL;
    }
    rv = pthread_mutex_init(&iomp->lock, NULL);
    if (rv != 0) {
        IOMP_LOG(ERROR, "pthread_mutex_init fail: %s", strerror(rv));
        close(iomp->intr[1]);
        close(iomp->intr[0]);
        iomp_queue_drop(iomp->queue);
        free(iomp);
        return NULL;
    }
    pthread_mutex_lock(&iomp->lock);
    if (nthreads <= 0) {
#if defined(__APPLE__) || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__)
        int mib[2] = { CTL_HW, HW_NCPU };
        int ncpu = 0;
        size_t len = sizeof(ncpu);
        rv = sysctl(mib, 2, &ncpu, &len, NULL, 0);
        if (rv != 0) {
            IOMP_LOG(ERROR, "sysctlbyname fail: %s", strerror(rv));
            close(iomp->intr[1]);
            close(iomp->intr[0]);
            iomp_queue_drop(iomp->queue);
            free(iomp);
            return NULL;
        }
        nthreads = ncpu;
#elif defined(__linux__)
        nthreads = sysconf(_SC_NPROCESSORS_ONLN);
        if (nthreads == -1) {
            IOMP_LOG(ERROR, "sysconf fail: %s", strerror(rv));
            close(iomp->intr[1]);
            close(iomp->intr[0]);
            iomp_queue_drop(iomp->queue);
            free(iomp);
            return NULL;
        }
#else
        IOMP_LOG(ERROR, "require positive nthreads");
        close(iomp->intr[1]);
        close(iomp->intr[0]);
        iomp_queue_drop(iomp->queue);
        free(iomp);
        return NULL;
#endif
    }
    iomp->nthreads = 0;
    iomp->stop.fildes = -1;
    iomp->stop.execute = do_stop;
    iomp->threads = (pthread_t*)malloc(sizeof(*iomp->threads) * nthreads);
    if (!iomp->threads) {
        IOMP_LOG(ERROR, "malloc fail: %s", strerror(errno));
        pthread_mutex_destroy(&iomp->lock);
        close(iomp->intr[1]);
        close(iomp->intr[0]);
        iomp_queue_drop(iomp->queue);
        free(iomp);
        return NULL;
    }
    for (int i = 0; i < nthreads; i++) {
        rv = pthread_create(iomp->threads + i, NULL, do_work, iomp);
        if (rv != 0) {
            IOMP_LOG(ERROR, "pthread_create fail: %s", strerror(rv));
        } else {
            iomp->nthreads++;
        }
    }
    pthread_mutex_unlock(&iomp->lock);
    return iomp;
}

void iomp_drop(iomp_t iomp) {
    if (!iomp) {
        return;
    }
    do_post(iomp, &iomp->stop);
    for (int i = 0; i < iomp->nthreads; i++) {
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

void iomp_read(iomp_t iomp, iomp_aio_t aio) {
    if (!aio || !aio->complete || !aio->release) {
        IOMP_LOG(ERROR, "invalid argument");
        return;
    }
    iomp_addref(&aio->refcnt);
    if (!iomp || !aio->buf || aio->nbytes == 0) {
        IOMP_COMPLETE(aio, EINVAL);
        return;
    }
    aio->offset = 0;
    aio->execute = do_read;
    do_post(iomp, aio);
}

void iomp_write(iomp_t iomp, iomp_aio_t aio) {
    if (!aio || !aio->complete || !aio->release) {
        IOMP_LOG(ERROR, "invalid argument");
        return;
    }
    iomp_addref(&aio->refcnt);
    if (!iomp || !aio->buf || aio->nbytes == 0) {
        IOMP_COMPLETE(aio, EINVAL);
        return;
    }
    aio->offset = 0;
    aio->execute = do_write;
    do_post(iomp, aio);
}

void* do_work(void* arg) {
    iomp_t iomp = (iomp_t)arg;
    iomp_evlist_t evs = iomp_evlist_new(128);
    if (!evs) {
        IOMP_LOG(ERROR, "iomp_evlist_new fail: %s", strerror(errno));
        return NULL;
    }
    int stop = 0;
    pthread_mutex_lock(&iomp->lock);
    while (!stop) {
        while (STAILQ_EMPTY(&iomp->jobs)) {
            do_wait(iomp, evs);
        }
        iomp_aio_t aio = STAILQ_FIRST(&iomp->jobs);
        STAILQ_REMOVE_HEAD(&iomp->jobs, entries);
        pthread_mutex_unlock(&iomp->lock);
        aio->execute(iomp, aio);
        if (aio == &iomp->stop) {
            stop = 1;
        }
        pthread_mutex_lock(&iomp->lock);
    }
    if (iomp->nthreads == 0) {
        iomp_aio_t aio = NULL;
        while (!STAILQ_EMPTY(&iomp->jobs)) {
            aio = STAILQ_FIRST(&iomp->jobs);
            STAILQ_REMOVE_HEAD(&iomp->jobs, entries);
            IOMP_COMPLETE(aio, -1);
        }
    }
    pthread_mutex_unlock(&iomp->lock);
    iomp_evlist_drop(evs);
    return NULL;
}

void do_post(iomp_t iomp, iomp_aio_t aio) {
    pthread_mutex_lock(&iomp->lock);
    STAILQ_INSERT_TAIL(&iomp->jobs, aio, entries);
    if (iomp->blocked == iomp->nthreads) {
        do_interrupt(iomp);
    }
    pthread_mutex_unlock(&iomp->lock);
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
                continue;
            }
            if (ev.flags & IOMP_EVENT_READ) {
                on_read(iomp, &ev);
            }
            if (ev.flags & IOMP_EVENT_WRITE) {
                on_write(iomp, &ev);
            }
        }
    }
    pthread_mutex_lock(&iomp->lock);
    iomp->blocked--;
}

void do_stop(iomp_t iomp, iomp_aio_t aio) {
    pthread_mutex_lock(&iomp->lock);
    if (--iomp->nthreads > 0) {
        STAILQ_INSERT_TAIL(&iomp->jobs, aio, entries);
        if (iomp->blocked == iomp->nthreads) {
            do_interrupt(iomp);
        }
    }
    pthread_mutex_unlock(&iomp->lock);
}

void do_interrupt(iomp_t iomp) {
    int data = 0;
    write(iomp->intr[1], &data, sizeof(data));
}

void do_read(iomp_t iomp, iomp_aio_t aio) {
    while (1) {
        size_t todo = aio->nbytes - aio->offset;
        ssize_t len = read(aio->fildes, aio->buf + aio->offset, todo);
        int err = errno;
        IOMP_LOG(DEBUG, "read(%d, %p+%zu, %zu-%zu) -> (%zd, %d)",
                aio->fildes,
                aio->buf, aio->offset,
                aio->nbytes, aio->offset,
                len, err);
        errno = err;
        if (len == todo) {
            aio->offset += len;
            IOMP_COMPLETE(aio, 0);
            break;
        } else if (len > 0) {
            aio->offset += len;
        } else if (len == -1 && errno == EAGAIN) {
            struct iomp_event ev;
            IOMP_EVENT_SET(
                &ev,
                aio->fildes,
                IOMP_EVENT_READ | IOMP_EVENT_ONCE,
                todo,
                aio
            );
            IOMP_LOG(DEBUG, "add to queue %d", ev.ident);
            if (iomp_queue_add(iomp->queue, &ev) == -1) {
                IOMP_LOG(WARNING, "add to queue %d fail", ev.ident);
                IOMP_COMPLETE(aio, errno);
            }
            break;
        } else {
            IOMP_COMPLETE(aio, (len == -1 ? errno : -1));
            break;
        }
    }
}

void on_read(iomp_t iomp, iomp_event_t ev) {
    iomp_aio_t aio = (iomp_aio_t)ev->udata;
    while (1) {
        size_t todo = aio->nbytes - aio->offset;
        ssize_t len = read(aio->fildes, aio->buf + aio->offset, todo);
        IOMP_LOG(DEBUG, "read(%d, %p + %zu, %zu - %zu) -> (%zd, %s)",
                aio->fildes, aio->buf, aio->offset, aio->nbytes, aio->offset,
                len, len == -1 ? strerror(errno) : (len == 0 ? "eof" : "ok"));
        if (len == todo) {
            //iomp_queue_del(iomp->queue, aio->fildes, IOMP_EVENT_READ);
            IOMP_COMPLETE(aio, 0);
            break;
        //} else if (len > 0) {
        //    aio->offset += len;
        } else if (len == -1 && errno == EAGAIN) {
            break;
        } else {
            int err = errno;
            //iomp_queue_del(iomp->queue, aio->fildes, IOMP_EVENT_READ);
            IOMP_COMPLETE(aio, (len == -1 ? err : -1));
            break;
        }
    }
}

void do_write(iomp_t iomp, iomp_aio_t aio) {
    while (1) {
        size_t todo = aio->nbytes - aio->offset;
        ssize_t len = write(aio->fildes, aio->buf + aio->offset, todo);
        if (len == todo) {
            aio->offset += len;
            IOMP_COMPLETE(aio, 0);
            break;
        //} else if (len > 0) {
        //    aio->offset += len;
        } else if (len == -1 && errno == EAGAIN) {
            struct iomp_event ev;
            IOMP_EVENT_SET(
                &ev,
                aio->fildes,
                IOMP_EVENT_WRITE | IOMP_EVENT_ONCE,
                todo,
                aio
            );
            if (iomp_queue_add(iomp->queue, &ev) == -1) {
                IOMP_COMPLETE(aio, errno);
            }
            break;
        } else {
            IOMP_COMPLETE(aio, (len == -1 ? errno : -1));
            break;
        }
    }
}

void on_write(iomp_t iomp, iomp_event_t ev) {
    iomp_aio_t aio = (iomp_aio_t)ev->udata;
    while (1) {
        size_t todo = aio->nbytes - aio->offset;
        ssize_t len = write(aio->fildes, aio->buf + aio->offset, todo);
        IOMP_LOG(DEBUG, "write(%d, %p + %zu, %zu - %zu) -> (%zd, %s)",
                aio->fildes, aio->buf, aio->offset, aio->nbytes, aio->offset,
                len, len == -1 ? strerror(errno) : (len == 0 ? "eof" : "ok"));
        if (len == todo) {
            //iomp_queue_del(iomp->queue, aio->fildes, IOMP_EVENT_WRITE);
            IOMP_COMPLETE(aio, 0);
            break;
        } else if (len > 0) {
            aio->offset += len;
        } else if (len == -1 && errno == EAGAIN) {
            break;
        } else {
            int err = errno;
            //iomp_queue_del(iomp->queue, aio->fildes, IOMP_EVENT_WRITE);
            IOMP_COMPLETE(aio, (len == -1 ? err : -1));
            break;
        }
    }
}

