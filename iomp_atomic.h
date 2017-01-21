#ifndef IOMP_ATOMIC_H
#define IOMP_ATOMIC_H

#include <stdint.h>

inline __attribute__((always_inline)) void iomp_addref(volatile uint64_t* n) {
    __asm__ __volatile__ ("lock; incl %0;":"+m"(*n)::"cc");
}

inline __attribute__((always_inline)) uint64_t iomp_release(volatile uint64_t* n) {
    uint64_t r = -1;
    __asm__ __volatile__ ("lock; xaddq %1, %0;":"+m"(*n), "+r"(r)::"cc");
    return r - 1;
}

#endif /* IOMP_ATOMIC_H */

