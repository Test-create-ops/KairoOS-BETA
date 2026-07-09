#include "bt.h"
#include "../usb/usb.h"

#define BT_NAME_LEN 48

static struct {
    int  present;
    int  usb_idx;
    int  vendor_id;
    int  product_id;
    char name[BT_NAME_LEN];
} bt_dev;

/* Known Bluetooth USB vendor IDs */
static const uint16_t known_bt_vids[] = {
    0x8087, /* Intel */
    0x0A12, /* Cambridge Silicon Radio (CSR) */
    0x0B05, /* Asus */
    0x13D3, /* IMC Networks / AzureWave */
    0x0489, /* Foxconn / Lite-On */
    0x0930, /* Toshiba */
    0x0CF3, /* Atheros / Qualcomm */
    0x04CA, /* Broadcom */
    0x05AC, /* Apple */
    0x1131, /* Integrated System Solution (ISSC) */
    0x0BDA, /* Realtek */
};
#define NUM_BT_VIDS (sizeof(known_bt_vids) / sizeof(known_bt_vids[0]))

int bt_init(void) {
    bt_dev.present = 0;
    bt_dev.usb_idx = -1;
    bt_dev.vendor_id = 0;
    bt_dev.product_id = 0;
    bt_dev.name[0] = 0;

    int dev_count = usb_device_count();
    for (int i = 0; i < dev_count; i++) {
        int vid = usb_device_vid(i);
        int pid = usb_device_pid(i);
        for (unsigned int v = 0; v < NUM_BT_VIDS; v++) {
            if ((uint16_t)vid == known_bt_vids[v]) {
                bt_dev.present = 1;
                bt_dev.usb_idx = i;
                bt_dev.vendor_id = vid;
                bt_dev.product_id = pid;
                const char *un = usb_device_name(i);
                int j;
                for (j = 0; un[j] && j < BT_NAME_LEN - 1; j++)
                    bt_dev.name[j] = un[j];
                bt_dev.name[j] = 0;
                return 1;
            }
        }
    }
    return 0;
}

int bt_is_present(void) {
    return bt_dev.present;
}

const char* bt_get_name(void) {
    return bt_dev.name;
}

int bt_get_vendor(void) {
    return bt_dev.vendor_id;
}

int bt_get_product(void) {
    return bt_dev.product_id;
}
