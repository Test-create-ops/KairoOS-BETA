#include "vfs.h"
#include "../utils/string.h"

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
            temp[ti] = 0;
            ti = 0;

            vfs_node_t *child = cur->children;
            while (child) {
                if (kstrcmp(child->name, temp) == 0) {
                    cur = child;
                    break;
                }
                child = child->next_sibling;
            }
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

long vfs_size(vfs_node_t *node) {
    (void)node;
    return 0;
}
