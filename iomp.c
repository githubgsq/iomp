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
#include "iomp_queue.h"
#include "iomp.h"

#define IOMP_COMPLETE(aio, err) \
    do { \
        (aio)->complete((aio), (err)); \
        if (iomp_release(&(aio)->refcnt) == 0) { \
            (aio)->release((aio)); \
        } \
    } while (0)

struct iomp_thread {
    TAILQ_ENTRY(iomp_thread) entries;
    iomp_t iomp;
    pthread_t thread;
    iomp_queue_t queue;
};
typedef struct iomp_thread* iomp_thread_t;

struct iomp_core {
    pthread_mutex_t lock;
    STAILQ_HEAD(, iomp_aio) jobs;
    struct iomp_aio stop;
    TAILQ_HEAD(, iomp_thread) actived;
    TAILQ_HEAD(, iomp_thread) blocked;
    int nthreads;
};

static int get_ncpu();

static iomp_thread_t iomp_thread_new(iomp_t iomp, int nevents);
static void iomp_thread_drop(iomp_thread_t t);
static void* iomp_thread_run(void* arg);

static void do_post(iomp_t iomp, iomp_aio_t aio);

static void do_stop(iomp_t iomp, iomp_queue_t q, iomp_aio_t aio);
static void do_read(iomp_t iomp, iomp_queue_t q, iomp_aio_t aio);
static void do_write(iomp_t iomp, iomp_queue_t q, iomp_aio_t aio);

iomp_t iomp_new(int nthreads) {
    if (nthreads <= 0) {
        nthreads = get_ncpu();
    }
    if (nthreads <= 0) {
        errno = EINVAL;
        return NULL;
    }
    iomp_t iomp = (iomp_t)malloc(sizeof(*iomp));
    if (!iomp) {
        IOMP_LOG(ERROR, "malloc fail: %s", strerror(errno));
        return NULL;
    }
    STAILQ_INIT(&iomp->jobs);
    TAILQ_INIT(&iomp->actived);
    TAILQ_INIT(&iomp->blocked);
    iomp->stop.execute = do_stop;
    iomp->nthreads = 0;
    int rv = pthread_mutex_init(&iomp->lock, NULL);
    if (rv != 0) {
        IOMP_LOG(ERROR, "pthread_mutex_init fail: %s", strerror(rv));
        free(iomp);
        return NULL;
    }
    pthread_mutex_lock(&iomp->lock);
    for (int i = 0; i < nthreads; i++) {
        iomp_thread_t t = iomp_thread_new(iomp, IOMP_EVENT_LIMIT);
        if (t) {
            TAILQ_INSERT_TAIL(&iomp->actived, t, entries);
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
    iomp_thread_t t = NULL;
    TAILQ_FOREACH(t, &iomp->actived, entries) {
        iomp_thread_drop(t);
    }
    pthread_mutex_destroy(&iomp->lock);
    free(iomp);
}

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

iomp_thread_t iomp_thread_new(iomp_t iomp, int nevents) {
    iomp_thread_t t = (iomp_thread_t)malloc(sizeof(*t));
    if (!t) {
        IOMP_LOG(ERROR, "malloc fail: %s", strerror(errno));
        return NULL;
    }
    t->iomp = iomp;
    t->queue = iomp_queue_new(nevents);
    if (!t->queue) {
        IOMP_LOG(ERROR, "iomp_queue_new fail");
        free(t);
        return NULL;
    }
    int rv = pthread_create(&t->thread, NULL, iomp_thread_run, t);
    if (rv == -1) {
        IOMP_LOG(ERROR, "pthread_create fail: %s", strerror(errno));
        iomp_queue_drop(t->queue);
        free(t);
        return NULL;
    }
    return t;
}

void iomp_thread_drop(iomp_thread_t t) {
    if (!t) {
        return;
    }
    pthread_join(t->thread, NULL);
    iomp_queue_drop(t->queue);
    free(t);
}

void* iomp_thread_run(void* arg) {
    iomp_thread_t t = (iomp_thread_t)arg;
    iomp_t iomp = t->iomp;
    int stop = 0;
    pthread_mutex_lock(&iomp->lock);
    while (!stop) {
        while (STAILQ_EMPTY(&iomp->jobs)) {
            TAILQ_REMOVE(&iomp->actived, t, entries);
            TAILQ_INSERT_TAIL(&iomp->blocked, t, entries);
            pthread_mutex_unlock(&iomp->lock);
            iomp_queue_run(t->queue, -1);
            pthread_mutex_lock(&iomp->lock);
            TAILQ_REMOVE(&iomp->blocked, t, entries);
            TAILQ_INSERT_TAIL(&iomp->actived, t, entries);
        }
        iomp_aio_t aio = STAILQ_FIRST(&iomp->jobs);
        STAILQ_REMOVE_HEAD(&iomp->jobs, entries);
        pthread_mutex_unlock(&iomp->lock);
        aio->execute(iomp, t->queue, aio);
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
    return NULL;
}

void do_post(iomp_t iomp, iomp_aio_t aio) {
    pthread_mutex_lock(&iomp->lock);
    STAILQ_INSERT_TAIL(&iomp->jobs, aio, entries);
    if (TAILQ_EMPTY(&iomp->actived)) {
        iomp_thread_t t = TAILQ_FIRST(&iomp->blocked);
        iomp_queue_interrupt(t->queue);
    }
    pthread_mutex_unlock(&iomp->lock);
}

void do_stop(iomp_t iomp, iomp_queue_t q, iomp_aio_t aio) {
    pthread_mutex_lock(&iomp->lock);
    if (--iomp->nthreads > 0) {
        STAILQ_INSERT_TAIL(&iomp->jobs, aio, entries);
        if (TAILQ_EMPTY(&iomp->actived)) {
            iomp_thread_t t = TAILQ_FIRST(&iomp->blocked);
            iomp_queue_interrupt(t->queue);
        }
    }
    pthread_mutex_unlock(&iomp->lock);
}

void do_read(iomp_t iomp, iomp_queue_t q, iomp_aio_t aio) {
    while (1) {
        size_t todo = aio->nbytes - aio->offset;
        ssize_t len = read(aio->fildes, aio->buf + aio->offset, todo);
#if 0
        IOMP_LOG(NOTICE, "read(%d, %p+%zu, %zu-%zu) -> (%zd, %d)",
                aio->fildes,
                aio->buf, aio->offset,
                aio->nbytes, aio->offset,
                len, err);
#endif
        if (len == todo) {
            aio->offset += len;
            IOMP_COMPLETE(aio, 0);
            break;
        } else if (len > 0) {
            aio->offset += len;
        } else if (len == -1 && errno == EAGAIN) {
            if (iomp_queue_read(q, aio) == -1) {
                IOMP_COMPLETE(aio, errno);
            }
            break;
        } else {
            IOMP_COMPLETE(aio, (len == -1 ? errno : -1));
            break;
        }
    }
}

void do_write(iomp_t iomp, iomp_queue_t q, iomp_aio_t aio) {
    while (1) {
        size_t todo = aio->nbytes - aio->offset;
        ssize_t len = write(aio->fildes, aio->buf + aio->offset, todo);
        if (len == todo) {
            aio->offset += len;
            IOMP_COMPLETE(aio, 0);
            break;
        } else if (len > 0) {
            aio->offset += len;
        } else if (len == -1 && errno == EAGAIN) {
            if (iomp_queue_write(q, aio) == -1) {
                IOMP_COMPLETE(aio, errno);
            }
            break;
        } else {
            IOMP_COMPLETE(aio, (len == -1 ? errno : -1));
            break;
        }
    }
}

int get_ncpu() {
    int ncpu = -1;
#if defined(__BSD__)
    int mib[2] = { CTL_HW, HW_NCPU };
    size_t len = sizeof(ncpu);
    int rv = sysctl(mib, 2, &ncpu, &len, NULL, 0);
    if (rv != 0) {
        IOMP_LOG(ERROR, "sysctlbyname fail: %s", strerror(rv));
        ncpu = -1;
    }
#elif defined(__linux__)
    ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu == -1) {
        IOMP_LOG(ERROR, "sysconf fail: %s", strerror(rv));
    }
#endif
    return ncpu;
}

