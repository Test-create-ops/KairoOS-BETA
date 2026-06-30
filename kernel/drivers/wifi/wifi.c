#include "wifi.h"

void wifi_init(void) {
    wifi_hw_init();
}

void wifi_send(const char *msg) {
    wifi_hw_send(msg);
}
