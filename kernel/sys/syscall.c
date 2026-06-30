#include "syscall.h"
#include "../ui/console.h"
#include "../fs/ramfs.h"

static uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len,
                          uint64_t d, uint64_t e, uint64_t f)
{
    (void)fd; (void)d; (void)e; (void)f;
    const char *s = (const char *)buf;
    for (uint64_t i = 0; i < len; i++)
        console_putc(s[i]);
    return len;
}

static uint64_t sys_exit(uint64_t code, uint64_t b, uint64_t c,
                         uint64_t d, uint64_t e, uint64_t f)
{
    (void)b; (void)c; (void)d; (void)e; (void)f;
    console_write("\n[process exited]\n");
    while (1) { __asm__ volatile("hlt"); }
    return code;
}

static uint64_t sys_getpid(uint64_t a, uint64_t b, uint64_t c,
                           uint64_t d, uint64_t e, uint64_t f)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    return 1;
}

static uint64_t sys_open(uint64_t name, uint64_t b, uint64_t c,
                         uint64_t d, uint64_t e, uint64_t f)
{
    (void)b; (void)c; (void)d; (void)e; (void)f;
    const char *n = (const char *)name;
    const char *data = ramfs_read_file(n);
    if (!data) return (uint64_t)-1;
    return (uint64_t)data;
}

static uint64_t sys_read(uint64_t fd, uint64_t buf, uint64_t len,
                         uint64_t d, uint64_t e, uint64_t f)
{
    (void)d; (void)e; (void)f;
    const char *src = (const char *)fd;
    char *dst = (char *)buf;
    for (uint64_t i = 0; i < len; i++)
        dst[i] = src[i];
    return len;
}

static uint64_t sys_close(uint64_t fd, uint64_t b, uint64_t c,
                          uint64_t d, uint64_t e, uint64_t f)
{
    (void)fd; (void)b; (void)c; (void)d; (void)e; (void)f;
    return 0;
}

#define MAX_SYSCALLS 64
static syscall_t syscalls[MAX_SYSCALLS];

void syscall_init(void)
{
    for (int i = 0; i < MAX_SYSCALLS; i++)
        syscalls[i] = 0;

    syscalls[0] = sys_write;
    syscalls[1] = sys_exit;
    syscalls[2] = sys_getpid;
    syscalls[3] = sys_open;
    syscalls[4] = sys_read;
    syscalls[5] = sys_close;
}

uint64_t syscall_dispatch(uint64_t num,
                          uint64_t a, uint64_t b, uint64_t c,
                          uint64_t d, uint64_t e)
{
    if (num >= MAX_SYSCALLS || !syscalls[num])
        return (uint64_t)-1;

    return syscalls[num](a, b, c, d, e, 0);
}
