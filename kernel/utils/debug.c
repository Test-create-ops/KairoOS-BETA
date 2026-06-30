#include "debug.h"
#include "log.h"

void debug_hex(unsigned long value) {
    log_hex(value);
}

void debug_msg(const char *msg) {
    log_info(msg);
}
