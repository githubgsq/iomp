#ifndef IOMP_H
#define IOMP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define IOMP_API __attribute__((visibility("default")))

struct iomp_core;
typedef struct iomp_core* iomp_t;

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

#include <functional>

namespace iomp {

class IOMultiPlexer;

class AsyncIO {
public:
    inline AsyncIO(int fildes, void* buf, size_t nbytes, int timeout_ms,
            std::function<void(AsyncIO&, bool)>&& complete) noexcept:
                _complete(complete) {
        _aio.fildes = fildes;
        _aio.buf = buf;
        _aio.nbytes = nbytes;
        _aio.timeout_ms = timeout_ms;
        _aio.complete = on_complete;
        _aio.error = 0;
    }
public:
    inline operator ::iomp_aio_t() noexcept { return &_aio; }
    inline operator const ::iomp_aio_t() const noexcept {
        return const_cast<const ::iomp_aio_t>(&_aio);
    }
    inline ::iomp_aio_t operator->() noexcept { return &_aio; }
    inline const ::iomp_aio_t operator->() const noexcept {
        return const_cast<const ::iomp_aio_t>(&_aio);
    }
private:
    static void on_complete(::iomp_aio_t aio, int succ) noexcept {
        auto self = reinterpret_cast<AsyncIO*>(aio);
        self->_complete(*self, succ);
    }
private:
    struct ::iomp_aio _aio;
    std::function<void(AsyncIO&, bool)> _complete;
};

class IOMultiPlexer {
public:
    inline explicit IOMultiPlexer(int nthread) noexcept:
        _iomp(::iomp_new(nthread)) { }
    inline ~IOMultiPlexer() noexcept {
        if (_iomp) {
            ::iomp_drop(_iomp);
        }
    }
public:
    inline explicit operator bool() noexcept { return _iomp != nullptr; }
    inline void read(const AsyncIO& aio) noexcept { ::iomp_read(_iomp, aio); }
private:
    ::iomp_t _iomp;
};

} /* namespace iomp */

#endif /* __cplusplus */

#endif /* IOMP_H */

