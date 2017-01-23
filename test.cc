#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <atomic>
#include <memory>
#include <vector>
#include <functional>
#include <thread>
#include <future>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include "iomp.h"

typedef struct { char x[1024]; } data_type;

static bool g_loop = true;
static std::atomic<uint64_t> rcnt { 0 };
static std::atomic<uint64_t> wcnt { 0 };

class Reader : public ::iomp::AsyncIO {
public:
    inline Reader(int sock, ::iomp::IOMultiPlexer& iomp) noexcept:
            ::iomp::AsyncIO(sock, &_data, sizeof(_data)), _iomp(iomp) {
        memset(&_data, 0, sizeof(_data));
    }
public:
    virtual void complete(int error) noexcept {
        if (error == 0) {
            rcnt++;
            _iomp.read(this);
        } else {
            if (error != -1) {
                IOMP_LOG(ERROR, "read fail: %s", strerror(error));
            }
            close(this->fildes);
            _promise.set_value();
            delete this;
        }
    }
    std::future<void> get_future() {
        return _promise.get_future();
    }
private:
    ::iomp::IOMultiPlexer& _iomp;
    data_type _data;
    std::promise<void> _promise;
};

class Writer : public ::iomp::AsyncIO {
public:
    inline Writer(int sock, ::iomp::IOMultiPlexer& iomp) noexcept:
            ::iomp::AsyncIO(sock, &_data, sizeof(_data)), _iomp(iomp) {
        memset(&_data, 0, sizeof(_data));
    }
public:
    virtual void complete(int error) noexcept {
        if (error == 0 && g_loop) {
            wcnt++;
            _iomp.write(this);
        } else {
            if (error != 0) {
                IOMP_LOG(ERROR, "write fail: %s",
                    error == -1 ? "eof" : strerror(error));
            }
            close(this->fildes);
            _promise.set_value();
            delete this;
        }
    }
    std::future<void> get_future() {
        return _promise.get_future();
    }
private:
    ::iomp::IOMultiPlexer& _iomp;
    data_type _data;
    std::promise<void> _promise;
};

class Acceptor : public ::iomp::AsyncIO {
public:
    Acceptor(const char* host, const char* port, ::iomp::IOMultiPlexer& iomp) noexcept:
            ::iomp::AsyncIO(-1, nullptr, 0),
            _iomp(iomp) {
        struct addrinfo hint = {
            0, AF_INET, SOCK_STREAM, IPPROTO_TCP, 0, NULL, NULL, NULL,
        };
        struct addrinfo* ai = nullptr;
        getaddrinfo(host, port, &hint, &ai);
        fildes = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        bind(fildes, ai->ai_addr, ai->ai_addrlen);
        freeaddrinfo(ai);
        listen(fildes, 1024);
        fcntl(fildes, F_SETFL, fcntl(fildes, F_GETFL, 0) | O_NONBLOCK);
    }
    inline ~Acceptor() noexcept {
        this->close();
        for (auto& f : _waits) {
            f.wait();
        }
    }
public:
    inline operator int() noexcept { return fildes; }
    inline void close() noexcept {
        if (fildes != -1) {
            ::close(fildes);
            fildes = -1;
        }
    }
public:
    virtual void complete(int error) noexcept {
        if (error != 0) {
            IOMP_LOG(ERROR, "add accept event fail: %s", strerror(error));
            this->close();
            return;
        }
        if (!_lock.try_lock()) {
            return;
        }
        while (1) {
            int remote = accept4(fildes, nullptr, nullptr, SOCK_NONBLOCK);
            if (remote == -1) {
                if (errno == EAGAIN) {
                    break;
                } else if (errno == ECONNABORTED) {
                    continue;
                } else {
                    IOMP_LOG(WARNING, "accept fail: %s", strerror(errno));
                    this->close();
                    break;
                }
            } else {
                IOMP_LOG(DEBUG, "accept %d", remote);
                ::close(remote);
                int sv[2] = { -1, -1 };
                socketpair(AF_LOCAL, SOCK_STREAM | SOCK_NONBLOCK, 0, sv);
                auto r = new Reader(sv[0], _iomp);
                _waits.push_back(r->get_future());
                _iomp.read(r);
                auto w = new Writer(sv[1], _iomp);
                _waits.push_back(w->get_future());
                _iomp.write(w);
            }
        }
        _lock.unlock();
    }
private:
    ::iomp::IOMultiPlexer& _iomp;
    std::mutex _lock;
    std::vector<std::future<void>> _waits;
};

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, [](int sig) noexcept {
        g_loop = false;
    });
    ::iomp_loglevel(IOMP_LOGLEVEL_DEBUG);
    ::iomp::IOMultiPlexer iomp;
    Acceptor accp { "0.0.0.0", "8643", iomp };
    ::iomp_accept(iomp, &accp);
#if 0
    std::vector<std::future<void>> waits;
    for (int i = 0; i < 10; i++) {
        int sv[2] = { -1, -1 };
        socketpair(AF_LOCAL, SOCK_STREAM | SOCK_NONBLOCK, 0, sv);
        auto r = new Reader(sv[0], iomp);
        waits.push_back(r->get_future());
        iomp.read(r);
        auto w = new Writer(sv[1], iomp);
        waits.push_back(w->get_future());
        iomp.write(w);
    }
#endif
    while (g_loop) {
        sleep(1);
        auto rqps = rcnt.exchange(0);
        auto wqps = wcnt.exchange(0);
        auto qps = std::min(rqps, wqps);
        IOMP_LOG(NOTICE, "%zu/%zu qps, %zu bit, %.2f Mbps",
                (size_t)rqps, (size_t)wqps, sizeof(data_type),
                qps / 1024.0 / 1024.0 * sizeof(data_type) * 8);
    }
#if 0
    for (auto& f : waits) {
        f.wait();
    }
#endif
    return 0;
}

