#ifndef IOMP_H
#define IOMP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define IOMP_API __attribute__((visibility("default")))

struct iomp;
typedef struct iomp* iomp_t;

#if 0
struct iomp_signal {
    int signal;
    int (*ready)(struct iomp_signal* sig, int ntime);
    void* udata;
};
typedef struct iomp_signal* iomp_signal_t;

struct iomp_accept {
    int fildes;
    int (*ready)(struct iomp_accept* accp, int sock);
    void* udata;
};
typedef struct iomp_accept* iomp_accept_t;
#endif

struct iomp_aio {
    int fildes;
    void* buf;
    size_t nbytes;
    int timeout_ms;
    void (*complete)(struct iomp_aio* aio, int succ);
    int error;
    void* udata;
};
typedef struct iomp_aio* iomp_aio_t;

IOMP_API iomp_t iomp_new(int nthread);

IOMP_API void iomp_drop(iomp_t iomp);

//IOMP_API int iomp_signal(iomp_t iomp, const iomp_signal_t sig);
//IOMP_API int iomp_accept(iomp_t iomp, const iomp_accept_t accp);
IOMP_API void iomp_read(iomp_t iomp, const iomp_aio_t aio);
IOMP_API int iomp_write(iomp_t iomp, const iomp_aio_t aio);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* IOMP_H */

