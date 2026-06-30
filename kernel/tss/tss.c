#include "tss.h"

uint8_t __stack_kernel[4096] __attribute__((aligned(16)));
uint8_t __ist1_stack[4096] __attribute__((aligned(16)));
uint8_t __ist2_stack[4096] __attribute__((aligned(16)));
uint8_t __ist3_stack[4096] __attribute__((aligned(16)));
uint8_t __ist4_stack[4096] __attribute__((aligned(16)));

extern struct tss64 tss;

void tss_init(void)
{
    tss.rsp0 = (uint64_t)(__stack_kernel + sizeof(__stack_kernel));
    tss.ist1 = (uint64_t)(__ist1_stack + sizeof(__ist1_stack));
    tss.ist2 = (uint64_t)(__ist2_stack + sizeof(__ist2_stack));
    tss.ist3 = (uint64_t)(__ist3_stack + sizeof(__ist3_stack));
    tss.ist4 = (uint64_t)(__ist4_stack + sizeof(__ist4_stack));
    tss.iomap_base = sizeof(struct tss64);
}
