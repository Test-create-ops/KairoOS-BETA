#include "../lib/framebuffer.h"
#include "../lib/string.h"
#include "../drivers/input/keyboard.c"
#include "../fs/vfs.h"
#include "../proc/proc.h"

long sys_write(const char *s)
{
    fb_write(s);
    return 0;
}

long sys_read(char *buf, long size)
{
    for (long i = 0; i < size; i++) {
        buf[i] = keyboard_get();
        if (buf[i] == '\n') return i + 1;
    }
    return size;
}

long sys_exec(const char *path)
{
    extern int elf_load_and_exec(const char *path);
    return elf_load_and_exec(path);
}

long sys_exit(int code)
{
    (void)code;
    extern void proc_exit(int code);
    proc_exit(code);
    return 0;
}

long syscall_dispatch(long n, long a1, long a2, long a3)
{
    switch (n) {
        case 0: return sys_write((const char*)a1);
        case 1: return sys_read((char*)a1, a2);
        case 2: return sys_exec((const char*)a1);
        case 3: return sys_exit((int)a1);
    }
    return -1;
}
