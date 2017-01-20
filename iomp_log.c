#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>
#include "iomp.h"

const char* iomp_now(char* buf, size_t bufsz) {
    struct timeval tv = { 0, 0 };
    gettimeofday(&tv, NULL);
    struct tm ltm;
    size_t len = strftime(buf, bufsz, "%Y-%m-%d %H:%M:%S",
            localtime_r(&tv.tv_sec, &ltm));
    if (len <= bufsz) {
        snprintf(buf + len, bufsz - len, ".%06d", (int32_t)tv.tv_usec);
    }
    return buf;
}

static int g_iomp_loglevel = IOMP_LOGLEVEL_WARNING;

int iomp_writelog(int level, const char* fmt, ...) {
    if (level < g_iomp_loglevel) {
        return 0;
    }
    va_list vg;
    va_start(vg, fmt);
    int rv = vfprintf(stderr, fmt, vg);
    va_end(vg);
    return rv;
}

int iomp_loglevel(int level) {
    int old = g_iomp_loglevel;
    if (level >= IOMP_LOGLEVEL_DEBUG && level <= IOMP_LOGLEVEL_FATAL) {
        g_iomp_loglevel = level;
    }
    return old;
}

