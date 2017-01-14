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

static int g_loop = 1;

static void do_stop(int sig) noexcept {
    g_loop = 0;
}

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, do_stop);
    auto iomp = iomp_new(4);
    int sv[2] = { 0, 0 };
    socketpair(AF_LOCAL, SOCK_STREAM, 0, sv);
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(sv[0], F_SETNOSIGPIPE, 1);
    fcntl(sv[1], F_SETNOSIGPIPE, 1);
    uint64_t buf = 0;
    struct iomp_aio aio = {
        sv[0],
        &buf,
        sizeof(buf),
        0,
        [](iomp_aio_t aio, int succ) {
            if (succ) {
                auto val = *reinterpret_cast<uint64_t*>(aio->buf);
                //fprintf(stderr, "got %016llx\n", val);
                //close(aio->fildes);
                iomp_read(reinterpret_cast<iomp_t>(aio->udata), aio);
            } else {
                fprintf(stderr, "read fail: %s\n",
                        aio->error == 0 ? "eof" : strerror(aio->error));
                shutdown(aio->fildes, SHUT_RDWR);
            }
        },
        0,
        iomp,
    };
    iomp_read(iomp, &aio);
    std::atomic<uint64_t> count { 0 };
    std::thread t { [&count](int sock) {
        uint64_t data = 0xdeadc00dfacebabe;
        while (1) {
            //fprintf(stderr, "begin write\n");
            auto len = write(sock, &data, sizeof(data));
            if (len != sizeof(data)) {
                fprintf(stderr, "write fail: %s\n", strerror(errno));
                break;
            }
            //fprintf(stderr, "write done\n");
            data++;
            count++;
            //struct timespec to { 0, 50000 };
            //nanosleep(&to, nullptr);
        }
        shutdown(sock, SHUT_RDWR);
    }, sv[1] };
    while (g_loop) {
        sleep(1);
    }
    //close(sv[0]);
    close(sv[1]);
    t.join();
    iomp_drop(iomp);
    return 0;
}

