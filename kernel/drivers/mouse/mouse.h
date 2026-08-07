#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

void mouse_init(void);
void mouse_poll(void);
void mouse_handler(void);
int mouse_get_x(void);
int mouse_get_y(void);
uint8_t mouse_get_buttons(void);
void draw_cursor(void);
int mouse_clicked(void);
extern int christmas_cursor;

#endif
