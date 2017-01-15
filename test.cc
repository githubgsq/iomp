#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <atomic>
#include <vector>
#include <fcntl.h>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>
#include "iomp.h"

static bool g_loop = true;

static void do_client(int sock, std::atomic<uint64_t>& count) noexcept;

class ScopedFd {
public:
    inline ScopedFd() noexcept: _fd(-1) { }
    inline explicit ScopedFd(int fd) noexcept: _fd(fd) { }
    inline ~ScopedFd() noexcept {
        this->close();
    }
    inline ScopedFd(ScopedFd&& rhs) noexcept: _fd(rhs._fd) {
        rhs._fd = -1;
    }
    inline ScopedFd& operator=(ScopedFd&& rhs) noexcept {
        _fd = rhs._fd;
        rhs._fd = -1;
        return *this;
    }
public:
    inline explicit operator bool() noexcept { return _fd != -1; }
    inline operator int() noexcept { return _fd; }
    inline void close() noexcept {
        if (_fd != -1) {
            fprintf(stderr, "close %d\n", _fd);
            ::close(_fd);
        }
        _fd = -1;
    }
private:
    int _fd;
};

int main(int argc, char* argv[]) {
    signal(SIGINT, [](auto sig) noexcept {
        g_loop = false;
    });
    std::atomic<uint64_t> count { 0 };
    ScopedFd sv[2];
    if (socketpair(AF_LOCAL, SOCK_STREAM, 0, reinterpret_cast<int*>(sv)) != 0) {
        fprintf(stderr, "socketpair fail: %s", strerror(errno));
        return -1;
    }
    if (fcntl(sv[1], F_SETNOSIGPIPE, 1) == -1) {
        fprintf(stderr, "fcntl fail: %s", strerror(errno));
        return -1;
    }

    std::vector<std::thread> cs;
    cs.emplace_back(do_client, (int)sv[1], std::ref(count));
    cs.emplace_back(do_client, (int)sv[1], std::ref(count));
    cs.emplace_back(do_client, (int)sv[1], std::ref(count));
    cs.emplace_back(do_client, (int)sv[1], std::ref(count));

    ::iomp::IOMultiPlexer iomp;
    uint64_t buf = 0;
    iomp.read({
        sv[0], &buf, sizeof(buf),
        [&iomp](auto aio, auto succ) noexcept {
            if (succ) {
                iomp.read(aio);
            } else {
                fprintf(stderr, "read fail: %s\n",
                        aio->error == 0 ? "eof" : strerror(aio->error));
                shutdown(aio->fildes, SHUT_RDWR);
            }
        }
    });

    while (g_loop) {
        sleep(1);
        auto qps = count.exchange(0);
        fprintf(stderr, "%llu qps(s)\n", qps);
    }
    sv[1].close();
    for (auto& c: cs) {
        c.join();
    }
}

void do_client(int sock, std::atomic<uint64_t>& count) noexcept {
    uint64_t data = 0xdeadc00dfacebabe;
    while (1) {
        auto len = write(sock, &data, sizeof(data));
        if (len != sizeof(data)) {
            fprintf(stderr, "write fail: %s\n", strerror(errno));
            break;
        }
        data++;
        count++;
        struct timespec to { 0, 50000 };
        nanosleep(&to, nullptr);
    }
}

