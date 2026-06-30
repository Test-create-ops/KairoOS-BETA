#include "timer.h"

static void (*timer_cb)(void) = 0;

void timer_set_callback(void (*cb)(void))
{
    timer_cb = cb;
}

void timer_tick(void)
{
    if (timer_cb)
        timer_cb();
}
