#include "stdio.h"
#include "../kernel/ui/console.h"
#include <stdarg.h>

void printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    while (*fmt) {
        if (*fmt == '%' && *(fmt+1) == 's') {
            fmt += 2;
            const char *s = va_arg(args, const char*);
            console_write(s);
        } else if (*fmt == '%' && *(fmt+1) == 'd') {
            fmt += 2;
            int v = va_arg(args, int);
            char buf[32];
            int i = 0;
            if (v == 0) buf[i++] = '0';
            while (v > 0) { buf[i++] = '0' + (v % 10); v /= 10; }
            while (i--) console_putc(buf[i]);
        } else {
            console_putc(*fmt++);
        }
    }

    va_end(args);
}
