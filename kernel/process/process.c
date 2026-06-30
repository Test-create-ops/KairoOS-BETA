#include "process.h"

static process_t proc;

void process_create(void (*entry)(void)) {
    proc.pid = 1;
    proc.entry = entry;
}

void process_run(process_t *p) {
    p->entry();
}
