#include "ble.h"

void ble_init(void) {
    ble_hw_init();
}

void ble_send(const char *msg) {
    ble_hw_send(msg);
}
