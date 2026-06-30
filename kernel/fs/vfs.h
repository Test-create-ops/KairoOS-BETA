typedef struct vfs_node vfs_node_t;

typedef struct vfs_node_ops {
    long (*read)(vfs_node_t *node, void *buf, long size, long offset);
} vfs_node_ops_t;

struct vfs_node {
    char *name;
    unsigned int flags;
    vfs_node_ops_t *ops;
    void *fs_data;
    vfs_node_t *parent;
    vfs_node_t *children;
    vfs_node_t *next_sibling;
};

vfs_node_t *vfs_open(const char *path);
long vfs_read(vfs_node_t *node, void *buf, long size, long offset);
long vfs_size(vfs_node_t *node);
