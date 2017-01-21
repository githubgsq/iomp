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

static bool g_loop = true;

class Session {
public:
    typedef struct { uint8_t x[1024]; } Data;
    inline Session(::iomp::IOMultiPlexer& iomp,
                std::atomic<uint64_t>& rcnt,
                std::atomic<uint64_t>& wcnt) noexcept:
            _iomp(iomp), _rcnt(rcnt), _wcnt(wcnt) {
        auto& seq = *reinterpret_cast<uint64_t*>(&_wbuf);
        seq = 0xdeadc00dfacebabe;
        int sv[2] = { -1, -1 };
        if (socketpair(AF_LOCAL, SOCK_STREAM, 0, sv) != 0) {
            IOMP_LOG(ERROR, "socketpair fail: %s", strerror(errno));
            return;
        }
#if 1
        fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL, 0) | O_NONBLOCK);
        iomp.read(sv[0], &_rbuf, sizeof(_rbuf),
                std::bind(std::mem_fn(&Session::on_read), this,
                    std::placeholders::_1, std::placeholders::_2));
#else
        std::thread t0 { [this](int sock) {
            while (1) {
                auto len = read(sock, &this->_rbuf, sizeof(this->_rbuf));
                if (len != sizeof(this->_rbuf)) {
                    break;
                }
                this->_rcnt++;
            }
            close(sock);
        }, sv[0] };
        t0.detach();
#endif
#if 1
        fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL, 0) | O_NONBLOCK);
        iomp.write(sv[1], &_wbuf, sizeof(_wbuf),
                std::bind(std::mem_fn(&Session::on_write), this,
                    std::placeholders::_1, std::placeholders::_2));
#else
        std::thread t1 { [this](int sock) {
            while (1) {
                auto len = write(sock, &this->_wbuf, sizeof(this->_wbuf));
                if (len != sizeof(this->_wbuf)) {
                    break;
                }
                //IOMP_LOG(DEBUG, "client write done");
                this->_wcnt++;
            }
            close(sock);
        }, sv[1] };
        t1.detach();
#endif
    }
    Session(const Session&) noexcept = delete;
    Session& operator=(const Session&) noexcept = delete;
    Session(Session&&) noexcept = delete;
    Session& operator=(Session&&) noexcept = delete;
public:
    void on_read(::iomp::AsyncIO& aio, int error) noexcept {
        if (error == 0) {
            _rcnt++;
            //auto seq = *reinterpret_cast<uint64_t*>(&_rbuf);
            //IOMP_LOG(WARNING, "read 0x%lx", seq);
            _iomp.read(aio);
        } else {
            if (error > 0) {
                IOMP_LOG(ERROR, "read fail: %s", strerror(error));
            } else {
                IOMP_LOG(NOTICE, "read end");
            }
            close(aio);
        }
    }
    void on_write(::iomp::AsyncIO& aio, int error) noexcept {
        if (error == 0) {
            _wcnt++;
            //auto& seq = *reinterpret_cast<uint64_t*>(&_wbuf);
            //IOMP_LOG(WARNING, "write 0x%lx", seq);
            //seq++;
            //struct timespec ts = { 0, 1000000000 }; nanosleep(&ts, nullptr);
            _iomp.write(aio);
        } else {
            if (error > 0) {
                IOMP_LOG(ERROR, "write fail: %s", strerror(error));
            } else {
                IOMP_LOG(NOTICE, "write end");
            }
            close(aio);
        }
    }
private:
    ::iomp::IOMultiPlexer& _iomp;
    std::atomic<uint64_t>& _rcnt;
    std::atomic<uint64_t>& _wcnt;
    Data _rbuf;
    Data _wbuf;
};

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, [](int sig) noexcept {
        g_loop = false;
    });
    std::atomic<uint64_t> rcnt { 0 };
    std::atomic<uint64_t> wcnt { 0 };
    std::vector<std::unique_ptr<Session>> sess;
    ::iomp_loglevel(IOMP_LOGLEVEL_DEBUG);
    ::iomp::IOMultiPlexer iomp { 0 };
    for (int i = 0; i < 16; i++) {
        sess.emplace_back(std::unique_ptr<Session>(new Session(iomp, rcnt, wcnt)));
    }
    while (g_loop) {
        sleep(1);
        auto rqps = rcnt.exchange(0);
        auto wqps = wcnt.exchange(0);
        auto qps = std::min(rqps, wqps);
        IOMP_LOG(NOTICE, "%zu/%zu qps, %zu bit, %.2f Mbps",
                (size_t)rqps, (size_t)wqps, sizeof(Session::Data),
                qps / 1024.0 / 1024.0 * sizeof(Session::Data) * 8);
    }
    return 0;
}

