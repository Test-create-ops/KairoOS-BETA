#ifndef PROCESS_H
#define PROCESS_H

typedef struct {
    int pid;
    void (*entry)(void);
} process_t;

void process_create(void (*entry)(void));
void process_run(process_t *p);

#endif
