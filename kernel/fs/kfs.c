#include "kfs.h"
#include "../lib/memory.h"
#include "../utils/string.h"
#include "../lib/framebuffer.h"
#include "../memory/heap.h"

extern int ahci_read_sectors(int port, uint64_t lba, uint16_t count, void *buf);
extern int ahci_write_sectors(int port, uint64_t lba, uint16_t count, const void *buf);
extern int ahci_disk_port;

static int kfs_port = -1;
static kfs_superblock_t sb;
static int mounted = 0;

static void kfs_puts(const char *s) {
    fb_write(s);
}

int kfs_format(int port)
{
    uint8_t sector[KFS_SECTOR_SIZE];
    memset(sector, 0, KFS_SECTOR_SIZE);
    kfs_superblock_t *sb = (kfs_superblock_t *)sector;
    sb->magic = KFS_MAGIC;
    sb->total_sectors = 65536;
    sb->file_count = 0;
    sb->data_start = 2;
    int ret = ahci_write_sectors(port, 0, 1, sector);
    if (ret) return -1;
    memset(sector, 0, KFS_SECTOR_SIZE);
    ret = ahci_write_sectors(port, 1, 1, sector);
    if (ret) return -1;
    kfs_puts("KFS formattato\n");
    return 0;
}

int kfs_mount(int port)
{
    int ret = ahci_read_sectors(port, 0, 1, &sb);
    if (ret) return -1;
    if (sb.magic != KFS_MAGIC) return -2;
    kfs_port = port;
    mounted = 1;
    char buf[64];
    kfs_puts("KFS montato: ");
    buf[0] = '0' + (sb.file_count / 10);
    buf[1] = '0' + (sb.file_count % 10);
    buf[2] = ' ';
    buf[3] = 'f';
    buf[4] = 'i';
    buf[5] = 'l';
    buf[6] = 'e';
    buf[7] = 's';
    buf[8] = 0;
    kfs_puts(buf);
    kfs_puts("\n");
    return 0;
}

int kfs_read(const char *name, void *buf, uint32_t max_size)
{
    if (!mounted) return -1;
    kfs_entry_t entries[KFS_MAX_FILES];
    int ret = ahci_read_sectors(kfs_port, 1, 1, entries);
    if (ret) return -1;
    for (uint32_t i = 0; i < sb.file_count && i < KFS_MAX_FILES; i++) {
        if (kstrcmp(entries[i].name, name) == 0) {
            uint32_t to_read = entries[i].size;
            if (to_read > max_size) to_read = max_size;
            uint32_t sectors = (to_read + KFS_SECTOR_SIZE - 1) / KFS_SECTOR_SIZE;
            ret = ahci_read_sectors(kfs_port, entries[i].start_sector, sectors, buf);
            if (ret) return -1;
            return to_read;
        }
    }
    return -1;
}

int kfs_write(const char *name, const void *buf, uint32_t size)
{
    if (!mounted) return -1;
    kfs_entry_t entries[KFS_MAX_FILES];
    int ret = ahci_read_sectors(kfs_port, 1, 1, entries);
    if (ret) return -1;
    uint32_t slot = sb.file_count;
    for (uint32_t i = 0; i < sb.file_count && i < KFS_MAX_FILES; i++) {
        if (kstrcmp(entries[i].name, name) == 0) {
            slot = i;
            break;
        }
    }
    if (slot >= KFS_MAX_FILES) return -1;
    int len = 0;
    while (name[len] && len < KFS_MAX_NAME - 1) {
        entries[slot].name[len] = name[len];
        len++;
    }
    entries[slot].name[len] = 0;
    uint32_t sectors = (size + KFS_SECTOR_SIZE - 1) / KFS_SECTOR_SIZE;
    uint32_t start = sb.data_start;
    for (uint32_t i = 0; i < sb.file_count && i < KFS_MAX_FILES; i++) {
        if (entries[i].start_sector + (entries[i].size + KFS_SECTOR_SIZE - 1) / KFS_SECTOR_SIZE > start)
            start = entries[i].start_sector + (entries[i].size + KFS_SECTOR_SIZE - 1) / KFS_SECTOR_SIZE;
    }
    entries[slot].start_sector = start;
    entries[slot].size = size;
    ret = ahci_write_sectors(kfs_port, start, sectors, buf);
    if (ret) return -1;
    if (slot >= sb.file_count) sb.file_count = slot + 1;
    ret = ahci_write_sectors(kfs_port, 1, 1, entries);
    if (ret) return -1;
    ret = ahci_write_sectors(kfs_port, 0, 1, &sb);
    if (ret) return -1;
    return size;
}

int kfs_list(void)
{
    if (!mounted) return -1;
    kfs_entry_t entries[64];
    int ret = ahci_read_sectors(kfs_port, 1, 1, entries);
    if (ret) return -1;
    kfs_puts("Files:\n");
    for (uint32_t i = 0; i < sb.file_count && i < 64; i++) {
        kfs_puts("  ");
        kfs_puts(entries[i].name);
        kfs_puts(" (");
        char sz[16];
        uint32_t s = entries[i].size;
        int di = 0;
        if (s == 0) { sz[0] = '0'; di = 1; }
        while (s > 0) { sz[di++] = '0' + (s % 10); s /= 10; }
        for (int j = 0; j < di / 2; j++) { char t = sz[j]; sz[j] = sz[di-1-j]; sz[di-1-j] = t; }
        sz[di] = 0;
        kfs_puts(sz);
        kfs_puts(" bytes)\n");
    }
    return 0;
}
