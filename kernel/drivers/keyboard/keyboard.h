#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

void keyboard_init(void);
void keyboard_handler(void);
char keyboard_last_char(void);
int is_alt_pressed(void);
int is_ctrl_pressed(void);

#endif
