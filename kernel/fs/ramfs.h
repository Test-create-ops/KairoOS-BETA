#ifndef RAMFS_H
#define RAMFS_H

#include <stdint.h>

void ramfs_init(void);
void ramfs_create_root(void);
void ramfs_add(const char *name, const unsigned char *data, unsigned long size);
const char *ramfs_read_file(const char *name);
int ramfs_get_count(void);
const char *ramfs_get_name(int i);
unsigned long ramfs_get_size(int i);
const unsigned char *ramfs_get_data(int i);
int ramfs_create_file(const char *name);

#endif
