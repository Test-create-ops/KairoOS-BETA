#include "usermode.h"
#include "../tss/tss.h"
#include "../proc/proc.h"

extern struct tss64 tss;
extern void usermode_trampoline(uint64_t entry, uint64_t user_stack);
extern long syscall_dispatch(long n, long a1, long a2, long a3);

static uint8_t __usermode_kstack[16384] __attribute__((aligned(16)));

void usermode_enter(uint64_t entry, uint64_t user_stack)
{
    tss.rsp0 = (uint64_t)(__usermode_kstack + sizeof(__usermode_kstack));
    usermode_trampoline(entry, user_stack);
}

void syscall_handler(uint64_t n, uint64_t a1, uint64_t a2, uint64_t a3)
{
    syscall_dispatch((long)n, (long)a1, (long)a2, (long)a3);
}
