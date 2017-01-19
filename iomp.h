#ifndef IOMP_H
#define IOMP_H

#include <stddef.h>
#include <sys/queue.h>

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
    STAILQ_ENTRY(iomp_aio) entries;
    int fildes;
    void* buf;
    size_t nbytes;
    int timeout_ms;
    size_t offset;
    void (*execute)(iomp_t iomp, struct iomp_aio* aio);
    void (*complete)(struct iomp_aio* aio, int error);
    volatile uint64_t refcnt;
    void (*release)(struct iomp_aio*);
};
typedef struct iomp_aio* iomp_aio_t;

IOMP_API iomp_t iomp_new(int nthreads);

IOMP_API void iomp_drop(iomp_t iomp);

//IOMP_API int iomp_signal(iomp_t iomp, const iomp_signal_t sig);
//IOMP_API int iomp_accept(iomp_t iomp, const iomp_accept_t accp);
IOMP_API void iomp_read(iomp_t iomp, iomp_aio_t aio);
IOMP_API void iomp_write(iomp_t iomp, iomp_aio_t aio);

#ifdef __cplusplus
}

#include <functional>

namespace iomp {

class IOMultiPlexer;

class AsyncIO : public ::iomp_aio {
public:
    inline AsyncIO(int fildes, void* buf, size_t nbytes,
        std::function<void(AsyncIO&, int)> complete,
        int timeout_ms = -1) noexcept:
            ::iomp_aio({
                { nullptr },
                fildes, buf, nbytes, timeout_ms,
                0, nullptr, &AsyncIO::complete,
                0, &AsyncIO::release,
            }),
            _complete(complete) {
    }
    AsyncIO(const AsyncIO&) noexcept = delete;
    AsyncIO& operator=(const AsyncIO&) noexcept = delete;
    AsyncIO(AsyncIO&&) noexcept = delete;
    AsyncIO& operator=(AsyncIO&&) noexcept = delete;
public:
    inline operator int() noexcept { return fildes; }
private:
    static void complete(::iomp_aio_t aio, int error) noexcept {
        auto self = reinterpret_cast<AsyncIO*>(aio);
        self->_complete(*self, error);
    }
    static void release(::iomp_aio_t aio) noexcept {
        auto self = reinterpret_cast<AsyncIO*>(aio);
        delete self;
    }
private:
    std::function<void(AsyncIO&, int)> _complete;
};

class IOMultiPlexer {
public:
    inline IOMultiPlexer() noexcept: IOMultiPlexer(0) { }
    inline explicit IOMultiPlexer(int nthread) noexcept:
        _iomp(::iomp_new(nthread)) { }
    inline ~IOMultiPlexer() noexcept {
        if (_iomp) {
            ::iomp_drop(_iomp);
        }
    }
    inline IOMultiPlexer(IOMultiPlexer&& rhs) noexcept: _iomp(rhs._iomp) {
        rhs._iomp = nullptr;
    }
    inline IOMultiPlexer& operator=(IOMultiPlexer&& rhs) noexcept {
        _iomp = rhs._iomp;
        rhs._iomp = nullptr;
        return *this;
    }
public:
    inline explicit operator bool() noexcept { return _iomp != nullptr; }
    inline operator ::iomp_t() noexcept { return _iomp; }
    inline void read(int fildes, void* buf, size_t nbytes,
            std::function<void(AsyncIO&, int)> complete,
            int timeout=-1) noexcept {
        ::iomp_read(_iomp,
                new AsyncIO(fildes, buf, nbytes, complete, timeout));
    }
    inline void read(AsyncIO& aio) noexcept {
        ::iomp_read(_iomp, &aio);
    }
private:
    ::iomp_t _iomp;
};

} /* namespace iomp */

#endif /* __cplusplus */

#endif /* IOMP_H */

