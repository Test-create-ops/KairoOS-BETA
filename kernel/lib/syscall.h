#ifndef SYSCALL_H
#define SYSCALL_H

static inline long write(const char *s)
{
    long ret;
    __asm__ volatile("mov $0, %%rax; mov %1, %%rdi; int $0x80; mov %%rax, %0"
                     : "=r"(ret) : "r"((long)s) : "rax", "rdi");
    return ret;
}

static inline long read(char *buf, long size)
{
    long ret;
    __asm__ volatile("mov $1, %%rax; mov %1, %%rdi; mov %2, %%rsi; int $0x80; mov %%rax, %0"
                     : "=r"(ret) : "r"((long)buf), "r"(size) : "rax", "rdi", "rsi");
    return ret;
}

static inline long exec(const char *path)
{
    long ret;
    __asm__ volatile("mov $2, %%rax; mov %1, %%rdi; int $0x80; mov %%rax, %0"
                     : "=r"(ret) : "r"((long)path) : "rax", "rdi");
    return ret;
}

#endif
