#ifndef RAMFS_H
#define RAMFS_H

#include <stdint.h>

void ramfs_init(void);
void ramfs_add(const char *name, const unsigned char *data, unsigned long size);
const char *ramfs_read_file(const char *name);

#endif
