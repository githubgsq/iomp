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
    inline Session(::iomp::IOMultiPlexer& iomp, std::atomic<uint64_t>& count) noexcept/*:
            _client(&Session::do_client, _sv.client(), std::ref(count))*/ {
        int sv[2] = { -1, -1 };
        if (socketpair(AF_LOCAL, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) != 0) {
            fprintf(stderr, "socketpair fail: %s\n", strerror(errno));
            return;
        }
        fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL, 0) | O_NONBLOCK);
        fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL, 0) | O_NONBLOCK);
        /*auto buf = new uint64_t(0);
        iomp.read(sv[0], buf, sizeof(*buf), [&iomp](::iomp::AsyncIO& aio, int error) {
            Session::do_server(iomp, aio, error);
        });*/
        auto buf2 = new uint64_t(0xdeadbeedfacebabe);
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
            fprintf(stderr, "recv 0x%lx\n", *reinterpret_cast<uint64_t*>(aio.buf));
            iomp.read(aio);
        } else {
            if (error > 0) {
                fprintf(stderr, "read fail: %s\n", strerror(error));
            } else {
                fprintf(stderr, "read end\n");
            }
            delete reinterpret_cast<uint64_t*>(aio.buf);
            close(aio);
            //shutdown(aio, SHUT_RDWR);
        }
    }
    static void do_client(::iomp::IOMultiPlexer& iomp, ::iomp::AsyncIO& aio, int error,
            std::atomic<uint64_t>& count) noexcept {
        if (error == 0) {
            auto& data = *reinterpret_cast<uint64_t*>(aio.buf);
            fprintf(stderr, "send 0x%lx\n", data);
            data++;
            count++;
            iomp.write(aio);
        } else {
            if (error > 0) {
                fprintf(stderr, "write fail: %s\n", strerror(error));
            } else {
                fprintf(stderr, "write end\n");
            }
            delete reinterpret_cast<uint64_t*>(aio.buf);
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
    for (int i = 0; i < 1; i++) {
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

