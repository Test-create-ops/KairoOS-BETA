#include "../lib/syscall.h"
#include "../lib/string.h"

static char input_buf[128];

static int str_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static int str_startswith(const char *s, const char *prefix)
{
    while (*prefix)
        if (*s++ != *prefix++) return 0;
    return 1;
}

void entry(void)
{
    write("Shell pronta.\n");

    while (1) {
        write("> ");
        int n = read(input_buf, 128);
        input_buf[n] = 0;

        if (str_eq(input_buf, "ls")) {
            write("bin init shell\n");
        } else if (str_startswith(input_buf, "run ")) {
            exec(input_buf + 4);
        } else {
            write("Comando non trovato.\n");
        }
    }
}
