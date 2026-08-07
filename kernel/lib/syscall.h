#ifndef SYSCALL_H
#define SYSCALL_H

static inline long ksys(long n, long a1, long a2, long a3)
{
    long ret;
    __asm__ volatile("mov %1, %%rax; mov %2, %%rbx; mov %3, %%rcx; mov %4, %%rdx; int $0x80; mov %%rax, %0"
                     : "=r"(ret) : "r"(n), "r"(a1), "r"(a2), "r"(a3) : "rax", "rbx", "rcx", "rdx", "memory");
    return ret;
}

static inline long write(const char *s) { return ksys(0, (long)s, 0, 0); }
static inline long read(char *buf, long size) { return ksys(1, (long)buf, size, 0); }
static inline long exec(const char *path) { return ksys(2, (long)path, 0, 0); }
static inline long exit_(int code) { return ksys(3, (long)code, 0, 0); }

#endif
