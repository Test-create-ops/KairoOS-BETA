#include "proc.h"
#include "../memory/heap.h"
#include "../memory/mmu.h"
#include "../usermode/usermode.h"

#define USER_STACK_TOP 0x70000000
#define USER_STACK_SIZE 16384

static struct proc proc_table[32];
static int proc_count = 0;

struct proc *proc_create(void)
{
    if (proc_count >= 32) return 0;
    struct proc *p = &proc_table[proc_count++];
    p->pid = proc_count;
    p->rip = 0;
    p->rsp = 0;
    p->cr3 = 0;
    p->next = 0;
    return p;
}

void proc_setup_address_space(struct proc *p)
{
    p->cr3 = mmu_create_address_space();
}

void proc_set_entry(struct proc *p, unsigned long entry)
{
    p->rip = entry;
}

void proc_set_user_stack(struct proc *p)
{
    unsigned long base = USER_STACK_TOP - USER_STACK_SIZE;
    for (unsigned long off = 0; off < USER_STACK_SIZE; off += MMU_PAGE_SIZE) {
        uint64_t phys = mmu_alloc_frame();
        if (!phys) break;
        mmu_map_page(p->cr3, base + off, phys, MMU_PRESENT | MMU_RW | MMU_USER);
    }
    p->rsp = USER_STACK_TOP;
}

void proc_run(struct proc *p)
{
    if (!p) return;
    mmu_switch(p->cr3);
    usermode_enter(p->rip, p->rsp);
}

void proc_switch(struct proc *p)
{
    if (!p || !p->cr3) return;
    mmu_switch(p->cr3);
}

void proc_exit(int code)
{
    (void)code;
    __asm__ volatile("cli");
    mmu_switch(mmu_kernel_cr3());
    kernel_longjmp(kctx);
    while (1) { __asm__ volatile("hlt"); }
}
