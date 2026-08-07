#ifndef KFS_H
#define KFS_H

#include <stdint.h>

#define KFS_MAGIC 0x53464B // "KFS\0"
#define KFS_SECTOR_SIZE 512
#define KFS_MAX_FILES 64
#define KFS_MAX_NAME 56

typedef struct {
    char     name[KFS_MAX_NAME];
    uint32_t start_sector;
    uint32_t size;
} __attribute__((packed)) kfs_entry_t;

typedef struct {
    uint32_t magic;
    uint32_t total_sectors;
    uint32_t file_count;
    uint32_t data_start;
} __attribute__((packed)) kfs_superblock_t;

int kfs_mount(int port);
int kfs_format(int port);
int kfs_read(const char *name, void *buf, uint32_t max_size);
int kfs_write(const char *name, const void *buf, uint32_t size);
int kfs_list(void);

#endif
