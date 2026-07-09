#ifndef ASSERT_H
#define ASSERT_H

#define ASSERT(cond) do { \
    if (!(cond)) { \
        gfx_print(10, 10, 0xFF0000, "ASSERT FAILED"); \
        for(;;); \
    } \
} while(0)

#endif