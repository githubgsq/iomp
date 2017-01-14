#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>
#include "iomp.h"

static bool g_loop = true;

static void do_server(int sock, pid_t pid) noexcept;
static void do_client(int sock) noexcept;
static void do_stop(int sig) noexcept {
    g_loop = false;
}

int main(int argc, char* argv[]) {
    int sv[2] = { 0, 0 };
    socketpair(AF_LOCAL, SOCK_STREAM, 0, sv);
    fcntl(sv[0], F_SETNOSIGPIPE, 1);
    fcntl(sv[1], F_SETNOSIGPIPE, 1);
    auto pid = fork();
    if (pid == -1) {
        fprintf(stderr, "fork fail: %s", strerror(errno));
        return -1;
    } else if (pid == 0) {
        do_client(sv[1]);
    } else {
        do_server(sv[0], pid);
    }
    return 0;
}

void do_server(int sock, pid_t client) noexcept {
    signal(SIGINT, SIG_IGN);
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
    ::iomp::IOMultiPlexer iomp { 4 };
    uint64_t buf = 0;
    iomp.read({
        sock, &buf, sizeof(buf), -1,
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
    waitpid(client, nullptr, 0);
}

void do_client(int sock) noexcept {
    signal(SIGINT, do_stop);
    std::atomic<uint64_t> count { 0 };
    auto& loop = g_loop;
    std::thread t { [&count, &loop](int sock) {
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
        loop = false;
    }, sock };
    while (g_loop) {
        sleep(1);
        auto qps = count.exchange(0);
        fprintf(stderr, "%llu qps(s)\n", qps);
    }
    close(sock);
    t.join();
}

