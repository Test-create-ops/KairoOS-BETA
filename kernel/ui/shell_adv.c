#include "shell_adv.h"
#include "console.h"
#include "../fs/ramfs.h"
#include "../user/elf.h"

#define BUF 256
static char cmd[BUF];
static int pos = 0;

static void prompt() {
    console_write("\n> ");
}

static void clear_buf() {
    for (int i = 0; i < BUF; i++) cmd[i] = 0;
    pos = 0;
}

static void cmd_ls() {
    console_write("\n[RAMFS files]\n");
    // RAMFS usa VFS, ma non abbiamo lista → simuliamo
    console_write("hello.txt\n");
    console_write("test.elf\n");
}

static void cmd_cat(const char *name) {
    const char *data = ramfs_read_file(name);
    if (!data) {
        console_write("File non trovato\n");
        return;
    }
    console_write("\n");
    console_write(data);
    console_write("\n");
}

static void cmd_run(const char *name) {
    console_write("\n[Eseguo ELF: ");
    console_write(name);
    console_write("]\n");

    if (elf_load_and_run(name) != 0)
        console_write("Errore: ELF non valido o non trovato.\n");
}

static void execute() {
    cmd[pos] = 0;

    if (pos == 0) { prompt(); return; }

    if (cmd[0] == 'l' && cmd[1] == 's') {
        cmd_ls();
    }
    else if (cmd[0]=='c' && cmd[1]=='a' && cmd[2]=='t' && cmd[3]==' ') {
        cmd_cat(cmd+4);
    }
    else if (cmd[0]=='r' && cmd[1]=='u' && cmd[2]=='n' && cmd[3]==' ') {
        cmd_run(cmd+4);
    }
    else {
        console_write("\nComando non riconosciuto: ");
        console_write(cmd);
        console_write("\n");
    }

    clear_buf();
    prompt();
}

void shell_adv_init(void) {
    clear_buf();
    console_write("Shell avanzata pronta.\n");
    prompt();
}

void shell_adv_on_key(char c) {
    if (c == '\n') {
        execute();
        return;
    }
    if (c == '\b') {
        if (pos > 0) pos--;
        return;
    }
    if (pos < BUF-1) {
        cmd[pos++] = c;
        console_putc(c);
    }
}
