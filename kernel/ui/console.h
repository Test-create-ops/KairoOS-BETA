#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>

void console_init(uint32_t w, uint32_t h);
void console_putc(char c);
void console_write(const char *s);
void console_clear(void);

#endif
