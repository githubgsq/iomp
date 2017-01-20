#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <atomic>
#include <vector>
#include <functional>
#include <fcntl.h>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>
#include "iomp.h"

static bool g_loop = true;

class Session {
public:
#if 0
    class Data {
    public:
        inline explicit Data(unsigned int val) noexcept: _val(val) {
            _data[0] = 0;
        }
        inline operator unsigned int() noexcept { return _val; }
        inline int operator++() noexcept { return _val++; }
        inline int operator++(int) noexcept { return ++_val; }
    private:
        unsigned int _val;
        uint64_t _data[100];
    };
#else
    typedef unsigned int Data;
#endif
    inline Session(::iomp::IOMultiPlexer& iomp, std::atomic<uint64_t>& count) noexcept/*:
            _client(&Session::do_client, _sv.client(), std::ref(count))*/ {
        int sv[2] = { -1, -1 };
        if (socketpair(AF_LOCAL, SOCK_STREAM, 0, sv) != 0) {
            fprintf(stderr, "socketpair fail: %s\n", strerror(errno));
            return;
        }
        fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL, 0) | O_NONBLOCK);
        fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL, 0) | O_NONBLOCK);
        auto buf = new Data(0);
        iomp.read(sv[0], buf, sizeof(*buf), [&iomp](::iomp::AsyncIO& aio, int error) {
            Session::do_server(iomp, aio, error);
        });
        auto buf2 = new Data(0xdeadc00d);
        iomp.write(sv[1], buf2, sizeof(*buf2), [&iomp, &count](::iomp::AsyncIO& aio, int error) {
            Session::do_client(iomp, aio, error, count);
        });
    }
    inline ~Session() noexcept {
        //_client.join();
    }
public:
    static void do_server(::iomp::IOMultiPlexer& iomp, ::iomp::AsyncIO& aio, int error) noexcept {
        if (error == 0) {
            //fprintf(stderr, "recv 0x%x\n", *reinterpret_cast<unsigned int*>(aio.buf));
            //struct timespec ts = { 0, 1000000 };
            //nanosleep(&ts, nullptr);
            iomp.read(aio);
        } else {
            if (error > 0) {
                fprintf(stderr, "read fail: %s\n", strerror(error));
            } else {
                fprintf(stderr, "read end\n");
            }
            delete reinterpret_cast<Data*>(aio.buf);
            close(aio);
            //shutdown(aio, SHUT_RDWR);
        }
    }
    static void do_client(::iomp::IOMultiPlexer& iomp, ::iomp::AsyncIO& aio, int error,
            std::atomic<uint64_t>& count) noexcept {
        if (error == 0) {
            auto& data = *reinterpret_cast<Data*>(aio.buf);
            //fprintf(stderr, "send 0x%x\n", data);
            data++;
            count++;
            //struct timespec ts = { 0, 1000000 };
            //nanosleep(&ts, nullptr);
            iomp.write(aio);
        } else {
            if (error > 0) {
                fprintf(stderr, "write fail: %s\n", strerror(error));
            } else {
                fprintf(stderr, "write end\n");
            }
            delete reinterpret_cast<Data*>(aio.buf);
            close(aio);
            //shutdown(aio, SHUT_RDWR);
        }
    }
#if 0
    static void do_client(int sock, std::atomic<uint64_t>& count) {
        uint64_t data = 0xdeadbeeffacebabe;
        while (1) {
            auto len = write(sock, &data, sizeof(data));
            if (len != sizeof(data)) {
                fprintf(stderr, "write fail: %s\n", strerror(errno));
                break;
            }
            data++;
            count++;
        }
    }
#endif
private:
    //SocketPair _sv;
    //std::thread _client;
};

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, [](int sig) noexcept {
        g_loop = false;
    });
    std::atomic<uint64_t> count { 0 };
    std::vector<std::unique_ptr<Session>> sess;
    ::iomp::IOMultiPlexer iomp { 1 };
    for (int i = 0; i < 4; i++) {
        sess.emplace_back(std::unique_ptr<Session>(new Session(iomp, count)));
    }
    //while (g_loop) {
    for (int i = 0; i < 5; i++) {
        sleep(1);
        auto qps = count.exchange(0);
        fprintf(stderr, "%zu qps(s)\n", (size_t)qps);
    }
    return 0;
}

