#include "../../lib/io.h"
#include "../../lib/queue.h"

queue_t key_queue;

void keyboard_init(void)
{
    queue_init(&key_queue);
}

void keyboard_irq(void)
{
    unsigned char sc = inb(0x60);
    queue_push(&key_queue, sc);
}

unsigned char keyboard_get(void)
{
    return queue_pop(&key_queue);
}
