#ifndef IOMP_EVENT_H
#define IOMP_EVENT_H

#include <stdint.h>
#include <stddef.h>
#include <sys/queue.h>

struct iomp_aio;
struct iomp_thread;

struct iomp_aiojb {
    STAILQ_ENTRY(iomp_aiojb) entries;
    struct iomp_aio* aio;
    size_t offset;
    void (*execute)(struct iomp_aiojb* job, struct iomp_thread* thread);
};
typedef struct iomp_aiojb* iomp_aiojb_t;

struct iomp_queue;
typedef struct iomp_queue* iomp_queue_t;

iomp_queue_t iomp_queue_new();
void iomp_queue_drop(iomp_queue_t q);

struct iomp_aio;
int iomp_queue_read(iomp_queue_t q, iomp_aiojb_t job);
int iomp_queue_write(iomp_queue_t q, iomp_aiojb_t job);

int iomp_queue_run(iomp_queue_t q, int timeout);
void iomp_queue_interrupt(iomp_queue_t q);

#if 0
struct iomp_evlist;
typedef struct iomp_evlist* iomp_evlist_t;

iomp_evlist_t iomp_evlist_new(int nevents);
void iomp_evlist_drop(iomp_evlist_t evs);
int iomp_evlist_next(iomp_evlist_t evs, iomp_event_t ev);
#endif

#endif /* IOMP_EVENT_H */

