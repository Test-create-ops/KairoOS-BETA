#ifndef TTF_FONT_H
#define TTF_FONT_H

// Bare-metal shim: map stdlib to kernel equivalents
#include <stdint.h>
#include <stddef.h>
#include "memory/heap.h"
#include "lib/memory.h"
#include "utils/string.h"

// ─── Math shims ───
static inline float k_absf(float x) { return x < 0 ? -x : x; }
static inline float k_floorf(float x) { return (float)(int)x - (x < 0 && x != (int)x); }
static inline float k_ceilf(float x) { return (float)(int)x + (x > 0 && x != (int)x); }
static inline float k_sqrtf(float x) {
    if (x <= 0) return 0;
    float g = x / 2;
    for (int i = 0; i < 20; i++) g = (g + x / g) / 2;
    return g;
}
static inline float k_fmodf(float x, float y) { return x - (int)(x/y) * y; }
static inline float k_powf(float base, float exp) {
    if (exp < 0) { base = 1.0f / base; exp = -exp; }
    float r = 1;
    int e = (int)exp;
    for (int i = 0; i < e; i++) r *= base;
    return r;
}
static inline float k_cosf(float x) {
    // Taylor series (reduced to [-pi,pi])
    while (x > 3.14159f) x -= 6.28318f;
    while (x < -3.14159f) x += 6.28318f;
    float r = 1, t = 1;
    t *= -x*x / (2*1); r += t;
    t *= -x*x / (3*4); r += t;
    t *= -x*x / (5*6); r += t;
    t *= -x*x / (7*8); r += t;
    t *= -x*x / (9*10); r += t;
    t *= -x*x / (11*12); r += t;
    return r;
}
static inline float k_acosf(float x) {
    // Simple acos approximation
    float sq = k_sqrtf(1.0f - x*x);
    float r = 1.5708f - x - x*x*x/6.0f - 3*x*x*x*x*x/40.0f;
    (void)sq;
    // More accurate: Newton iteration on cos
    float a = 0.5f;
    for (int i = 0; i < 16; i++) {
        float c = k_cosf(a);
        float dc = -k_sqrtf(1.0f - c*c); // -sin
        if (dc > -0.0001f && dc < 0.0001f) dc = -0.0001f;
        a = a - (c - x) / dc;
    }
    return a;
}

// ─── Prevent ALL stb_truetype #includes ───
#define STBTT_ifloor(x)   ((int) k_floorf(x))
#define STBTT_iceil(x)    ((int) k_ceilf(x))
#define STBTT_sqrt(x)     k_sqrtf(x)
#define STBTT_pow(x,y)    k_powf(x,y)
#define STBTT_fmod(x,y)   k_fmodf(x,y)
#define STBTT_cos(x)      k_cosf(x)
#define STBTT_acos(x)     k_acosf(x)
#define STBTT_fabs(x)     k_absf(x)
#define STBTT_malloc(x,u) ((void)(u),kmalloc(x))
#define STBTT_free(x,u)   ((void)(u),kfree(x))
#define STBTT_assert(x)   ((void)(x))
#define STBTT_strlen(x)   kstrlen(x)
#define STBTT_memcpy      memcpy
#define STBTT_memset      memset

static inline void stb_compat_qsort(void *base, size_t n, size_t sz,
                                    int (*cmp)(const void*, const void*))
{
    char *b = (char *)base;
    for (size_t i = 1; i < n; i++) {
        char tmp[64];
        memcpy(tmp, b + i * sz, sz);
        size_t j = i;
        while (j > 0 && cmp(b + (j-1) * sz, tmp) > 0) {
            memcpy(b + j * sz, b + (j-1) * sz, sz);
            j--;
        }
        memcpy(b + j * sz, tmp, sz);
    }
}
#define qsort stb_compat_qsort

#define STB_TRUETYPE_IMPLEMENTATION

#include "stb_truetype.h"

// ─── TTF Font API for KairoOS ───

// Initialize from embedded font data
typedef struct {
    stbtt_fontinfo info;
    int initialized;
    float scale;
    int ascent, descent, line_gap;
} ttf_font_t;

// Init a font from raw TTF data
static inline int ttf_init(ttf_font_t *font, const unsigned char *data, int data_len) {
    font->initialized = 0;
    if (!stbtt_InitFont(&font->info, data, stbtt_GetFontOffsetForIndex(data, 0)))
        return 0;
    font->initialized = 1;
    return 1;
}

// Set pixel size
static inline void ttf_set_size(ttf_font_t *font, float pixel_height) {
    font->scale = stbtt_ScaleForPixelHeight(&font->info, pixel_height);
    stbtt_GetFontVMetrics(&font->info, &font->ascent, &font->descent, &font->line_gap);
}

// Get advance width for a character
static inline int ttf_get_advance(ttf_font_t *font, int codepoint) {
    int advance = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&font->info, codepoint, &advance, &lsb);
    return (int)(advance * font->scale);
}

// Get kerning between two codepoints
static inline int ttf_get_kern(ttf_font_t *font, int cp1, int cp2) {
    return (int)(stbtt_GetCodepointKernAdvance(&font->info, cp1, cp2) * font->scale);
}

// Rasterize a glyph into an alpha bitmap.
// Caller must free(*pixels) after use.
// Returns bitmap width and height.
static inline int ttf_get_glyph_bitmap(ttf_font_t *font, int codepoint,
                                        unsigned char **pixels,
                                        int *w, int *h,
                                        int *xoff, int *yoff) {
    *pixels = stbtt_GetCodepointBitmap(&font->info, font->scale, font->scale,
                                        codepoint, w, h, xoff, yoff);
    return (*pixels != 0);
}

// Free a bitmap allocated by ttf_get_glyph_bitmap
static inline void ttf_free_bitmap(unsigned char *pixels) {
    stbtt_FreeBitmap(pixels, 0);
}

#endif // TTF_FONT_H
