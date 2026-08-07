#include "../mmu/mmu.h"
#include "../memory/mmu.h"
#include "../proc/proc.h"

void *alloc_user_region(void *proc, unsigned long vaddr, unsigned long size)
{
    struct proc *p = (struct proc *)proc;
    unsigned long pages = (size + MMU_PAGE_SIZE - 1) / MMU_PAGE_SIZE;
    unsigned long base = (vaddr / MMU_PAGE_SIZE) * MMU_PAGE_SIZE;

    if (!p || !p->cr3 || size == 0) return 0;

    for (unsigned long i = 0; i < pages; i++) {
        uint64_t phys = mmu_alloc_frame();
        if (!phys) return 0;
        mmu_map_page(mmu_kernel_cr3(), base + i * MMU_PAGE_SIZE, phys,
                     MMU_PRESENT | MMU_RW);
        mmu_map_page(p->cr3, base + i * MMU_PAGE_SIZE, phys,
                     MMU_PRESENT | MMU_RW | MMU_USER);
    }
    return (void *)base;
}
