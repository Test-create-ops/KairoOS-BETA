#include "../mmu/mmu.h"
#include "../memory/heap.h"

void *alloc_user_region(void *proc, unsigned long vaddr, unsigned long size)
{
    (void)vaddr;
    struct process *p = (struct process *)proc;
    (void)p;
    return kmalloc(size);
}
