#include "loader.h"
#include "../memory/heap.h"

#define MAX_PROGRAMS 32

typedef struct {
    const char *name;
    program_t prog;
} prog_entry_t;

static prog_entry_t *table[MAX_PROGRAMS];

static int str_eq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a || *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return 1;
}

void loader_init(void)
{
    for (int i = 0; i < MAX_PROGRAMS; i++)
        table[i] = 0;
}

int loader_register(const char *name, program_t prog)
{
    for (int i = 0; i < MAX_PROGRAMS; i++) {
        if (!table[i]) {
            prog_entry_t *e = kmalloc(sizeof(prog_entry_t));
            e->name = name;
            e->prog = prog;
            table[i] = e;
            return 0;
        }
    }
    return -1;
}

int loader_run(const char *name)
{
    for (int i = 0; i < MAX_PROGRAMS; i++) {
        if (table[i] && str_eq(table[i]->name, name)) {
            table[i]->prog();
            return 0;
        }
    }
    return -1;
}
