#ifndef USB_H
#define USB_H

int usb_init(void);
int usb_device_count(void);
const char* usb_device_name(int index);
int usb_device_vid(int index);
int usb_device_pid(int index);
int usb_poll(void);
int usb_hid_read(int *x, int *y, int *btn);

#endif
