static long ksys(long n, long a1, long a2, long a3)
{
    long ret;
    __asm__ volatile(
        "mov %1, %%rax\n"
        "mov %2, %%rbx\n"
        "mov %3, %%rcx\n"
        "mov %4, %%rdx\n"
        "int $0x80\n"
        "mov %%rax, %0\n"
        : "=r"(ret)
        : "r"(n), "r"(a1), "r"(a2), "r"(a3)
        : "rax", "rbx", "rcx", "rdx", "memory");
    return ret;
}

static void print(const char *s)
{
    ksys(0, (long)s, 0, 0);
}

static volatile unsigned long counter = 0;

static void spin(void)
{
    volatile unsigned long i;
    for (i = 0; i < 4000000; i++) counter++;
}

void _start(void)
{
    print("Kairo-OS userland: PID 1 alive in ring3!\n");
    print("Syscalls: write (0), read (1), exec (2), exit (3)\n");
    spin();
    print("User-mode init finished its work.\n");
    print("Handing control back to the kernel...\n");
    ksys(3, 0, 0, 0);   /* exit(0) -> kernel_longjmp resumes the GUI */
    for (;;) { }
}
