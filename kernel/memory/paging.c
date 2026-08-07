#include "paging.h"

static uint64_t pml4[PML4_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static uint64_t pdpt[PDP_ENTRIES]  __attribute__((aligned(PAGE_SIZE)));
static uint64_t pd[PD_ENTRIES]     __attribute__((aligned(PAGE_SIZE)));
static uint64_t pt[PT_ENTRIES]     __attribute__((aligned(PAGE_SIZE)));

static void map_page(uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    pml4[pml4_idx] = (uint64_t)&pdpt[0] | flags;
    pdpt[pdpt_idx] = (uint64_t)&pd[0]   | flags;
    pd[pd_idx]     = (uint64_t)&pt[0]   | flags;
    pt[pt_idx]     = phys | flags;
}

void paging_init(uint32_t fb_addr)
{
    for (int i = 0; i < PML4_ENTRIES; i++) pml4[i] = 0;
    for (int i = 0; i < PDP_ENTRIES;  i++) pdpt[i] = 0;
    for (int i = 0; i < PD_ENTRIES;   i++) pd[i]   = 0;
    for (int i = 0; i < PT_ENTRIES;   i++) pt[i]   = 0;

    uint64_t flags = PAGE_PRESENT | PAGE_RW;
    uint64_t huge = PAGE_PRESENT | PAGE_RW | PAGE_PS;

    pml4[0] = (uint64_t)&pdpt[0] | flags;
    pdpt[0] = (uint64_t)&pd[0] | flags;

    // Identity map first 16MB using 2MB huge pages
    for (uint64_t addr = 0; addr < 0x1000000; addr += 0x200000) {
        uint64_t idx = (addr >> 21) & 0x1FF;
        pd[idx] = addr | huge;
    }

    // Map detected framebuffer as 2MB huge pages (16MB total)
    uint64_t fb_flags = PAGE_PRESENT | PAGE_RW | PAGE_PS;
    uint64_t base = (uint64_t)fb_addr;
    for (uint64_t off = 0; off < 0x1000000; off += 0x200000) {
        uint64_t addr = base + off;
        uint64_t fb_pdpt = (addr >> 30) & 0x1FF;
        uint64_t fb_idx  = (addr >> 21) & 0x1FF;
        pdpt[fb_pdpt] = (uint64_t)&pd[0] | PAGE_PRESENT | PAGE_RW;
        pd[fb_idx] = addr | fb_flags;
    }

    uint64_t pml4_phys = (uint64_t)&pml4[0];
    __asm__ volatile("mov %0, %%cr3" :: "r"(pml4_phys));
}

void paging_enable(void)
{
    uint64_t cr0, cr4;

    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1UL << 5); // PAE
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));

    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1UL << 31); // paging enable
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));
}
