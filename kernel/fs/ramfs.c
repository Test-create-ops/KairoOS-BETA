#include "vfs.h"
#include "../lib/string.h"
#include "../lib/memory.h"

typedef struct {
    const char *name;
    const unsigned char *data;
    unsigned long size;
} ramfs_file_t;

static ramfs_file_t ramfs_files[64];
static int ramfs_count = 0;

int ramfs_get_count(void) { return ramfs_count; }
const char *ramfs_get_name(int i) { return (i >= 0 && i < ramfs_count) ? ramfs_files[i].name : 0; }
unsigned long ramfs_get_size(int i) { return (i >= 0 && i < ramfs_count) ? ramfs_files[i].size : 0; }
const unsigned char *ramfs_get_data(int i) { return (i >= 0 && i < ramfs_count) ? ramfs_files[i].data : 0; }

const char *ramfs_read_file(const char *name)
{
    for (int i = 0; i < ramfs_count; i++) {
        int eq = 1;
        const char *a = ramfs_files[i].name;
        const char *b = name;
        while (*a || *b) {
            if (*a != *b) { eq = 0; break; }
            a++; b++;
        }
        if (eq)
            return (const char *)ramfs_files[i].data;
    }
    return 0;
}

static long ramfs_read(vfs_node_t *node, void *buf, long size, long offset)
{
    ramfs_file_t *f = (ramfs_file_t *)node->fs_data;
    if (offset >= (long)f->size) return 0;
    long to_read = (offset + size > (long)f->size) ? ((long)f->size - offset) : size;
    memcpy(buf, f->data + offset, to_read);
    return to_read;
}

void ramfs_add(const char *name, const unsigned char *data, unsigned long size)
{
    ramfs_files[ramfs_count].name = name;
    ramfs_files[ramfs_count].data = data;
    ramfs_files[ramfs_count].size = size;

    vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
    node->name = (char *)name;
    node->ops = kmalloc(sizeof(vfs_node_ops_t));
    node->ops->read = ramfs_read;
    node->fs_data = &ramfs_files[ramfs_count];
    node->children = 0;
    node->next_sibling = 0;

    if (!root_fs) {
        root_fs = node;
    } else {
        node->next_sibling = root_fs->children;
        root_fs->children = node;
    }

    ramfs_count++;
}
