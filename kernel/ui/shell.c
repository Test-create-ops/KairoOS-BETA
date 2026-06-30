#include "shell.h"
#include "console.h"
#include "../user/loader.h"

#define SHELL_BUF_SIZE 128
static char buf[SHELL_BUF_SIZE];
static int pos = 0;

static void shell_prompt(void)
{
    console_write("> ");
}

void shell_init(void)
{
    pos = 0;
    for (int i = 0; i < SHELL_BUF_SIZE; i++)
        buf[i] = 0;
    console_write("Shell pronta.\n");
    shell_prompt();
}

static void shell_exec(void)
{
    buf[pos] = 0;
    if (pos == 0) {
        console_write("\n");
        shell_prompt();
        return;
    }

    if (loader_run(buf) != 0) {
        console_write("\nComando non trovato: ");
        console_write(buf);
        console_write("\n");
    } else {
        console_write("\n");
    }

    pos = 0;
    shell_prompt();
}

void shell_on_key(char c)
{
    if (c == '\n') {
        shell_exec();
        return;
    }
    if (c == '\b') {
        if (pos > 0) {
            pos--;
            console_write("\b");
        }
        return;
    }
    if (pos < SHELL_BUF_SIZE - 1) {
        buf[pos++] = c;
        console_putc(c);
    }
}
