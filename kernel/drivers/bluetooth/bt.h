#ifndef BT_H
#define BT_H

int  bt_init(void);
int  bt_is_present(void);
const char* bt_get_name(void);
int  bt_get_vendor(void);
int  bt_get_product(void);

#endif
