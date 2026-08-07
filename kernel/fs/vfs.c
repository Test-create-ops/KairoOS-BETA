#include "vfs.h"
#include "../utils/string.h"
#include "../memory/heap.h"

vfs_node_t *root_fs = 0;

vfs_node_t *vfs_open(const char *path) {
    if (!root_fs) return 0;
    vfs_node_t *cur = root_fs;
    if (kstrcmp(path, "/") == 0) return cur;
    if (path[0] == '/') path++;
    char temp[128];
    int ti = 0;
    while (*path) {
        if (*path == '/') {
            temp[ti] = 0; ti = 0;
            vfs_node_t *child = cur->children;
            while (child) {
                if (kstrcmp(child->name, temp) == 0) { cur = child; break; }
                child = child->next_sibling;
            }
            if (!child) return 0;
        } else {
            temp[ti++] = *path;
        }
        path++;
    }
    temp[ti] = 0;
    vfs_node_t *child = cur->children;
    while (child) {
        if (kstrcmp(child->name, temp) == 0) return child;
        child = child->next_sibling;
    }
    return 0;
}

long vfs_read(vfs_node_t *node, void *buf, long size, long offset) {
    if (!node || !node->ops || !node->ops->read) return -1;
    return node->ops->read(node, buf, size, offset);
}

long vfs_write(vfs_node_t *node, const void *buf, long size, long offset) {
    if (!node || !node->ops || !node->ops->write) return -1;
    return node->ops->write(node, buf, size, offset);
}

long vfs_size(vfs_node_t *node) {
    if (!node || !node->ops || !node->ops->size) return 0;
    return node->ops->size(node);
}

vfs_node_t *vfs_create(const char *path, vfs_node_ops_t *ops, void *fs_data) {
    if (!root_fs) return 0;
    const char *p = path;
    if (p[0] == '/') p++;
    char parent_path[128];
    char name[128];
    int li = 0, last_slash = -1;
    for (int i = 0; p[i]; i++) {
        if (p[i] == '/') last_slash = i;
    }
    if (last_slash < 0) {
        int j; for (j = 0; p[j]; j++) name[j] = p[j];
        name[j] = 0;
        vfs_node_t *n = kmalloc(sizeof(vfs_node_t));
        n->name = kmalloc(kstrlen(name) + 1);
        int j2; for (j2 = 0; name[j2]; j2++) n->name[j2] = name[j2];
        n->name[j2] = 0;
        n->ops = ops;
        n->fs_data = fs_data;
        n->parent = root_fs;
        n->children = 0;
        n->next_sibling = root_fs->children;
        root_fs->children = n;
        return n;
    }
    int j; for (j = 0; j < last_slash; j++) parent_path[j] = p[j];
    parent_path[j] = 0;
    int k; for (k = last_slash + 1; p[k]; k++) name[k - last_slash - 1] = p[k];
    name[k - last_slash - 1] = 0;
    vfs_node_t *parent = vfs_open(parent_path);
    if (!parent) return 0;
    vfs_node_t *n = kmalloc(sizeof(vfs_node_t));
    n->name = kmalloc(kstrlen(name) + 1);
    int j3; for (j3 = 0; name[j3]; j3++) n->name[j3] = name[j3];
    n->name[j3] = 0;
    n->ops = ops;
    n->fs_data = fs_data;
    n->parent = parent;
    n->children = 0;
    n->next_sibling = parent->children;
    parent->children = n;
    return n;
}
