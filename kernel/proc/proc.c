#include "proc.h"
#include "../memory/heap.h"
#include "../usermode/usermode.h"

static struct process proc_table[32];
static int proc_count = 0;

struct process *proc_create(void)
{
    if (proc_count >= 32) return 0;
    struct process *p = &proc_table[proc_count++];
    p->pid = proc_count;
    p->rip = 0;
    p->rsp = 0;
    p->next = 0;
    return p;
}

void proc_setup_address_space(struct process *p)
{
    (void)p;
}

void proc_set_entry(struct process *p, unsigned long entry)
{
    p->rip = entry;
}

void proc_set_user_stack(struct process *p)
{
    void *stack = kmalloc(16384);
    p->rsp = (unsigned long)stack + 16384;
}

void proc_run(struct process *p)
{
    usermode_enter(p->rip, p->rsp);
}

void proc_switch(struct process *p)
{
    (void)p;
}

void proc_exit(int code)
{
    (void)code;
    while (1) { __asm__ volatile("hlt"); }
}
