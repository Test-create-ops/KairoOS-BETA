#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>

typedef struct task {
    uint64_t rsp;
    int active;
} task_t;

void scheduler_init(void);
void scheduler_add_task(uint64_t entry, uint64_t stack_top);
void scheduler_tick(void);

#endif
