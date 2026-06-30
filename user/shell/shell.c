#include "shell.h"
#include "../../kernel/ui/console.h"
#include "../../kernel/fs/ramfs.h"
#include "../../kernel/user/elf.h"

void user_shell_start(void)
{
    console_write("User Shell pronta.\n");

    while (1) {
        console_write("> ");
        char buf[128];
        int pos = 0;

        while (1) {
            unsigned char sc = 0;
            __asm__ volatile("inb $0x60, %0" : "=a"(sc));
            if (sc == 0x1C) break;
            if (sc < 0x80) {
                buf[pos++] = sc;
                console_putc(sc);
            }
        }
        buf[pos] = 0;

        if (pos == 0) continue;

        if (buf[0]=='r' && buf[1]=='u' && buf[2]=='n' && buf[3]==' ') {
            const char *file = buf + 4;
            elf_load_and_run(file);
        } else {
            console_write("\nComando non riconosciuto.\n");
        }
    }
}
