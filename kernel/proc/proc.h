#pragma once
#include <stdint.h>

struct proc {
    unsigned long pid;
    unsigned long rip;
    unsigned long rsp;
    uint64_t cr3;
    struct proc *next;
};

struct proc *proc_create();
void proc_setup_address_space(struct proc *p);
void proc_set_entry(struct proc *p, unsigned long entry);
void proc_set_user_stack(struct proc *p);
void proc_run(struct proc *p);
void proc_switch(struct proc *p);
void proc_exit(int code);
