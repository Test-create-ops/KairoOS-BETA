#ifndef MMU_MEMORY_H
#define MMU_MEMORY_H

#include <stdint.h>

#define MMU_PAGE_SIZE 4096

#define MMU_PRESENT   (1ULL << 0)
#define MMU_RW        (1ULL << 1)
#define MMU_USER      (1ULL << 2)
#define MMU_PS        (1ULL << 7)

void mmu_init(uint32_t fb_addr);

uint64_t mmu_alloc_frame(void);
void mmu_free_frame(uint64_t phys);

void mmu_map_page(uint64_t cr3, uint64_t virt, uint64_t phys, uint64_t flags);
void mmu_unmap_page(uint64_t cr3, uint64_t virt);
int mmu_map_region(uint64_t cr3, uint64_t virt, uint64_t size, uint64_t flags);

uint64_t mmu_kernel_cr3(void);
uint64_t mmu_current_cr3(void);
uint64_t mmu_create_address_space(void);
void mmu_switch(uint64_t cr3);

#endif
