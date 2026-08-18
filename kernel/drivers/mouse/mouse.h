#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>
#include "../../cursor_pack.h"

void mouse_init(void);
void mouse_poll(void);
void mouse_handler(void);
int mouse_get_x(void);
int mouse_get_y(void);
uint8_t mouse_get_buttons(void);
int mouse_get_scroll(void);
void draw_cursor(void);
void cursor_hide(void);
int mouse_clicked(void);
void mouse_set_cursor(enum cursor_type t);
enum cursor_type mouse_get_cursor(void);
extern int christmas_cursor;

#endif
