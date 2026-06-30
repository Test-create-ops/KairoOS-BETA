#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

typedef uint64_t (*syscall_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

void syscall_init(void);
uint64_t syscall_dispatch(uint64_t num,
                          uint64_t a, uint64_t b, uint64_t c,
                          uint64_t d, uint64_t e);

#endif
