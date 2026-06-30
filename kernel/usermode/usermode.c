#include "usermode.h"

extern void usermode_trampoline(uint64_t entry, uint64_t user_stack);

void usermode_enter(uint64_t entry, uint64_t user_stack)
{
    // In un OS vero qui imposteresti:
    // - segmenti ring3
    // - page table user
    // - stack user
    // - SYSCALL/SYSRET
    // Noi chiamiamo un trampolino ASM che simula il passaggio.
    usermode_trampoline(entry, user_stack);
}
