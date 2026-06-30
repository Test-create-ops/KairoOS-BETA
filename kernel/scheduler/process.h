#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>

typedef struct process {
    uint32_t pid;
    uint64_t entry;
    uint64_t stack_top;
    int active;
} process_t;

void process_init(void);
process_t *process_create(uint64_t entry);

#endif
