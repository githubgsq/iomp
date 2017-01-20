#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <atomic>
#include <memory>
#include <vector>
#include <thread>
#include <functional>
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
        int sv[2] = { -1, -1 };
        if (socketpair(AF_LOCAL, SOCK_STREAM, 0, sv) != 0) {
            fprintf(stderr, "socketpair fail: %s\n", strerror(errno));
            return;
        }
#if 1
        fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL, 0) | O_NONBLOCK);
        iomp.read(sv[0], &_rbuf, sizeof(_rbuf),
                std::bind(std::mem_fn(&Session::on_read), this,
                    std::placeholders::_1, std::placeholders::_2));
#else
        std::thread thr1 { [buf, &rcnt](int sock) {
            while (1) {
                auto len = read(sock, buf, sizeof(*buf));
                if (len != sizeof(*buf)) {
                    break;
                }
                rcnt++;
            }
            delete buf;
            close(sock);
        }, sv[0]};
        thr1.detach();
#endif
#if 0
        std::thread thr2 { [&wcnt](int sock) {
            Data data;
            auto& seq = *reinterpret_cast<uint64_t*>(&data);
            seq = 0xdeadc00dfacebabe;
            size_t len = sizeof(data);
            setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &len, sizeof(len));
            while (1) {
                auto len = write(sock, &data, sizeof(data));
                if (len != sizeof(data)) {
                    break;
                }
                wcnt++;
                //IOMP_LOG(WARNING, "write %lx done", seq++);
                //struct timespec ts { 0, 100000000 };
                //nanosleep(&ts, nullptr);
            }
            //delete buf2;
            close(sock);
        }, sv[1]};
        thr2.detach();
#else
        fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL, 0) | O_NONBLOCK);
        iomp.write(sv[1], &_wbuf, sizeof(_wbuf),
                std::bind(std::mem_fn(&Session::on_write), this,
                    std::placeholders::_1, std::placeholders::_2));
#endif
    }
    Session(const Session&) noexcept = delete;
    Session& operator=(const Session&) noexcept = delete;
    Session(Session&&) noexcept = delete;
    Session& operator=(Session&&) noexcept = delete;
public:
    void on_read(::iomp::AsyncIO& aio, int error) noexcept {
        //IOMP_LOG(WARNING, "read(%d, %p, %zu) -> %d", aio.fildes, aio.buf, aio.nbytes, error);
        if (error == 0) {
            //nanosleep(&_delay, nullptr);
            _rcnt++;
            //auto seq = *reinterpret_cast<uint64_t*>(reinterpret_cast<Data*>(aio.buf));
            //auto seq2 = *reinterpret_cast<uint64_t*>(&_rbuf);
            //IOMP_LOG(WARNING, "read again seq=%p->%lx, %p->%lx -> %d", aio.buf, seq, &_rbuf, seq2, aio.fildes);
            _iomp.read(aio);
        } else {
            if (error > 0) {
                fprintf(stderr, "read fail: %s\n", strerror(error));
            } else {
                fprintf(stderr, "read end\n");
            }
            close(aio);
        }
    }
    void on_write(::iomp::AsyncIO& aio, int error) noexcept {
        if (error == 0) {
            _wcnt++;
            //nanosleep(&_delay, nullptr);
            auto& seq = *reinterpret_cast<uint64_t*>(&_wbuf);
            seq++;
            _iomp.write(aio);
        } else {
            if (error > 0) {
                fprintf(stderr, "write fail: %s\n", strerror(error));
            } else {
                fprintf(stderr, "write end\n");
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
    //::iomp_loglevel(IOMP_LOGLEVEL_DEBUG);
    ::iomp::IOMultiPlexer iomp { 2 };
    for (int i = 0; i < 1; i++) {
        sess.emplace_back(std::unique_ptr<Session>(new Session(iomp, rcnt, wcnt)));
    }
    while (g_loop) {
    //for (int i = 0; i < 5; i++) {
        //struct timespec ts { 0, 100000000 };
        struct timespec ts { 1, 0 };
        nanosleep(&ts, nullptr);
        auto rqps = rcnt.exchange(0);
        auto wqps = wcnt.exchange(0);
        auto qps = std::min(rqps, wqps);
        fprintf(stderr, "%zu/%zu qps, %zu bit, %.2f Mbps\n",
                (size_t)rqps, (size_t)wqps, sizeof(Session::Data),
                qps / 1024.0 / 1024.0 * sizeof(Session::Data) * 8);
    }
    return 0;
}

