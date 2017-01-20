#ifndef IOMP_H
#define IOMP_H

#include <stddef.h>
#include <stdint.h>
#include <sys/queue.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define IOMP_API __attribute__((visibility("default")))

#define IOMP_LOGLEVEL_DEBUG     1
#define IOMP_LOGLEVEL_INFO      2
#define IOMP_LOGLEVEL_NOTICE    3
#define IOMP_LOGLEVEL_WARNING   4
#define IOMP_LOGLEVEL_ERROR     5
#define IOMP_LOGLEVEL_FATAL     6

#define IOMP_LOG(level, fmt, ...) \
    do { \
        char __buf[27] = {'\0'}; \
        iomp_writelog(IOMP_LOGLEVEL_##level, \
                "[libiomp] " #level " %s " __FILE__ ":%s:%d " fmt "\n", \
                iomp_now(__buf, sizeof(__buf)), \
                __func__, __LINE__, ##__VA_ARGS__); \
    } while (0)

IOMP_API const char* iomp_now(char* buf, size_t bufsz);
IOMP_API int iomp_writelog(int level, const char* fmt, ...);
IOMP_API int iomp_loglevel(int level);

struct iomp_core;
typedef struct iomp_core* iomp_t;

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
    inline void write(int fildes, void* buf, size_t nbytes,
            std::function<void(AsyncIO&, int)> complete,
            int timeout=-1) noexcept {
        ::iomp_write(_iomp,
                new AsyncIO(fildes, buf, nbytes, complete, timeout));
    }
    inline void write(AsyncIO& aio) noexcept {
        ::iomp_write(_iomp, &aio);
    }
private:
    ::iomp_t _iomp;
};

} /* namespace iomp */

#endif /* __cplusplus */

#endif /* IOMP_H */

