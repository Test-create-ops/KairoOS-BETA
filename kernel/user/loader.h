#ifndef LOADER_H
#define LOADER_H

#include <stdint.h>

typedef void (*program_t)(void);

void loader_init(void);
int loader_register(const char *name, program_t prog);
int loader_run(const char *name);

#endif
