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

class SocketPair {
public:
    inline SocketPair() noexcept: _sv{ -1, -1 } {
        socketpair(AF_LOCAL, SOCK_STREAM, 0, _sv);
    }
    inline ~SocketPair() noexcept {
        close(_sv[0]);
        close(_sv[1]);
    }
    inline SocketPair(SocketPair&& rhs) noexcept {
        _sv[0] = rhs._sv[0];
        rhs._sv[0] = -1;
        _sv[1] = rhs._sv[1];
        rhs._sv[1] = -1;
    }
    inline SocketPair& operator=(SocketPair&& rhs) noexcept {
        _sv[0] = rhs._sv[0];
        rhs._sv[0] = -1;
        _sv[1] = rhs._sv[1];
        rhs._sv[1] = -1;
        return *this;
    }
public:
    inline explicit operator bool() noexcept { return _sv[0] != -1; }
    inline int server() noexcept { return _sv[0]; }
    inline int client() noexcept { return _sv[1]; }
private:
    int _sv[2];
};

class Session {
public:
    inline Session(::iomp::IOMultiPlexer& iomp, std::atomic<uint64_t>& count) noexcept:
            _client(&Session::do_client, _sv.client(), std::ref(count)) {
        auto buf = new uint64_t(0);
        iomp.read(_sv.server(), buf, sizeof(*buf), [&iomp](::iomp::AsyncIO& aio, bool succ) {
            Session::do_server(iomp, aio, succ);
        });
    }
    inline ~Session() noexcept {
        _client.join();
    }
public:
    inline void stop() noexcept {
        shutdown(_sv.server(), SHUT_RDWR);
    }
    static void do_server(::iomp::IOMultiPlexer& iomp, ::iomp::AsyncIO& aio, bool succ) noexcept {
        if (succ) {
            //fprintf(stderr, "got 0x%lx\n", *reinterpret_cast<uint64_t*>(aio.buf));
            iomp.read(aio);
        } else {
            if (aio.error != 0) {
                fprintf(stderr, "read fail: %s\n", strerror(aio.error));
            }
            delete reinterpret_cast<uint64_t*>(aio.buf);
        }
    }
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
private:
    SocketPair _sv;
    std::thread _client;
};

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, [](int sig) noexcept {
        g_loop = false;
    });
    ::iomp::IOMultiPlexer iomp { 1 };
    std::atomic<uint64_t> count { 0 };
    std::vector<std::unique_ptr<Session>> sess;
    for (int i = 0; i < 2; i++) {
        sess.emplace_back(std::unique_ptr<Session>(new Session(iomp, count)));
    }
    while (g_loop) {
        sleep(1);
        auto qps = count.exchange(0);
        fprintf(stderr, "%zu qps(s)\n", (size_t)qps);
    }
    for (auto& s: sess) {
        s->stop();
    }
    return 0;
}

