#include "bt.h"

void bt_init(void) {
    bt_hw_init();
}

void bt_send(const char *msg) {
    bt_hw_send(msg);
}
