#include "mmu.h"
#include "heap.h"

#define MMU_RAM_START 0x0100000
#define MMU_RAM_END   0x10000000

#define MMU_FRAMES ((MMU_RAM_END - MMU_RAM_START) / MMU_PAGE_SIZE)
#define MMU_HUGE_SIZE 0x200000

#define MMU_TABLES 256

extern char _end[];

static uint8_t mmu_table_arena[MMU_TABLES * MMU_PAGE_SIZE] __attribute__((aligned(MMU_PAGE_SIZE)));
static int mmu_table_next = 0;

static uint64_t mmu_bitmap[MMU_FRAMES / 64];
static uint64_t mmu_kernel_root;

static uint64_t mmu_alloc_table(void)
{
    if (mmu_table_next >= MMU_TABLES) return 0;
    uint64_t addr = (uint64_t)&mmu_table_arena[mmu_table_next * MMU_PAGE_SIZE];
    mmu_table_next++;
    uint64_t *p = (uint64_t *)addr;
    for (int i = 0; i < MMU_PAGE_SIZE / 8; i++) p[i] = 0;
    return addr;
}

uint64_t mmu_alloc_frame(void)
{
    for (int i = 0; i < MMU_FRAMES; i++) {
        if (mmu_bitmap[i / 64] & (1ULL << (i % 64))) continue;
        mmu_bitmap[i / 64] |= (1ULL << (i % 64));
        uint64_t phys = MMU_RAM_START + (uint64_t)i * MMU_PAGE_SIZE;
        uint64_t *p = (uint64_t *)phys;
        for (int j = 0; j < MMU_PAGE_SIZE / 8; j++) p[j] = 0;
        return phys;
    }
    return 0;
}

void mmu_free_frame(uint64_t phys)
{
    if (phys < MMU_RAM_START || phys >= MMU_RAM_END) return;
    int i = (int)((phys - MMU_RAM_START) / MMU_PAGE_SIZE);
    mmu_bitmap[i / 64] &= ~(1ULL << (i % 64));
}

static uint64_t mmu_walk(uint64_t cr3, uint64_t virt, uint64_t flags, int alloc)
{
    uint64_t pml4i = (virt >> 39) & 0x1FF;
    uint64_t pdpti = (virt >> 30) & 0x1FF;
    uint64_t pdi   = (virt >> 21) & 0x1FF;
    uint64_t pti   = (virt >> 12) & 0x1FF;
    uint64_t inter = MMU_PRESENT | MMU_RW | (flags & MMU_USER);

    uint64_t *pml4 = (uint64_t *)cr3;
    if (!(pml4[pml4i] & MMU_PRESENT)) {
        if (!alloc) return 0;
        uint64_t t = mmu_alloc_table();
        if (!t) return 0;
        pml4[pml4i] = t | inter;
    }
    uint64_t *pdpt = (uint64_t *)(pml4[pml4i] & ~0xFFFULL);

    if (!(pdpt[pdpti] & MMU_PRESENT)) {
        if (!alloc) return 0;
        uint64_t t = mmu_alloc_table();
        if (!t) return 0;
        pdpt[pdpti] = t | inter;
    }
    uint64_t *pd = (uint64_t *)(pdpt[pdpti] & ~0xFFFULL);

    if (!(pd[pdi] & MMU_PRESENT)) {
        if (!alloc) return 0;
        uint64_t t = mmu_alloc_table();
        if (!t) return 0;
        pd[pdi] = t | inter;
    } else if (pd[pdi] & MMU_PS) {
        if (!alloc) return 0;
        uint64_t base = pd[pdi] & ~(MMU_HUGE_SIZE - 1);
        uint64_t keep = (pd[pdi] & 0xFFF) & ~MMU_PS;
        uint64_t pt = mmu_alloc_table();
        if (!pt) return 0;
        uint64_t *ptv = (uint64_t *)pt;
        for (int i = 0; i < 512; i++) ptv[i] = (base + i * MMU_PAGE_SIZE) | keep | MMU_PRESENT;
        pd[pdi] = pt | inter;
    }
    uint64_t *pt = (uint64_t *)(pd[pdi] & ~0xFFFULL);

    return (uint64_t)&pt[pti];
}

void mmu_map_page(uint64_t cr3, uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t *pte = (uint64_t *)mmu_walk(cr3, virt, flags, 1);
    if (!pte) return;
    *pte = (phys & ~0xFFFULL) | flags | MMU_PRESENT;
    __asm__ volatile("invlpg (%0)" :: "r"(virt));
}

void mmu_unmap_page(uint64_t cr3, uint64_t virt)
{
    uint64_t *pte = (uint64_t *)mmu_walk(cr3, virt, 0, 0);
    if (!pte) return;
    *pte = 0;
    __asm__ volatile("invlpg (%0)" :: "r"(virt));
}

int mmu_map_region(uint64_t cr3, uint64_t virt, uint64_t size, uint64_t flags)
{
    uint64_t base = (virt / MMU_PAGE_SIZE) * MMU_PAGE_SIZE;
    uint64_t pages = (size + MMU_PAGE_SIZE - 1) / MMU_PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = mmu_alloc_frame();
        if (!phys) return -1;
        mmu_map_page(cr3, base + i * MMU_PAGE_SIZE, phys, flags);
    }
    return 0;
}

uint64_t mmu_kernel_cr3(void)
{
    return mmu_kernel_root;
}

uint64_t mmu_current_cr3(void)
{
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

void mmu_switch(uint64_t cr3)
{
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3));
}

uint64_t mmu_create_address_space(void)
{
    uint64_t *kr = (uint64_t *)mmu_kernel_root;
    uint64_t new_root = mmu_alloc_table();
    if (!new_root) return 0;
    uint64_t *nr = (uint64_t *)new_root;

    for (int i = 0; i < 512; i++) {
        if (!(kr[i] & MMU_PRESENT)) continue;
        uint64_t new_pdpt = mmu_alloc_table();
        if (!new_pdpt) return 0;
        uint64_t *op = (uint64_t *)(kr[i] & ~0xFFFULL);
        uint64_t *np = (uint64_t *)new_pdpt;
        for (int j = 0; j < 512; j++) {
            if (!(op[j] & MMU_PRESENT)) continue;
            if (op[j] & MMU_PS) {
                np[j] = op[j];
                continue;
            }
            uint64_t new_pd = mmu_alloc_table();
            if (!new_pd) return 0;
            uint64_t *opd = (uint64_t *)(op[j] & ~0xFFFULL);
            uint64_t *npd = (uint64_t *)new_pd;
            for (int k = 0; k < 512; k++) npd[k] = opd[k];
            np[j] = new_pd | (op[j] & 0xFFF) | MMU_USER;
        }
        nr[i] = new_pdpt | (kr[i] & 0xFFF) | MMU_USER;
    }
    return new_root;
}

void mmu_init(uint32_t fb_addr)
{
    mmu_table_next = 0;
    for (int i = 0; i < MMU_FRAMES / 64; i++) mmu_bitmap[i] = 0;

    uint64_t heap_start = ((uint64_t)_end + MMU_PAGE_SIZE - 1) & ~(uint64_t)(MMU_PAGE_SIZE - 1);
    uint64_t reserve_end = heap_start + HEAP_INITIAL_SIZE;
    for (uint64_t a = MMU_RAM_START; a < reserve_end && a < MMU_RAM_END; a += MMU_PAGE_SIZE) {
        int i = (int)((a - MMU_RAM_START) / MMU_PAGE_SIZE);
        mmu_bitmap[i / 64] |= (1ULL << (i % 64));
    }

    uint64_t *pml4 = (uint64_t *)mmu_alloc_table();
    uint64_t *pdpt = (uint64_t *)mmu_alloc_table();
    uint64_t *pd   = (uint64_t *)mmu_alloc_table();

    pml4[0] = (uint64_t)pdpt | MMU_PRESENT | MMU_RW;
    pdpt[0] = (uint64_t)pd   | MMU_PRESENT | MMU_RW;

    uint64_t n_huge = MMU_RAM_END / MMU_HUGE_SIZE;
    for (uint64_t i = 0; i < n_huge; i++)
        pd[i] = (i * MMU_HUGE_SIZE) | MMU_PRESENT | MMU_RW | MMU_PS;

    if (fb_addr) {
        uint64_t fb_pml4i = (fb_addr >> 39) & 0x1FF;
        uint64_t fb_pdpti = (fb_addr >> 30) & 0x1FF;
        uint64_t base = (fb_addr / MMU_HUGE_SIZE) * MMU_HUGE_SIZE;
        uint64_t count = (16 * 1024 * 1024 + MMU_HUGE_SIZE - 1) / MMU_HUGE_SIZE;

        uint64_t *fb_pdpt;
        if (fb_pml4i == 0) {
            fb_pdpt = pdpt;
        } else {
            fb_pdpt = (uint64_t *)mmu_alloc_table();
            pml4[fb_pml4i] = (uint64_t)fb_pdpt | MMU_PRESENT | MMU_RW;
        }
        uint64_t *fb_pd = (uint64_t *)mmu_alloc_table();
        fb_pdpt[fb_pdpti] = (uint64_t)fb_pd | MMU_PRESENT | MMU_RW;
        for (uint64_t i = 0; i < count; i++) {
            uint64_t a = base + i * MMU_HUGE_SIZE;
            fb_pd[(a >> 21) & 0x1FF] = a | MMU_PRESENT | MMU_RW | MMU_PS;
        }
    }

    mmu_kernel_root = (uint64_t)pml4;
    mmu_switch(mmu_kernel_root);
}
