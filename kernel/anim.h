#ifndef ANIM_H
#define ANIM_H

/*
 * KairoOS Animation Framework
 * Easing functions + tick-based interpolation
 * All math is integer-only (no FPU in kernel)
 */

/* ─── Easing Functions ───
 * t: progress 0..1000 (fixed-point, 1000 = 1.0)
 * Returns: eased value 0..1000
 */

static inline int ease_linear(int t) {
    return t;
}

static inline int ease_in_quad(int t) {
    return (t * t) / 1000;
}

static inline int ease_out_quad(int t) {
    return t - ((t * (1000 - t)) / 1000);
}

static inline int ease_in_out_quad(int t) {
    if (t < 500) return (t * t) / 500;
    t = 1000 - t;
    return 1000 - (t * t) / 500;
}

static inline int ease_in_cubic(int t) {
    return (t * t * t) / 1000000;
}

static inline int ease_out_cubic(int t) {
    int u = 1000 - t;
    return 1000 - (u * u * u) / 1000000;
}

static inline int ease_in_out_cubic(int t) {
    if (t < 500) return (t * t * t) / 500000;
    int u = 1000 - t;
    return 1000 - (u * u * u) / 500000;
}

static inline int ease_out_back(int t) {
    /* Overshoot: goes past 1000 then settles */
    int s = 1701; /* 1.70158 * 1000 */
    int u = 1000 - t;
    return 1000 - (u * u * ((s + 1000) * u - s)) / 1000000;
}

static inline int ease_out_elastic(int t) {
    if (t == 0 || t == 1000) return t;
    int p = 300; /* period * 1000 */
    int s = p / 400;
    int u = t - s;
    if (u < 0) {
        /* Rising phase */
        int d = s;
        return -(((1000 * u * u) / (d * d)) * 200) / 1000 + 100;
    }
    /* Falling phase */
    int amp = 200; /* amplitude * 100 */
    int decay = t * 10 / 1000; /* approximate exp decay */
    if (decay > 9) decay = 9;
    int wave_table[] = {0, 707, 1000, 707, 0, -707, -1000, -707, 0, 707};
    int wave = wave_table[decay];
    return 1000 - (amp * wave / 1000);
}

static inline int ease_out_bounce(int t) {
    if (t < 250) {
        return (t * t * 16) / 1000;
    } else if (t < 500) {
        t -= 375;
        return (t * t * 16) / 1000 + 750;
    } else if (t < 750) {
        t -= 625;
        return (t * t * 16) / 1000 + 938;
    } else {
        t -= 875;
        return (t * t * 16) / 1000 + 984;
    }
}

/* ─── Interpolation ─── */

/* Lerp: interpolate from a to b by progress t (0..1000) */
static inline int lerp(int a, int b, int t) {
    return a + ((b - a) * t) / 1000;
}

/* Clamp value between min and max */
static inline int clamp(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* ─── Animation Handle ───
 * Usage:
 *   anim_t a;
 *   anim_start(&a, 0, 100, 300);  // from 0 to 100 over 300 ticks
 *   // in loop:
 *   int val = anim_eval(&a, ease_out_cubic);
 *   if (anim_done(&a)) { ... }
 */

typedef struct {
    int from;       /* start value */
    int to;         /* end value */
    int duration;   /* total ticks */
    int elapsed;    /* current ticks */
    int started;    /* 1 if running */
} anim_t;

static inline void anim_start(anim_t *a, int from, int to, int duration) {
    a->from = from;
    a->to = to;
    a->duration = duration;
    a->elapsed = 0;
    a->started = 1;
}

static inline void anim_reset(anim_t *a) {
    a->elapsed = 0;
    a->started = 0;
}

static inline void anim_finish(anim_t *a) {
    a->elapsed = a->duration;
}

/* Evaluate animation at current progress using given easing function */
static inline int anim_eval(anim_t *a, int (*ease)(int)) {
    if (!a->started) return a->from;
    int t = (a->elapsed * 1000) / a->duration;
    if (t > 1000) t = 1000;
    int eased = ease(t);
    return lerp(a->from, a->to, eased);
}

/* Advance animation by dt ticks. Returns 1 if still running */
static inline int anim_tick(anim_t *a, int dt) {
    if (!a->started) return 0;
    a->elapsed += dt;
    if (a->elapsed >= a->duration) {
        a->elapsed = a->duration;
        return 0; /* done */
    }
    return 1; /* still running */
}

static inline int anim_done(anim_t *a) {
    return a->started && a->elapsed >= a->duration;
}

static inline int anim_progress(anim_t *a) {
    if (!a->started || a->duration == 0) return 1000;
    int t = (a->elapsed * 1000) / a->duration;
    return (t > 1000) ? 1000 : t;
}

/* ─── Spring Physics (for dock magnification) ───
 * Simple damped spring: x'' = -k*(x-target) - d*x'
 * Returns new position, velocity is updated in-place
 */
static inline int spring_update(int pos, int target, int *vel, int stiffness, int damping) {
    int force = (target - pos) * stiffness / 100;
    *vel = (*vel + force) * damping / 100;
    return pos + *vel;
}

/* ─── Color Interpolation ─── */
static inline uint32_t color_lerp(uint32_t c1, uint32_t c2, int t) {
    int r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
    int r2 = (c2 >> 16) & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = c2 & 0xFF;
    int r = lerp(r1, r2, t);
    int g = lerp(g1, g2, t);
    int b = lerp(b1, b2, t);
    return (r << 16) | (g << 8) | b;
}

#endif
