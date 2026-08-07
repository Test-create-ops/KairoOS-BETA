#include "vfs.h"
#include "../lib/string.h"
#include "../lib/memory.h"
#include "../memory/heap.h"

#define MAX_RAMFS_FILES 64

typedef struct {
    char *name;
    unsigned char *data;
    unsigned long size;
    unsigned long capacity;
    int writable;
} ramfs_file_t;

static ramfs_file_t ramfs_files[MAX_RAMFS_FILES];
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
    if (to_read > 0)
        memcpy(buf, f->data + offset, to_read);
    return to_read;
}

static long ramfs_size(vfs_node_t *node)
{
    ramfs_file_t *f = (ramfs_file_t *)node->fs_data;
    return (long)f->size;
}

static long ramfs_write(vfs_node_t *node, const void *buf, long size, long offset)
{
    ramfs_file_t *f = (ramfs_file_t *)node->fs_data;
    if (!f->writable) return -1;
    unsigned long needed = offset + size;
    if (needed > f->capacity) {
        unsigned long new_cap = f->capacity;
        while (new_cap < needed) new_cap = new_cap ? new_cap * 2 : 4096;
        unsigned char *new_data = kmalloc(new_cap);
        if (!new_data) return -1;
        if (f->data && f->size > 0)
            memcpy(new_data, f->data, f->size);
        kfree(f->data);
        f->data = new_data;
        f->capacity = new_cap;
    }
    memcpy(f->data + offset, buf, size);
    if (needed > f->size) f->size = needed;
    return size;
}

void ramfs_create_root(void)
{
    if (root_fs) return;
    vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
    node->name = (char *)"/";
    node->ops = 0;
    node->fs_data = 0;
    node->parent = 0;
    node->children = 0;
    node->next_sibling = 0;
    root_fs = node;
}

void ramfs_add(const char *name, const unsigned char *data, unsigned long size)
{
    if (ramfs_count >= MAX_RAMFS_FILES) return;
    ramfs_files[ramfs_count].name = (char *)name;
    ramfs_files[ramfs_count].data = (unsigned char *)data;
    ramfs_files[ramfs_count].size = size;
    ramfs_files[ramfs_count].capacity = size;
    ramfs_files[ramfs_count].writable = 0;

    vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
    node->name = (char *)name;
    node->ops = kmalloc(sizeof(vfs_node_ops_t));
    node->ops->read = ramfs_read;
    node->ops->write = ramfs_write;
    node->ops->size = ramfs_size;
    node->fs_data = &ramfs_files[ramfs_count];
    node->parent = 0;
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

int ramfs_create_file(const char *name)
{
    if (!name || !name[0]) return -1;
    if (ramfs_count >= MAX_RAMFS_FILES) return -1;
    int len = 0; while (name[len]) len++;
    char *cpy = kmalloc(len + 1);
    int i; for (i = 0; i < len; i++) cpy[i] = name[i];
    cpy[len] = 0;
    ramfs_files[ramfs_count].name = cpy;
    ramfs_files[ramfs_count].data = 0;
    ramfs_files[ramfs_count].size = 0;
    ramfs_files[ramfs_count].capacity = 0;
    ramfs_files[ramfs_count].writable = 1;

    vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
    node->name = cpy;
    node->ops = kmalloc(sizeof(vfs_node_ops_t));
    node->ops->read = ramfs_read;
    node->ops->write = ramfs_write;
    node->ops->size = ramfs_size;
    node->fs_data = &ramfs_files[ramfs_count];
    node->parent = 0;
    node->children = 0;
    node->next_sibling = 0;

    if (!root_fs) {
        root_fs = node;
    } else {
        node->next_sibling = root_fs->children;
        root_fs->children = node;
    }
    ramfs_count++;
    return 0;
}
