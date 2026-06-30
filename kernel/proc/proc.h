#pragma once
struct process {
    unsigned long pid;
    unsigned long rip;
    unsigned long rsp;
    struct process *next;
};

struct process *proc_create();
void proc_setup_address_space(struct process *p);
void proc_set_entry(struct process *p, unsigned long entry);
void proc_set_user_stack(struct process *p);
void proc_run(struct process *p);
void proc_switch(struct process *p);
void proc_exit(int code);
