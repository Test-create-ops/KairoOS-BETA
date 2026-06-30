#ifndef USERMODE_H
#define USERMODE_H

#include <stdint.h>

void usermode_enter(uint64_t entry, uint64_t user_stack);

#endif
