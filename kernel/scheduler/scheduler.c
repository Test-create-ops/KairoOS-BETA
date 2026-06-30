#include "scheduler.h"

extern void context_switch(uint64_t *old_rsp, uint64_t new_rsp);

#define MAX_TASKS 16

static task_t tasks[MAX_TASKS];
static int current_task = -1;

void scheduler_init(void)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        tasks[i].active = 0;
        tasks[i].rsp = 0;
    }
    current_task = -1;
}

void scheduler_add_task(uint64_t entry, uint64_t stack_top)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        if (!tasks[i].active) {
            tasks[i].active = 1;
            tasks[i].rsp = stack_top - 16 * sizeof(uint64_t);

            uint64_t *stack = (uint64_t *)tasks[i].rsp;
            stack[0] = entry;  // RIP (top of stack for 'ret')

            if (current_task == -1)
                current_task = i;
            return;
        }
    }
}

void scheduler_tick(void)
{
    if (current_task == -1)
        return;

    int next = current_task;
    for (int i = 0; i < MAX_TASKS; i++) {
        next = (next + 1) % MAX_TASKS;
        if (tasks[next].active)
            break;
    }

    if (next == current_task)
        return;

    uint64_t *old_rsp = &tasks[current_task].rsp;
    uint64_t new_rsp = tasks[next].rsp;

    current_task = next;
    context_switch(old_rsp, new_rsp);
}
