#include "../proc/proc.h"
#include "../drivers/timer/timer.h"

static struct process *current = 0;
static struct process *plist = 0;

void sched_add(struct process *p)
{
    p->next = plist;
    plist = p;
}

void sched_tick(void)
{
    if (!plist) return;

    if (!current)
        current = plist;
    else
        current = current->next ? current->next : plist;

    proc_switch(current);
}

void sched_init(void)
{
    timer_set_callback(sched_tick);
}
