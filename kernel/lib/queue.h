#ifndef QUEUE_H
#define QUEUE_H

typedef struct {
    unsigned char data[256];
    int head, tail, count;
} queue_t;

static inline void queue_init(queue_t *q)
{
    q->head = q->tail = q->count = 0;
}

static inline void queue_push(queue_t *q, unsigned char c)
{
    if (q->count < 256) {
        q->data[q->tail] = c;
        q->tail = (q->tail + 1) & 255;
        q->count++;
    }
}

static inline unsigned char queue_pop(queue_t *q)
{
    unsigned char c = 0;
    if (q->count > 0) {
        c = q->data[q->head];
        q->head = (q->head + 1) & 255;
        q->count--;
    }
    return c;
}

#endif
