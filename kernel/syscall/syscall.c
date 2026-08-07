#include "../lib/framebuffer.h"
#include "../lib/string.h"
#include "../fs/vfs.h"
#include "../proc/proc.h"

extern char keyboard_last_char(void);

long sys_write(const char *s)
{
    fb_write(s);
    return 0;
}

long sys_read(char *buf, long size)
{
    long n = 0;
    while (n < size) {
        char c = keyboard_last_char();
        if (!c) break;
        buf[n++] = c;
        if (c == '\n') break;
    }
    return n;
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

long sys_fs_read(const char *path, void *buf, long max)
{
    extern int kfs_read(const char *name, void *buf, unsigned int max_size);
    const char *name = path;
    while (*name == '/') name++;
    return kfs_read(name, buf, max);
}

long sys_fs_write(const char *path, const void *buf, long size)
{
    extern int kfs_write(const char *name, const void *buf, unsigned int size);
    const char *name = path;
    while (*name == '/') name++;
    return kfs_write(name, buf, size);
}

long sys_fs_list(void)
{
    extern int kfs_list(void);
    return kfs_list();
}

long syscall_dispatch(long n, long a1, long a2, long a3)
{
    switch (n) {
        case 0: return sys_write((const char*)a1);
        case 1: return sys_read((char*)a1, a2);
        case 2: return sys_exec((const char*)a1);
        case 3: return sys_exit((int)a1);
        case 4: return sys_fs_read((const char*)a1, (void*)a2, a3);
        case 5: return sys_fs_write((const char*)a1, (const void*)a2, a3);
        case 6: return sys_fs_list();
    }
    return -1;
}
