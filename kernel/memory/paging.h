#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

#define PAGE_SIZE 4096

#define PML4_ENTRIES 512
#define PDP_ENTRIES  512
#define PD_ENTRIES   512
#define PT_ENTRIES   512

#define PAGE_PRESENT   (1ULL << 0)
#define PAGE_RW        (1ULL << 1)
#define PAGE_USER      (1ULL << 2)
#define PAGE_PWT       (1ULL << 3)
#define PAGE_PCD       (1ULL << 4)
#define PAGE_ACCESSED  (1ULL << 5)
#define PAGE_DIRTY     (1ULL << 6)
#define PAGE_PS        (1ULL << 7)
#define PAGE_GLOBAL    (1ULL << 8)

void paging_init(uint32_t fb_addr);
void paging_enable(void);

#endif
