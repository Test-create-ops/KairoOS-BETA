#include "process.h"
#include "scheduler.h"
#include "../memory/heap.h"

#define MAX_PROCESSES 32
#define PROCESS_STACK_SIZE (16 * 1024)

static process_t processes[MAX_PROCESSES];
static uint32_t next_pid = 1;

void process_init(void)
{
    for (int i = 0; i < MAX_PROCESSES; i++) {
        processes[i].pid = 0;
        processes[i].entry = 0;
        processes[i].stack_top = 0;
        processes[i].active = 0;
    }
    next_pid = 1;
}

process_t *process_create(uint64_t entry)
{
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (!processes[i].active) {
            processes[i].active = 1;
            processes[i].pid = next_pid++;
            processes[i].entry = entry;

            void *stack = kmalloc(PROCESS_STACK_SIZE);
            if (!stack) {
                processes[i].active = 0;
                return NULL;
            }

            processes[i].stack_top = (uint64_t)stack + PROCESS_STACK_SIZE;

            scheduler_add_task(processes[i].entry, processes[i].stack_top);
            return &processes[i];
        }
    }
    return NULL;
}
