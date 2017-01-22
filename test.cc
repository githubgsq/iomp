#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <atomic>
#include <memory>
#include <vector>
#include <functional>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include "iomp.h"
#include "helper.h"

typedef struct { char x[1024]; } data_type;

static bool g_loop = true;
static std::atomic<uint64_t> rcnt { 0 };
static std::atomic<uint64_t> wcnt { 0 };

class Reader : public ::iomp::AsyncIO {
public:
    inline Reader(int sock, ::iomp::IOMultiPlexer& iomp) noexcept:
        ::iomp::AsyncIO(sock, &_data, sizeof(_data)), _iomp(iomp) { }
public:
    virtual void complete(int error) noexcept {
        if (error == 0) {
            rcnt++;
            _iomp.read(this);
        } else {
            IOMP_LOG(ERROR, "read fail: %s",
                    error == -1 ? "eof" : strerror(error));
            close(this->fildes);
            delete this;
        }
    }
private:
    ::iomp::IOMultiPlexer& _iomp;
    data_type _data;
};

class Writer : public ::iomp::AsyncIO {
public:
    inline Writer(int sock, ::iomp::IOMultiPlexer& iomp) noexcept:
        ::iomp::AsyncIO(sock, &_data, sizeof(_data)), _iomp(iomp) { }
public:
    virtual void complete(int error) noexcept {
        if (error == 0) {
            wcnt++;
            _iomp.write(this);
        } else {
            IOMP_LOG(ERROR, "write fail: %s",
                    error == -1 ? "eof" : strerror(error));
            close(this->fildes);
            delete this;
        }
    }
private:
    ::iomp::IOMultiPlexer& _iomp;
    data_type _data;
};

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, [](int sig) noexcept {
        g_loop = false;
    });
    ::iomp_loglevel(IOMP_LOGLEVEL_DEBUG);
    ::iomp::IOMultiPlexer iomp { 0 };
    for (int i = 0; i < 10; i++) {
        int sv[2] = { -1, -1 };
        socketpair(AF_LOCAL, SOCK_STREAM | SOCK_NONBLOCK, 0, sv);
        iomp.read(new Reader(sv[0], iomp));
        iomp.write(new Writer(sv[1], iomp));
    }
    while (g_loop) {
        sleep(1);
        auto rqps = rcnt.exchange(0);
        auto wqps = wcnt.exchange(0);
        auto qps = std::min(rqps, wqps);
        IOMP_LOG(NOTICE, "%zu/%zu qps, %zu bit, %.2f Mbps",
                (size_t)rqps, (size_t)wqps, sizeof(data_type),
                qps / 1024.0 / 1024.0 * sizeof(data_type) * 8);
    }
    return 0;
}

