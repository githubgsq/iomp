#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include "iomp.h"

#define IOMP_LOG(fmt, ...) \
    fprintf(stderr, "[libiomp] " __FILE__ ":%d " fmt "\n", \
            __LINE__, ##__VA_ARGS__)

#define IOMP_EVENT_LIMIT 1024

struct iomp_core {
    pthread_mutex_t lock;
    pthread_t* threads;
    int nthread;
    int eventfd;
    struct kevent events[IOMP_EVENT_LIMIT];
    uint64_t blocked;
    int intr[2];
    unsigned stop:1;
};

static void* do_work(void* arg);
static void do_wait(iomp_t iomp, struct timespec* timeout);
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
    iomp->eventfd = kqueue();
    if (iomp->eventfd == -1) {
        IOMP_LOG("kqueue fail: %s", strerror(errno));
        free(iomp);
        return NULL;
    }
    int rv = pipe(iomp->intr);
    if (rv != 0) {
        IOMP_LOG("pipe fail: %s", strerror(errno));
        close(iomp->eventfd);
        free(iomp);
        return NULL;
    }
    fcntl(iomp->intr[0], F_SETFL, fcntl(iomp->intr[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(iomp->intr[1], F_SETFL, fcntl(iomp->intr[1], F_GETFL, 0) | O_NONBLOCK);
    struct kevent ev;
    EV_SET(&ev, iomp->intr[0], EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, iomp);
    if (kevent(iomp->eventfd, &ev, 1, NULL, 0, NULL) == -1) {
        IOMP_LOG("kevent fail: %s", strerror(errno));
        close(iomp->intr[1]);
        close(iomp->intr[0]);
        close(iomp->eventfd);
        free(iomp);
        return NULL;
    }
    rv = pthread_mutex_init(&iomp->lock, NULL);
    if (rv != 0) {
        IOMP_LOG("pthread_mutex_init fail: %s", strerror(rv));
        close(iomp->intr[1]);
        close(iomp->intr[0]);
        close(iomp->eventfd);
        free(iomp);
        return NULL;
    }
    pthread_mutex_lock(&iomp->lock);
    if (nthread <= 0) {
        int mib[2] = { CTL_HW, HW_NCPU };
        int ncpu = 0;
        size_t len = sizeof(ncpu);
        rv = sysctl(mib, 2, &ncpu, &len, NULL, 0);
        if (rv != 0) {
            IOMP_LOG("sysctlbyname fail: %s", strerror(rv));
            close(iomp->intr[1]);
            close(iomp->intr[0]);
            close(iomp->eventfd);
            free(iomp);
            return NULL;
        }
        nthread = ncpu;
    }
    iomp->nthread = 0;
    iomp->threads = (pthread_t*)malloc(sizeof(*iomp->threads) * nthread);
    if (!iomp->threads) {
        IOMP_LOG("malloc fail: %s", strerror(errno));
        pthread_mutex_destroy(&iomp->lock);
        close(iomp->intr[1]);
        close(iomp->intr[0]);
        close(iomp->eventfd);
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
    close(iomp->eventfd);
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

void iomp_read(iomp_t iomp, const iomp_aio_t aio) {
    if (!aio || !aio->complete) {
        IOMP_LOG("invalid argument");
        return;
    }
    if (!iomp) {
        aio->error = EINVAL;
        aio->complete(aio, 0);
        return;
    }
    int val = fcntl(aio->fildes, F_GETFL, 0);
    if (val == -1) {
        aio->error = errno;
        aio->complete(aio, 0);
        return;
    }
    if (fcntl(aio->fildes, F_SETFL, val | O_NONBLOCK) == -1) {
        aio->error = errno;
        aio->complete(aio, 0);
        return;
    }
    struct kevent ev;
    EV_SET(&ev,
            aio->fildes, EVFILT_READ, EV_ADD | EV_ONESHOT, 0, aio->nbytes, aio);
    if (kevent(iomp->eventfd, &ev, 1, NULL, 0, NULL) == -1) {
        aio->error = errno;
        aio->complete(aio, 0);
        return;
    }
}

void* do_work(void* arg) {
    iomp_t iomp = (iomp_t)arg;
    pthread_mutex_lock(&iomp->lock);
    while (!iomp->stop) {
        do_wait(iomp, NULL);
    }
    if (iomp->blocked) {
        do_interrupt(iomp);
    }
    pthread_mutex_unlock(&iomp->lock);
    return NULL;
}

void do_wait(iomp_t iomp, struct timespec* timeout) {
    iomp->blocked++;
    pthread_mutex_unlock(&iomp->lock);
    int nevent = kevent(iomp->eventfd,
            NULL, 0, iomp->events, IOMP_EVENT_LIMIT, timeout);
    for (int i = 0; i < nevent; i++) {
        struct kevent* ev = iomp->events + i;
        /*if (ev->filter == EVFILT_SIGNAL) {
            iomp_signal_t sig = (iomp_signal_t)ev->udata;
            sig->ready(sig, ev->data);
        } else */
        if (ev->ident == iomp->intr[0]) {
            int buf = 0;
            read(iomp->intr[0], &buf, sizeof(buf));
            IOMP_LOG("interrupted");
        } else if (ev->filter == EVFILT_READ) {
            iomp_aio_t aio = (iomp_aio_t)ev->udata;
            ssize_t len = read(aio->fildes, aio->buf, aio->nbytes);
            if (len == aio->nbytes) {
                aio->complete(aio, 1);
            } else {
                aio->error = (len == -1 ? errno : 0);
                aio->complete(aio, 0);
            }
        }
    }
    pthread_mutex_lock(&iomp->lock);
    iomp->blocked--;
    if (nevent < 0) {
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

