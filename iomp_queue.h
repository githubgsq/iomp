#ifndef IOMP_EVENT_H
#define IOMP_EVENT_H

#include <stdint.h>
#include <stddef.h>

struct iomp_queue;
typedef struct iomp_queue* iomp_queue_t;

iomp_queue_t iomp_queue_new();
void iomp_queue_drop(iomp_queue_t q);

struct iomp_aio;
int iomp_queue_read(iomp_queue_t q, struct iomp_aio* aio);
int iomp_queue_write(iomp_queue_t q, struct iomp_aio* aio);

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

