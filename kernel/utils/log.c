#include "log.h"
#include "../ui/console.h"

static void log_prefix(const char *p) {
    console_write(p);
}

void log_info(const char *msg) {
    log_prefix("[INFO] ");
    console_write(msg);
    console_write("\n");
}

void log_warn(const char *msg) {
    log_prefix("[WARN] ");
    console_write(msg);
    console_write("\n");
}

void log_error(const char *msg) {
    log_prefix("[ERR ] ");
    console_write(msg);
    console_write("\n");
}

void log_hex(unsigned long value) {
    char buf[32];
    int i = 0;
    if (value == 0) buf[i++] = '0';
    while (value) {
        int d = value & 0xF;
        buf[i++] = (d < 10) ? ('0' + d) : ('A' + d - 10);
        value >>= 4;
    }
    console_write("0x");
    while (i--) console_putc(buf[i]);
    console_write("\n");
}
