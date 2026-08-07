#include "../fs/vfs.h"
#include "../fs/ramfs.h"
#include "../lib/memory.h"

extern unsigned char _binary_initrd_tar_start[];
extern unsigned char _binary_initrd_tar_end[];

static unsigned long strtoul(const char *p, char **end, int base)
{
    unsigned long r = 0;
    while (*p >= '0' && *p <= '7') {
        r = r * base + (*p - '0');
        p++;
    }
    if (end) *end = (char *)p;
    return r;
}

void initrd_load(void)
{
    ramfs_create_root();
    unsigned char *p = _binary_initrd_tar_start;
    while (p < _binary_initrd_tar_end) {
        char name[101];
        memcpy(name, p, 100);
        name[100] = 0;
        if (name[0] == 0) break;

        char *base = name;
        while (*base == '.') base++;
        if (*base == '/') base++;
        for (char *q = base; *q; q++)
            if (*q == '/') base = q + 1;

        unsigned long size = strtoul((char *)(p + 124), 0, 8);
        unsigned char *file_data = p + 512;

        if (base[0]) {
            int blen = 0;
            while (base[blen]) blen++;
            char *copy = kmalloc(blen + 1);
            int bi;
            for (bi = 0; bi < blen; bi++) copy[bi] = base[bi];
            copy[blen] = 0;
            ramfs_add(copy, file_data, size);
        }

        unsigned long blocks = (size + 511) / 512;
        p += (blocks + 1) * 512;
    }
}
