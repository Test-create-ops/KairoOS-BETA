#include "kernel.h"
#include "lib/io.h"
#include "lib/framebuffer.c"
#include "wallpaper.h"
#include "anim.h"

static void serial_puts(const char *s);
static void serial_dec(int v);

#include "gdt/gdt.c"
#include "tss/tss.c"
#include "idt/idt.c"
#include "interrupts/isr.c"
#include "interrupts/irq.c"
#include "memory/paging.c"
#include "memory/heap.c"
#include "memory/mmu.c"
#include "drivers/graphics/gfx.c"
#include "ttf_font.h"
#include "material_icons_ttf.h"
#include "drivers/graphics/render3d.h"
#include "../ui/renderer/renderer.c"
#include "../ui/windows/window.c"
#include "drivers/mouse/mouse.c"
#include "drivers/keyboard/keyboard.c"
#include "drivers/usb/usb.c"
#include "drivers/camera/camera.c"
#include "drivers/bluetooth/bt.c"
#include "drivers/audio/ac97.c"
#include "drivers/audio/stt.c"
#include "drivers/net/rtl8139.c"
#include "drivers/net/netstack.c"
#include "drivers/net/http.c"
#include "drivers/ahci/ahci.c"
#include "scheduler/scheduler.c"
#include "scheduler/process.c"
#include "usermode/usermode.c"
#include "syscall/syscall.c"
#include "boot_screen.c"
#include "launch_pad.c"

// ─── TTF Icon Font (Material Icons) ───
static ttf_font_t icon_font;
static int icon_font_ready = 0;

// Material Icons codepoints for common OS icons
#define ICON_HOME        0xE88F
#define ICON_SETTINGS    0xE8B8
#define ICON_SEARCH      0xE8B6
#define ICON_CLOSE       0xE5CD
#define ICON_ARROW_UP    0xE5CE
#define ICON_ARROW_DOWN  0xE5CF
#define ICON_ARROW_LEFT  0xE5CB
#define ICON_ARROW_RIGHT 0xE5CC
#define ICON_CHECK       0xE5CA
#define ICON_STAR        0xE838
#define ICON_WIFI        0xE63E
#define ICON_BATTERY     0xE1A3
#define ICON_FOLDER      0xE2C7
#define ICON_FILE        0xE245
#define ICON_DELETE      0xE872
#define ICON_EDIT        0xE3C9
#define ICON_PLAY        0xE037
#define ICON_PAUSE       0xE034
#define ICON_STOP        0xE047
#define ICON_ADD         0xE145
#define ICON_REMOVE      0xE15B
#define ICON_CHEVRON_UP  0xE5C7
#define ICON_CHEVRON_DN  0xE5C5
#define ICON_REFRESH     0xE5D5
#define ICON_TERMINAL    0xE90E
#define ICON_INFO        0xE88E
#define ICON_WARNING     0xE002
#define ICON_ERROR       0xE000
#define ICON_PALETTE     0xE40A
#define ICON_MUSIC_NOTE  0xE405
#define ICON_BRIGHTNESS  0xE3AB
#define ICON_VOLUME_UP   0xE050
#define ICON_CALENDAR    0xE935
#define ICON_CHAT        0xE0B9
#define ICON_CAMERA      0xE3B0
#define ICON_AIRPLANE    0xE539
#define ICON_BLUETOOTH   0xE1A7
#define ICON_LOCK        0xE897
#define ICON_UNLOCK      0xE898
#define ICON_CALCULATOR  0xF0EC
#define ICON_SHOP        0xE8F4
#define ICON_BRAIN       0xEF69
#define ICON_COMPUTER    0xE30A
#define ICON_MOVIE       0xE028
#define ICON_TIMER       0xE425
#define ICON_CLOUD       0xE812
#define ICON_PUBLIC      0xEA21
#define ICON_KEYBOARD    0xE322
#define ICON_GAME        0xE929
#define ICON_GAMES       0xE936
#define ICON_MIC         0xE319
#define ICON_SCHEDULER   0xE935
#define ICON_PHONE       0xE0CD
#define ICON_CALL        0xE0B0
#define ICON_DIALPAD     0xE319

static void icon_font_init(void) {
    if (ttf_init(&icon_font, material_icons_ttf, material_icons_ttf_len)) {
        ttf_set_size(&icon_font, 20.0f);
        icon_font_ready = 1;
    }
}

// Render a Material Icon glyph at (x, y) with given color
static void draw_icon(int x, int y, int codepoint, uint32_t color, float size) {
    if (!icon_font_ready) return;
    float saved_scale = icon_font.scale;
    ttf_set_size(&icon_font, size);
    unsigned char *bmp = 0;
    int w, h, xoff, yoff;
    ttf_get_glyph_bitmap(&icon_font, codepoint, &bmp, &w, &h, &xoff, &yoff);
    if (bmp) {
        gfx_blit_alpha(x + xoff, y + yoff, w, h, bmp, color);
        ttf_free_bitmap(bmp);
    }
    ttf_set_size(&icon_font, saved_scale);
}

// ─── Dock Icon Bitmap Cache (pre-rendered at boot, zero heap during hover) ───
#define ICON_CACHE_SIZES 13
#define ICON_CACHE_ICONS 10
#define ICON_CACHE_BMP   64

typedef struct {
    unsigned char bmp[ICON_CACHE_BMP * ICON_CACHE_BMP];
    int w, h;
} icon_cache_entry_t;

static icon_cache_entry_t icon_cache[ICON_CACHE_ICONS][ICON_CACHE_SIZES];
static int icon_cache_ready = 0;

static const int _dock_cache_cp[10] = {
    ICON_PUBLIC, ICON_MUSIC_NOTE, ICON_GAME, ICON_CHAT, ICON_CALCULATOR,
    ICON_EDIT, ICON_SHOP, ICON_CLOUD, ICON_GAMES, ICON_COMPUTER
};

static void icon_cache_init(void) {
    float saved_scale = icon_font.scale;
    for (int i = 0; i < ICON_CACHE_ICONS; i++) {
        for (int s = 0; s < ICON_CACHE_SIZES; s++) {
            float sz = 24.0f + (float)s;
            ttf_set_size(&icon_font, sz);
            int x0, y0, x1, y1;
            stbtt_GetCodepointBitmapBox(&icon_font.info, _dock_cache_cp[i],
                icon_font.scale, icon_font.scale, &x0, &y0, &x1, &y1);
            int bw = x1 - x0, bh = y1 - y0;
            icon_cache_entry_t *e = &icon_cache[i][s];
            if (bw > 0 && bh > 0 && bw <= ICON_CACHE_BMP && bh <= ICON_CACHE_BMP) {
                stbtt_MakeCodepointBitmap(&icon_font.info, e->bmp, bw, bh, bw,
                    icon_font.scale, icon_font.scale, _dock_cache_cp[i]);
                e->w = bw; e->h = bh;
            } else {
                e->w = 0; e->h = 0;
            }
        }
    }
    ttf_set_size(&icon_font, saved_scale);
    icon_cache_ready = 1;
}

static void draw_icon_centered(int box_x, int box_y, int box_sz, int codepoint, uint32_t color, float size) {
    if (!icon_font_ready) return;
    if (icon_cache_ready) {
        for (int i = 0; i < ICON_CACHE_ICONS; i++) {
            if (_dock_cache_cp[i] == codepoint) {
                int si = (int)(size + 0.5f) - 24;
                if (si < 0) si = 0;
                if (si >= ICON_CACHE_SIZES) si = ICON_CACHE_SIZES - 1;
                icon_cache_entry_t *e = &icon_cache[i][si];
                if (e->w > 0) {
                    int dx = box_x + (box_sz - e->w) / 2;
                    int dy = box_y + (box_sz - e->h) / 2;
                    gfx_blit_alpha(dx, dy, e->w, e->h, e->bmp, color);
                    return;
                }
                break;
            }
        }
    }
    float saved_scale = icon_font.scale;
    ttf_set_size(&icon_font, size);
    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(&icon_font.info, codepoint, icon_font.scale, icon_font.scale, &x0, &y0, &x1, &y1);
    int bw = x1 - x0, bh = y1 - y0;
    if (bw > 0 && bh > 0 && bw <= 64 && bh <= 64) {
        static unsigned char icon_buf[64 * 64];
        stbtt_MakeCodepointBitmap(&icon_font.info, icon_buf, bw, bh, bw, icon_font.scale, icon_font.scale, codepoint);
        int dx = box_x + (box_sz - bw) / 2;
        int dy = box_y + (box_sz - bh) / 2;
        gfx_blit_alpha(dx, dy, bw, bh, icon_buf, color);
    }
    ttf_set_size(&icon_font, saved_scale);
}

// Buffer per il fallback STT: mono 16k, max 5s (invio seriale al proxy)
static int16_t stt_tmp_mono[80000];

// Nuova UI: finestre gestite da window_t (ui/windows/window.c) e chrome
// disegnato dal renderer font8x8 (ui/renderer/renderer.c)
static int wx, wy, ww, wh;
static window_t g_win;
static const char *g_win_title = 0;
static int g_win_init = 0;

// Kernel context saved before launching a ring3 program; sys_exit
// restores it via kernel_longjmp so kernel_main resumes into the GUI.
extern int kernel_setjmp(unsigned long ctx[8]);
extern void kernel_longjmp(const unsigned long ctx[8]);
static unsigned long kctx[8];

// String utilities (needed by VFS, ELF, etc.)
#include "utils/string.c"
#include "lib/string.c"
#include "lib/memory.c"

// Process abstraction (user-space processes, used by ELF loader)
#include "proc/proc.c"

// File System
#include "fs/vfs.c"
#include "fs/ramfs.c"
#include "fs/kfs.c"
#include "initrd/initrd.c"

// ELF program loader
#include "elf/elf.c"




void kernel_early(void) {
    gdt_init();
    tss_init();
    idt_init();
    isr_init();
    irq_init();
}

void kernel_memory(uint32_t fb_addr) {
    paging_init(fb_addr);
    paging_enable();
    mmu_init(fb_addr);
    kheap_init();
}

void kernel_drivers(void) {
    keyboard_init();
    mouse_init();
    pci_scan();
    ac97_init();
    rtl8139_init();
    ahci_init();
}

void kernel_tasks(void) {
    scheduler_init();
    process_init();
}

static uint32_t g_fb_addr = 0;

int strmatch(const char *s, const char *t);

static int cmos_read(int reg) {
    outb(0x70, reg | 0x80);  // select register, disable NMI
    int val = inb(0x71);
    outb(0x70, 0x80);        // deselect, keep NMI disabled
    return val;
}

static void rtc_read(int *h, int *m) {
    while (cmos_read(0x0A) & 0x80);
    int regb = cmos_read(0x0B);
    int bin = regb & 4;
    int raw = cmos_read(0x04);
    int vm = cmos_read(0x02);
    if (!(regb & 2)) {
        // 12-hour mode: strip PM bit, convert BCD→decimal, then to 24h
        int pm = raw & 0x80;
        raw &= 0x7F;
        int vh = bin ? raw : (((raw>>4)&0xF)*10 + (raw&0x0F));
        if (pm) { if (vh != 12) vh += 12; }
        else { if (vh == 12) vh = 0; }
        *h = vh;
    } else {
        // 24-hour mode: just convert BCD→decimal if needed
        *h = bin ? raw : (((raw>>4)&0xF)*10 + (raw&0x0F));
    }
    *m = bin ? vm : (((vm>>4)&0xF)*10 + (vm&0x0F));
}

static void rtc_read_date(int *m, int *d) {
    while (cmos_read(0x0A) & 0x80);
    int regb = cmos_read(0x0B);
    int bin = regb & 4;
    int vm = cmos_read(0x08), vd = cmos_read(0x07);
    if (bin) { *m = vm; *d = vd; }
    else { *m = ((vm>>4)*10)+(vm&0x0F); *d = ((vd>>4)*10)+(vd&0x0F); }
}

static void rtc_read_time(int *h, int *m, int *s) {
    while (cmos_read(0x0A) & 0x80);
    int regb = cmos_read(0x0B);
    int bin = regb & 4;
    int raw = cmos_read(0x04);
    int vm = cmos_read(0x02);
    int vs = cmos_read(0x00);
    if (!(regb & 2)) {
        // 12-hour mode: strip PM bit, convert BCD→decimal, then to 24h
        int pm = raw & 0x80;
        raw &= 0x7F;
        int vh = bin ? raw : (((raw>>4)&0xF)*10 + (raw&0x0F));
        if (pm) { if (vh != 12) vh += 12; }
        else { if (vh == 12) vh = 0; }
        *h = vh;
    } else {
        // 24-hour mode: just convert BCD→decimal if needed
        *h = bin ? raw : (((raw>>4)&0xF)*10 + (raw&0x0F));
    }
    *m = bin ? vm : (((vm>>4)&0xF)*10 + (vm&0x0F));
    *s = bin ? vs : (((vs>>4)&0xF)*10 + (vs&0x0F));
}

static void rtc_read_full(int *y, int *m, int *d, int *wd) {
    while (cmos_read(0x0A) & 0x80);
    int regb = cmos_read(0x0B);
    int bin = regb & 4;
    int vy = cmos_read(0x09), vm = cmos_read(0x08), vd = cmos_read(0x07), vw = cmos_read(0x06);
    if (bin) { *y = vy; *m = vm; *d = vd; *wd = vw; }
    else {
        *y = ((vy>>4)*10)+(vy&0x0F);
        *m = ((vm>>4)*10)+(vm&0x0F);
        *d = ((vd>>4)*10)+(vd&0x0F);
        *wd = (vw&0x0F);
    }
    *y += 2000;  // CMOS stores two-digit year
}

#define BG      0x0A0A28
#define BORDER  0x00E5FF
#define ACCENT  0x3B82F6   // unico blu accent dell'interfaccia (stile macOS)
#define TEXT    0x88CCFF
#define DIM     0x5A6A8A
#define PANEL_BG 0x0E1030
#define PROG_BG 0x1A1A4E
#define PROG_FG 0x00E5FF
#define SHADOW  0x001050

// Dark Mode → softer, dimmer colors
#define BG_DM      0x040410
#define ACCENT_DM  0x3366CC
#define TEXT_DM    0x6699BB
#define DIM_DM     0x3A4A6A
#define PANEL_BG_DM 0x060618
#define PROG_BG_DM 0x0E0E28

static void draw_progress(int x, int y, int w, int pct)
{
    gfx_rect(x, y, w, 8, PROG_BG);
    gfx_rect(x, y, w * pct / 100, 8, PROG_FG);
    gfx_rect(x + w * pct / 100, y, 2, 8, 0x66FFFF);
}

static void delay(void) {
    for (volatile int i = 0; i < 4000000; i++);
}

static void play_click(void) {
    // Frequency ~800 Hz (divisor 1491 = 0x05D3)
    outb(0x43, 0xB6);
    outb(0x42, 0xD3);
    outb(0x42, 0x05);
    int tmp = inb(0x61);
    outb(0x61, tmp | 3);
    for (volatile int i = 0; i < 2000000; i++);
    outb(0x61, tmp & 0xFC);
}

static void play_jingle_bells(void) {
    if (!ac97_initialized) return;
    // Short Jingle Bells — non-blocking via speaker_tone + delay per note
    int _nj_notes[] = {659,659,659,784,523,587,659, 698,698,698,698,698,659,659, 784,784,587,523};
    int _nj_durs[] = {80,80,160,80,80,80,200, 80,80,80,80,80,80,80, 120,120,120,300};
    for (int _ni = 0; _ni < 18; _ni++) {
        speaker_tone(_nj_notes[_ni]);
        for (volatile int _nd = 0; _nd < _nj_durs[_ni] * 12000; _nd++) asm volatile("pause");
    }
    speaker_off();
}

// Freq sweep — defined in ac97.c

static void animate_bar(int x, int y, int w, int target) {
    for (int p = 0; p <= target; p += 4) {
        draw_progress(x, y, w, p);
        delay();
    }
    draw_progress(x, y, w, target);
}

// ─── Serial port (COM1) for network proxy ───
#define SERIAL_PORT 0x3F8

static void serial_init(void) {
    outb(SERIAL_PORT + 1, 0x00);
    outb(SERIAL_PORT + 3, 0x80);
    outb(SERIAL_PORT + 0, 0x01);
    outb(SERIAL_PORT + 1, 0x00);
    outb(SERIAL_PORT + 3, 0x03);
    outb(SERIAL_PORT + 2, 0xC7);
}

static int serial_available(void) {
    return inb(SERIAL_PORT + 5) & 1;
}

static char serial_read(void) {
    while (!serial_available());
    return inb(SERIAL_PORT + 0);
}

static void serial_write(char c) {
    while (!(inb(SERIAL_PORT + 5) & 0x20));
    outb(SERIAL_PORT + 0, c);
}

static void serial_puts(const char *s) {
    while (*s) serial_write(*s++);
}

static void serial_dec(int v) {
    char tmp[16]; int n = 0;
    if (v < 0) { serial_write('-'); v = -v; }
    if (v == 0) { serial_write('0'); return; }
    while (v > 0 && n < 15) { tmp[n++] = '0' + (v % 10); v /= 10; }
    while (n > 0) serial_write(tmp[--n]);
}

static int serial_read_timeout(char *c, int max_loops) {
    for (int _i = 0; _i < max_loops; _i++) {
        if (serial_available()) { *c = inb(SERIAL_PORT + 0); return 1; }
        for (volatile int _d = 0; _d < 500000; _d++);
    }
    return 0;
}

// Wi-Fi panel state (scan reale via proxy)
static char wifi_names[16][40];
static int wifi_sig[16];
static int wifi_sec[16];
static int wifi_count = 0, wifi_connected = 0, wifi_scan_done = 0;

static void wifi_request_scan(void)
{
    wifi_count = 0;
    wifi_connected = 0;
    wifi_scan_done = 0;
    while (serial_available()) serial_read();
    serial_puts("WIFI|SCAN\n");
    for (int _tw = 0; _tw < 15000; _tw++) {
        if (serial_available()) break;
        for (volatile int _d = 0; _d < 300000; _d++);
    }
    char _rline[64];
    while (serial_available()) {
        int _ri = 0;
        while (_ri < 63) {
            if (!serial_read_timeout(&_rline[_ri], 4000)) break;
            if (_rline[_ri] == '\n' || _rline[_ri] == '\r') break;
            _ri++;
        }
        _rline[_ri] = 0;
        if (_rline[0] == 'E' && _rline[1] == 'N' && _rline[2] == 'D' && _rline[3] == 0) break;
        if (_rline[0] == 'W' && _rline[1] == 'I' && _rline[2] == 'F' && _rline[3] == 'I' && _rline[4] == '|' && wifi_count < 15) {
            char *p = _rline + 5;
            int _n = 0;
            while (*p && *p != '|' && _n < 39) wifi_names[wifi_count][_n++] = *p++;
            wifi_names[wifi_count][_n] = 0;
            if (*p == '|') p++;
            int _rssi = 0, _neg = 0;
            if (*p == '-') { _neg = 1; p++; }
            while (*p && *p != '|' && *p >= '0' && *p <= '9') { _rssi = _rssi * 10 + (*p - '0'); p++; }
            if (*p == '|') p++;
            wifi_sig[wifi_count] = _neg ? -_rssi : _rssi;
            wifi_sec[wifi_count] = (p[0] == 'O' && p[1] == 'P') ? 0 : 1;
            wifi_count++;
        }
    }
    wifi_scan_done = 1;
}

static int wifi_bars(int rssi)
{
    if (rssi >= -55) return 4;
    if (rssi >= -65) return 3;
    if (rssi >= -75) return 2;
    if (rssi >= -85) return 1;
    return 1;
}

// Ricezione bulk per l'audio: poll veloce senza delay, con tetto di poll vuoti.
static int serial_read_bulk(char *buf, int n, int max_empty) {
    int got = 0, empty = 0;
    while (got < n) {
        if (serial_available()) { buf[got++] = inb(SERIAL_PORT + 0); empty = 0; }
        else { if (++empty > max_empty) break; }
    }
    return got;
}

#include "system_transfer.c"

#define MAX_POSTS 20

typedef struct {
    char sub[24];
    char author[24];
    char title[120];
    int score;
} SocialPost;

// Syntax-highlighted line printer for Kairo Studio
// Splits line into colored segments and prints each.
static void studio_print_hl(int mode, const char *line, int x, int y) {
    char seg[62]; int si = 0, sx = x;
    uint32_t col = 0xFFFFFF;
    int st = 0; // 0=normal, 1=in-tag-bracket, 2=tag-name, 3=string, 4=comment-line, 5=attr-space, 6=squote
    int i;
    #define SEG_F() do { if (si) { seg[si]=0; gfx_print(sx,y,col,seg); sx+=si*8; si=0; }} while(0)
    #define SEG_P(c) do { if (si<60) seg[si++]=c; } while(0)
    #define KW(c,kw) (((c)>='a'&&(c)<='z')||((c)>='A'&&(c)<='Z')||(c)=='_')
    for (i = 0; line[i] && si < 60; i++) {
        char c = line[i];
        if (mode == 0) { // XML
            if (st == 4) { SEG_P(c); if (c=='>' && i>=3 && line[i-1]=='-' && line[i-2]=='-') { SEG_F(); st=0; col=0xFFFFFF; } continue; }
            if (c == '<') { SEG_F(); if (line[i+1]=='?') { col=0x3A4A6A; st=4; SEG_P(c); }
                           else if (line[i+1]=='/') { col=0x4488AA; SEG_P(c); st=1; }
                           else if (line[i+1]=='!') { col=0x3A4A6A; st=4; do { SEG_P(line[i]); i++; } while(line[i] && line[i]!='>'); if(line[i]=='>') { SEG_P('>'); SEG_F(); st=0; col=0xFFFFFF; } } 
                           else { col=0x4488AA; SEG_P(c); st=1; } continue; }
            if (c == '>') { SEG_F(); col=0x4488AA; SEG_P(c); SEG_F(); col=0xFFFFFF; st=0; continue; }
            if (st == 1) { if (c != ' ' && c != '/') { col=0x44AADD; st=2; SEG_P(c); } else { SEG_P(c); } continue; }
            if (st == 2) { if (c == ' ') { SEG_F(); col=0xFFFFFF; st=5; SEG_P(c); } else if (c == '/') { SEG_F(); col=0x4488AA; st=0; SEG_P(c); } else if (c == '>') { SEG_F(); col=0x4488AA; SEG_P(c); SEG_F(); col=0xFFFFFF; st=0; } else { SEG_P(c); } continue; }
            if (st == 5) { if (c == '=') { SEG_F(); col=0xFFFFFF; SEG_P(c); st=0; } else if (c != ' ') { SEG_F(); col=0xFFFFFF; st=0; SEG_P(c); } else { SEG_P(c); } continue; }
            if (c == '"') { SEG_F(); col=0x44AA44; SEG_P(c); st=3; continue; }
            if (st == 3) { SEG_P(c); if (c == '"') { SEG_F(); col=0xFFFFFF; st=0; } continue; }
            if (c >= '0' && c <= '9' && st==0) { SEG_F(); col=0xFFAA44; SEG_P(c); SEG_F(); col=0xFFFFFF; continue; }
            SEG_P(c);
        } else if (mode == 1) { // JS
            if (st == 4) { SEG_P(c); continue; }
            if (c == '"') { SEG_F(); col=0x44AA44; SEG_P(c); st=3; continue; }
            if (c == '\'') { SEG_F(); col=0x44AA44; SEG_P(c); st=6; continue; }
            if (st == 3) { SEG_P(c); if (c == '"') { SEG_F(); col=0xFFFFFF; st=0; } continue; }
            if (st == 6) { SEG_P(c); if (c == '\'') { SEG_F(); col=0xFFFFFF; st=0; } continue; }
            if (c == '/' && line[i+1] == '/') { SEG_F(); col=0x8888CC; st=4; do { SEG_P(line[i]); i++; } while(line[i]); break; }
            if (c == '{' || c == '}') { SEG_F(); col=0xAAAA44; SEG_P(c); SEG_F(); col=0xFFFFFF; continue; }
            if (c >= '0' && c <= '9') { SEG_F(); col=0xFFAA44; SEG_P(c); SEG_F(); col=0xFFFFFF; continue; }
            // Simple keyword check
            if (KW(c,0)) {
                static const char *const kws[] = {"class","constructor","this","function","var","let","const","if","else","return","new","for","while","do","switch","case","break","continue","import","export","from","try","catch","finally","typeof","instanceof","void","delete","in","of","yield","async","await","static","get","set","extends","super","throw","default",0};
                int is_kw = 0;
                for (int kw_i = 0; kws[kw_i]; kw_i++) {
                    int j; for (j = 0; kws[kw_i][j] && line[i+j] && kws[kw_i][j] == line[i+j]; j++);
                    if (kws[kw_i][j]==0 && !KW(line[i+j],0)) { is_kw=1; break; }
                }
                if (is_kw) { SEG_F(); col=0x4466DD; while(KW(line[i],0)||(line[i]>='0'&&line[i]<='9')){SEG_P(line[i]);i++;} i--; SEG_F(); col=0xFFFFFF; continue; }
            }
            SEG_P(c);
        } else { // CSS
            if (st == 4) { SEG_P(c); if (c=='*' && line[i+1]=='/') { i++; SEG_P('/'); SEG_F(); st=0; col=0xFFFFFF; } continue; }
            if (c == '/' && line[i+1] == '*') { SEG_F(); col=0x3A4A6A; st=4; SEG_P(c); continue; }
            if (c == '{' || c == '}') { SEG_F(); col=0xAAAA44; SEG_P(c); SEG_F(); col=0xFFFFFF; continue; }
            if (c == ':') { SEG_F(); col=0x88AACC; SEG_P(c); SEG_F(); col=0xFFFFFF; continue; }
            SEG_P(c);
        }
    }
    SEG_F();
    #undef SEG_F
    #undef SEG_P
    #undef KW
}

// ─────────────────────────────────────────────────────────────
// HOME overlay + OEM Unlocker + BootLoader Command Line
// ─────────────────────────────────────────────────────────────

static int bootloader_unlocked = 0;   // REAL unlock flag (persistent kernel-global)
static int _pending_launch = 0;       // app launched after a full desktop redraw (Home)
static int home_active = 0;           // 1 = Home overlay open
static int home_focus = 0;            // 0 = apps grid, 1 = System Apps
static int home_sel = 0;              // selected app in the left grid
static int home_sys_sel = 0;          // selected System App on the right
static int oem_active = 0;            // 1 = Kairo Blue BIOS open

#define HOME_COLS 5
#define HOME_APP_COUNT 31
#define HOME_PW 540
#define HOME_PH 480
#define HOME_VROWS 5
static int home_scroll = 0;
static const int home_app_act[HOME_APP_COUNT] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 16, 17, 18, 19,
    20, 21, 23, 24, 25, 26, 28, 29, 30, 31, 32, 33, 34, 35, 36, 39
};
static const char *const home_app_name[HOME_APP_COUNT] = {
    "Terminal","Settings","OreoAI","Calculator","Notes","App Store",
    "Studio","KairoVM","Camera","Player","True Video","Calendar",
    "Pomodoro","Weather","Monitor","Art","Typing","Files","Tetris",
    "Games","Snake","Wii","Mic Test","Pong","Paint","Maze","Music",
    "Dino","Browser","Chat","Phone"
};

static char bl_lines[40][80];
static int bl_count = 0, bl_scroll = 0;
static char bl_buf[128];
static int bl_pos = 0;

static char syslog_lines[60][80];
static int syslog_count = 0, syslog_scroll = 0;

// Forward declarations for VoIP callbacks
static void phone_redraw(int dmc);
static void syslog_add(const char *s);

// ─── Phone (Dialer) app state ───
static char phone_num[20];
static int phone_pos = 0;
static int phone_sel = 0;   // 0-11 = dial pad, 12 = call, 13 = hangup
static int phone_calling = 0;
static int phone_ring_tick = 0;

// ─── VoIP / RTP (over UDP) ───
#define VOIP_PORT       5004
#define VOIP_SIGNAL_PORT 5060
#define RTP_VERSION     2
#define RTP_PAYLOAD_PCM 0       // raw PCM 8kHz mono 16-bit
#define RTP_HDR_SIZE    12
#define VOIP_SAMPLE_RATE 8000
#define VOIP_FRAME_MS   20
#define VOIP_FRAME_SIZE (VOIP_SAMPLE_RATE * VOIP_FRAME_MS / 1000)  // 160 samples
#define VOIP_JITTER_BUF 4

typedef enum { VOIP_IDLE, VOIP_INVITING, VOIP_RINGING, VOIP_ACTIVE, VOIP_HANGING } voip_state_t;
static voip_state_t voip_state = VOIP_IDLE;
static uint32_t voip_peer_ip = 0;
static uint16_t voip_peer_port = VOIP_PORT;
static uint32_t voip_seq = 0;
static uint32_t voip_ssrc = 0x12345678;
static uint16_t voip_local_port = VOIP_PORT;

// Signal messages (UDP on port 5060)
#define SIG_INVITE   0x01
#define SIG_ACCEPT   0x02
#define SIG_HANGUP   0x03
#define SIG_RINGING  0x04

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  pad[3];
    uint32_t ip;
    uint16_t port;
    uint16_t pad2;
} voip_signal_t;

// Jitter buffer for incoming audio
static int16_t voip_jitter_buf[VOIP_JITTER_BUF][VOIP_FRAME_SIZE];
static int voip_jitter_head = 0;
static int voip_jitter_count = 0;

// RTP header (12 bytes)
static void rtp_build(uint8_t *hdr, uint8_t payload_type, uint32_t seq, uint32_t ts, uint32_t ssrc) {
    hdr[0] = (RTP_VERSION << 6) | payload_type;
    hdr[1] = 0;
    hdr[2] = (seq >> 8) & 0xFF;
    hdr[3] = seq & 0xFF;
    hdr[4] = (ts >> 24) & 0xFF;
    hdr[5] = (ts >> 16) & 0xFF;
    hdr[6] = (ts >> 8) & 0xFF;
    hdr[7] = ts & 0xFF;
    hdr[8] = (ssrc >> 24) & 0xFF;
    hdr[9] = (ssrc >> 16) & 0xFF;
    hdr[10] = (ssrc >> 8) & 0xFF;
    hdr[11] = ssrc & 0xFF;
}

// Send VoIP signal (invite/accept/hangup)
static void voip_send_signal(uint8_t type, uint32_t dst_ip) {
    voip_signal_t sig;
    sig.type = type;
    sig.pad[0] = sig.pad[1] = sig.pad[2] = 0;
    sig.ip = htonl(net_get_ip());
    sig.port = htons(voip_local_port);
    sig.pad2 = 0;
    udp_send(dst_ip, VOIP_SIGNAL_PORT, VOIP_SIGNAL_PORT, (uint8_t*)&sig, sizeof(sig));
}

// Send audio frame via RTP
static void voip_send_audio(const int16_t *samples, int count) {
    uint8_t pkt[RTP_HDR_SIZE + VOIP_FRAME_SIZE * 2 + 4];
    rtp_build(pkt, RTP_PAYLOAD_PCM, voip_seq, voip_seq * VOIP_FRAME_SIZE, voip_ssrc);
    voip_seq++;
    for (int i = 0; i < count; i++) {
        pkt[RTP_HDR_SIZE + i*2] = (samples[i] >> 8) & 0xFF;
        pkt[RTP_HDR_SIZE + i*2 + 1] = samples[i] & 0xFF;
    }
    udp_send(voip_peer_ip, voip_peer_port, voip_local_port, pkt, RTP_HDR_SIZE + count * 2);
}

// Handle incoming VoIP signal
static void voip_signal_handler(uint32_t src_ip, uint16_t src_port, const uint8_t *data, int len) {
    if (len < sizeof(voip_signal_t)) return;
    voip_signal_t *sig = (voip_signal_t*)data;

    switch (sig->type) {
        case SIG_INVITE:
            if (voip_state == VOIP_IDLE) {
                voip_peer_ip = src_ip;
                voip_peer_port = ntohs(sig->port);
                voip_state = VOIP_RINGING;
                syslog_add("[PHONE] Incoming call!");
                ac97_play_notify();
            }
            break;
        case SIG_ACCEPT:
            if (voip_state == VOIP_INVITING) {
                voip_peer_port = ntohs(sig->port);
                voip_state = VOIP_ACTIVE;
                voip_seq = 0;
                ac97_start_capture();
                syslog_add("[PHONE] Call connected!");
                ac97_play_confirm();
            }
            break;
        case SIG_HANGUP:
            voip_state = VOIP_IDLE;
            ac97_stop_capture();
            syslog_add("[PHONE] Call ended");
            phone_calling = 0;
            break;
        case SIG_RINGING:
            if (voip_state == VOIP_INVITING) {
                syslog_add("[PHONE] Ringing...");
            }
            break;
    }
}

// Handle incoming RTP audio
static void voip_rtp_handler(uint32_t src_ip, uint16_t src_port, const uint8_t *data, int len) {
    if (len < RTP_HDR_SIZE + 2) return;
    if (voip_state != VOIP_ACTIVE) return;

    // Store in jitter buffer
    int16_t *frame = voip_jitter_buf[(voip_jitter_head + voip_jitter_count) % VOIP_JITTER_BUF];
    int samples = (len - RTP_HDR_SIZE) / 2;
    if (samples > VOIP_FRAME_SIZE) samples = VOIP_FRAME_SIZE;
    for (int i = 0; i < samples; i++) {
        frame[i] = (int16_t)((data[RTP_HDR_SIZE + i*2] << 8) | data[RTP_HDR_SIZE + i*2 + 1]);
    }
    if (voip_jitter_count < VOIP_JITTER_BUF) voip_jitter_count++;
}

// Combined VoIP UDP callback (handles both signaling and RTP audio)
static void voip_udp_callback(uint32_t src_ip, uint16_t src_port, const uint8_t *data, int len) {
    // Signal messages are exactly 8 bytes; RTP packets are larger
    if (len == sizeof(voip_signal_t)) {
        voip_signal_handler(src_ip, src_port, data, len);
    } else if (len > RTP_HDR_SIZE && voip_state == VOIP_ACTIVE) {
        voip_rtp_handler(src_ip, src_port, data, len);
    }
}

// Start a call
static void voip_start_call(void) {
    // Parse phone_num as IP: "10.0.2.15" format or just use last octet "15"
    uint32_t ip = 0;
    int parts[4] = {0,0,0,0};
    int part = 0;
    for (int i = 0; phone_num[i]; i++) {
        if (phone_num[i] == '.') { part++; continue; }
        if (phone_num[i] >= '0' && phone_num[i] <= '9') {
            parts[part] = parts[part] * 10 + (phone_num[i] - '0');
        }
    }
    if (parts[0] == 0 && parts[1] == 0 && parts[2] == 0 && parts[3] == 0) return;

    // Support shorthand: just last octet (e.g. "2" → 10.0.2.2)
    if (parts[1] == 0 && parts[2] == 0 && parts[3] != 0 && parts[0] < 256 && parts[0] != 0) {
        // Full IP
        ip = (parts[0]<<24)|(parts[1]<<16)|(parts[2]<<8)|parts[3];
    } else if (parts[3] != 0 || parts[2] != 0 || parts[1] != 0) {
        ip = (parts[0]<<24)|(parts[1]<<16)|(parts[2]<<8)|parts[3];
    } else {
        // Just last octet
        ip = (10<<24)|(0<<16)|(2<<8)|parts[0];
    }

    voip_peer_ip = ip;
    voip_state = VOIP_INVITING;
    voip_seq = 0;
    syslog_add("[PHONE] Calling...");
    voip_send_signal(SIG_INVITE, ip);
}

// Hang up
static void voip_hangup(void) {
    if (voip_state != VOIP_IDLE) {
        voip_send_signal(SIG_HANGUP, voip_peer_ip);
        voip_state = VOIP_IDLE;
    }
}

static void phone_redraw(int _dmc) {
    int px = wx + 8, py = wy + 44, pw = ww - 16, ph = wh - 56;

    gfx_fill_round_rect(px, py, pw, ph, 6, _dmc ? 0x03030E : 0x08081C);
    gfx_round_rect(px, py, pw, ph, 6, _dmc ? 0x1A2A4A : 0x2A5AAA);

    // Title
    draw_icon(px + 8, py + 4, ICON_PHONE, 0x44FF88, 18.0f);
    gfx_print(px + 30, py + 6, 0x44FF88, "Phone");
    if (voip_state == VOIP_ACTIVE) {
        gfx_print(px + pw - 80, py + 6, 0x44FF44, "Connected");
    } else if (voip_state == VOIP_INVITING) {
        gfx_print(px + pw - 80, py + 6, 0xFFFF44, "Calling...");
    } else if (voip_state == VOIP_RINGING) {
        // Blink "Incoming call"
        static int _ring_blink = 0; _ring_blink++;
        if ((_ring_blink / 20) % 2 == 0)
            gfx_print(px + pw - 120, py + 6, 0xFF8844, "Incoming!");
    } else if (phone_calling) {
        gfx_print(px + pw - 80, py + 6, 0x44FF44, "Calling...");
    }
    gfx_rect(px, py + 26, pw, 1, _dmc ? 0x1A2A4A : 0x2A4A7A);

    // Display (number being dialed)
    int dx = px + 12, dy = py + 34, dw = pw - 24, dh = 28;
    gfx_fill_round_rect(dx, dy, dw, dh, 4, 0x000000);
    gfx_round_rect(dx, dy, dw, dh, 4, phone_calling ? 0x228822 : 0x3A5A8A);
    // Show number centered
    {
        int nl = 0; while (phone_num[nl]) nl++;
        int max_chars = dw / 8;
        int start = 0;
        if (nl > max_chars) start = nl - max_chars;
        gfx_print(dx + 6, dy + 8, phone_calling ? 0x44FF44 : 0xFFFFFF, phone_num + start);
        // Blinking cursor
        static int _phone_blink = 0; _phone_blink++;
        if (!phone_calling && (_phone_blink / 30) % 2 == 0) {
            int cx = dx + 6 + (nl - start) * 8;
            if (cx < dx + dw - 4)
                gfx_rect(cx, dy + 6, 2, 16, 0x44FF88);
        }
    }

    // Dial pad grid: 4 rows x 3 cols
    const char *pad_labels[12] = {
        "1", "2", "3",
        "4", "5", "6",
        "7", "8", "9",
        "*", "0", "#"
    };
    const char *pad_sub[12] = {
        "", "ABC", "DEF",
        "GHI", "JKL", "MNO",
        "PQRS", "TUV", "WXYZ",
        "", "+", ""
    };

    int pad_x = px + 20, pad_y = py + 72;
    int btn_w = (pw - 60) / 3;
    int btn_h = 36;
    int btn_gap = 6;

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 3; c++) {
            int idx = r * 3 + c;
            int bx = pad_x + c * (btn_w + btn_gap);
            int by = pad_y + r * (btn_h + btn_gap);
            int focused = (phone_sel == idx);

            if (focused) {
                gfx_fill_round_rect(bx - 2, by - 2, btn_w + 4, btn_h + 4, 8, 0x1A3A6A);
            }
            gfx_fill_round_rect(bx, by, btn_w, btn_h, 6, _dmc ? 0x0A1A30 : 0x102848);
            gfx_round_rect(bx, by, btn_w, btn_h, 6, focused ? 0x44AAFF : (_dmc ? 0x1A2A4A : 0x2A4A7A));

            // Digit centered
            int dl = 0; while (pad_labels[idx][dl]) dl++;
            gfx_print(bx + (btn_w - dl * 8) / 2, by + 6, focused ? 0xFFFFFF : 0xCCDDEE, pad_labels[idx]);

            // Sub-text (letters)
            if (pad_sub[idx][0]) {
                int sl = 0; while (pad_sub[idx][sl]) sl++;
                gfx_print(bx + (btn_w - sl * 5) / 2, by + 22, _dmc ? 0x2A3A5A : 0x4A6A8A, pad_sub[idx]);
            }
        }
    }

    // Call / Hangup button
    int act_y = pad_y + 4 * (btn_h + btn_gap) + 4;
    int act_w = pw - 40;
    int act_h = 32;

    if (voip_state == VOIP_ACTIVE || phone_calling) {
        // Hangup button (red)
        int focused = (phone_sel == 13);
        if (focused) gfx_fill_round_rect(pad_x - 2, act_y - 2, act_w + 4, act_h + 4, 8, 0x4A1010);
        gfx_fill_round_rect(pad_x, act_y, act_w, act_h, 6, 0x4A1010);
        gfx_round_rect(pad_x, act_y, act_w, act_h, 6, focused ? 0xFF4444 : 0x882222);
        draw_icon(pad_x + 8, act_y + 6, ICON_STOP, 0xFF6666, 20.0f);
        gfx_print(pad_x + 34, act_y + 8, 0xFFFFFF, "Hang Up");
    } else if (voip_state == VOIP_RINGING) {
        // Accept (green) / Reject (red) side by side
        int aw = (act_w - 6) / 2;
        { int focused = (phone_sel == 12);
          if (focused) gfx_fill_round_rect(pad_x - 2, act_y - 2, aw + 4, act_h + 4, 8, 0x104A10);
          gfx_fill_round_rect(pad_x, act_y, aw, act_h, 6, 0x103A10);
          gfx_round_rect(pad_x, act_y, aw, act_h, 6, focused ? 0x44FF44 : 0x228822);
          draw_icon(pad_x + 8, act_y + 6, ICON_CALL, 0x44FF44, 20.0f);
          gfx_print(pad_x + 34, act_y + 8, 0xFFFFFF, "Accept");
        }
        { int rx = pad_x + aw + 6, focused = (phone_sel == 13);
          if (focused) gfx_fill_round_rect(rx - 2, act_y - 2, aw + 4, act_h + 4, 8, 0x4A1010);
          gfx_fill_round_rect(rx, act_y, aw, act_h, 6, 0x4A1010);
          gfx_round_rect(rx, act_y, aw, act_h, 6, focused ? 0xFF4444 : 0x882222);
          draw_icon(rx + 8, act_y + 6, ICON_DELETE, 0xFF6666, 20.0f);
          gfx_print(rx + 34, act_y + 8, 0xFFFFFF, "Reject");
        }
    } else {
        // Call button (green)
        int focused = (phone_sel == 12);
        if (focused) gfx_fill_round_rect(pad_x - 2, act_y - 2, act_w + 4, act_h + 4, 8, 0x104A10);
        gfx_fill_round_rect(pad_x, act_y, act_w, act_h, 6, 0x103A10);
        gfx_round_rect(pad_x, act_y, act_w, act_h, 6, focused ? 0x44FF44 : 0x228822);
        draw_icon(pad_x + 8, act_y + 6, ICON_CALL, 0x44FF44, 20.0f);
        gfx_print(pad_x + 34, act_y + 8, 0xFFFFFF, phone_pos > 0 ? "Call" : "Dial");
    }

    // Backspace button (right side of call/hangup)
    if (!phone_calling && phone_pos > 0) {
        int bs_x = pad_x + act_w - 40, bs_y = act_y;
        gfx_fill_round_rect(bs_x, bs_y, 36, act_h, 6, _dmc ? 0x2A1A1A : 0x3A2020);
        gfx_round_rect(bs_x, bs_y, 36, act_h, 6, 0x884444);
        draw_icon(bs_x + 8, bs_y + 6, ICON_DELETE, 0xFF6666, 20.0f);
    }

    // Footer
    gfx_print(px + 8, py + ph - 14, _dmc ? 0x1A2A4A : 0x3A5A7A,
              "[U/D/L/R] nav  [Enter] press  [Bksp] del  [Esc]");
}

static void bl_add(const char *s)
{
    while (*s && bl_count < 40) {
        int take = 0;
        while (s[take] && take < 38) take++;
        if (take == 38) {
            while (take > 0 && s[take] != ' ') take--;
            if (take < 1) take = 38;
        }
        int i;
        for (i = 0; i < take; i++) bl_lines[bl_count][i] = s[i];
        bl_lines[bl_count][i] = 0;
        bl_count++;
        s += take;
        if (*s == ' ') s++;
    }
    bl_scroll = bl_count - 1;
    if (bl_scroll < 0) bl_scroll = 0;
}

static int bl_has(const char *s, const char *sub)
{
    if (!*sub) return 1;
    for (; *s; s++) {
        const char *a = s, *b = sub;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

static void bl_open(void)
{
    bl_count = 0; bl_scroll = 0; bl_pos = 0; bl_buf[0] = 0;
    bl_add("[KairoOS BootLoader Command Line v1.0]");
    if (bootloader_unlocked) {
        bl_add("Bootloader: UNLOCKED");
        bl_add("Type 'help' for commands.");
    } else {
        bl_add("Bootloader: LOCKED");
        bl_add("Use OEM Unclocker in System Apps to unlock it.");
    }
}

static void bl_redraw(void)
{
    gfx_fill_round_rect(wx + 8, wy + 44, ww - 16, wh - 56, 6, 0x02020A);
    gfx_round_rect(wx + 8, wy + 44, ww - 16, wh - 56, 6, bootloader_unlocked ? 0x0A3A7A : 0x550000);
    gfx_print(wx + 16, wy + 46, bootloader_unlocked ? 0x66AAFF : 0xFF6644,
              bootloader_unlocked ? "BOOTLOADER UNLOCKED — KAIRO BLUE" : "BOOTLOADER LOCKED — USE OEM UNCLOCKER");
    gfx_rect(wx + 8, wy + 58, ww - 16, 1, 0x1A3A5A);
    int _ml = (wh - 80) / 16; if (_ml < 1) _ml = 1;
    int _start = bl_scroll - _ml + 1; if (_start < 0) _start = 0;
    int _ty = wy + 66;
    for (int i = _start; i <= bl_scroll && i < bl_count; i++) {
        uint32_t _c = 0x66CCFF;
        if (bl_lines[i][0] == '[') _c = 0xFFCC44;
        if (bl_lines[i][0] == '/') _c = 0x44FF66;
        if (bl_lines[i][0] == '-') _c = 0xFFAA66;
        gfx_print(wx + 16, _ty, _c, bl_lines[i]);
        _ty += 16;
    }
    char _pb[80]; int _pn = 0, _pi;
    char _pfx[] = "BLoader:~$ ";
    for (_pi = 0; _pfx[_pi]; _pi++) _pb[_pn++] = _pfx[_pi];
    for (_pi = 0; _pi < bl_pos && _pi < 70; _pi++) _pb[_pn++] = bl_buf[_pi];
    _pb[_pn] = 0;
    gfx_print(wx + 16, wy + 44 + wh - 56 - 16, 0x44FF66, _pb);
}

static void syslog_add(const char *s) {
    while (*s && syslog_count < 60) {
        int take = 0;
        while (s[take] && take < 38) take++;
        if (take == 38) { while (take > 0 && s[take] != ' ') take--; if (take < 1) take = 38; }
        int i; for (i = 0; i < take; i++) syslog_lines[syslog_count][i] = s[i];
        syslog_lines[syslog_count][i] = 0;
        syslog_count++;
        s += take;
        if (*s == ' ') s++;
    }
    syslog_scroll = syslog_count - 1;
    if (syslog_scroll < 0) syslog_scroll = 0;
}

static void syslog_redraw(void) {
    gfx_fill_round_rect(wx+8,wy+44,ww-16,wh-56,6,0x020208);
    gfx_round_rect(wx+8,wy+44,ww-16,wh-56,6,0x2A2A6A);
    gfx_print(wx+16,wy+46,0x8888FF,"Syslog Viewer");
    gfx_print(wx+ww-100,wy+46,0x4466AA,bootloader_unlocked?"UNLOCKED":"LOCKED");
    gfx_rect(wx+8,wy+58,ww-16,1,0x1A1A4A);
    int _ml = (wh-80)/16; if(_ml<1)_ml=1;
    int _start = syslog_scroll-_ml+1; if(_start<0)_start=0;
    int _ty = wy+66;
    for(int i=_start;i<=syslog_scroll&&i<syslog_count;i++){
        uint32_t _c=0x66CCFF;
        if(syslog_lines[i][0]=='['&&syslog_lines[i][1]=='I')_c=0x44AAFF;
        if(syslog_lines[i][0]=='['&&syslog_lines[i][1]=='W')_c=0xFFCC44;
        if(syslog_lines[i][0]=='['&&syslog_lines[i][1]=='E')_c=0xFF4444;
        if(syslog_lines[i][0]=='['&&syslog_lines[i][1]=='B')_c=0x44FF44;
        gfx_print(wx+16,_ty,_c,syslog_lines[i]); _ty+=16;
    }
    gfx_print(wx+20,wy+wh-18,0x2A3A5A,"[U/D] scroll  [Esc] close");
}

static void syslog_open(void) {
    syslog_count=0; syslog_scroll=0;
    syslog_add("[BOOT] KairoOS kernel v1.0 starting...");
    syslog_add("[BOOT] CPU: x86_64 Long Mode");
    syslog_add("[INFO] Memory: 256 MB detected");
    syslog_add("[INFO] Display: VBE 1280x720x32");
    syslog_add("[INFO] PCI bus scanned");
    syslog_add("[INFO] AC97 audio controller found");
    syslog_add("[INFO] USB host controller initialized");
    syslog_add("[INFO] PS/2 mouse driver loaded");
    syslog_add("[INFO] File system mounted");
    syslog_add("[BOOT] Desktop environment ready");
    syslog_add("[INFO] Network: waiting for link...");
    syslog_add("[WARN] No RTC CMOS battery detected");
    syslog_add("[INFO] System idle - all services running");
}

static void draw_home(int w, int h, int slide)
{
    int px = 10, pw = HOME_PW;
    int py = h - 70 - HOME_PH + slide;
    int ph = HOME_PH;

    // ════════════════════════════════════════════
    //  PANEL CHROME
    // ════════════════════════════════════════════

    // Shadow
    for (int i = 0; i < 10; i++) {
        int a = (10 - i) * 2;
        gfx_rect(px + 5 - i/2, py + ph + i, pw + i, 1, (a<<16)|(a<<8)|a);
    }

    // Body
    gfx_fill_round_rect(px, py, pw, ph, 12, 0x080818);
    gfx_round_rect(px, py, pw, ph, 12, 0x283860);
    gfx_round_rect(px+1, py+1, pw-2, ph-2, 11, 0x182038);

    // ════════════════════════════════════════════
    //  HEADER
    // ════════════════════════════════════════════
    int hx = px + 16, hy = py + 12, hh = 38;

    // Logo square
    gfx_fill_round_rect(hx, hy, hh, hh, 10, 0x1A3068);
    gfx_round_rect(hx, hy, hh, hh, 10, 0x4488FF);
    // K inside
    gfx_rect(hx+12, hy+8, 3, 22, 0x88CCFF);
    { int i; for(i=0;i<10;i++) gfx_rect(hx+15+i,hy+18-i,2,2,0x88CCFF);
      for(i=0;i<10;i++) gfx_rect(hx+15+i,hy+20+i,2,2,0x88CCFF); }

    // Title
    gfx_print(hx + hh + 12, hy + 4, 0xFFFFFF, "KairoOS");
    gfx_print(hx + hh + 12, hy + 22, 0x3A5A8A, "Home");

    // Close button (top-right)
    int cbx = px + pw - 32, cby = py + 12;
    gfx_fill_circle_aa(cbx + 14, cby + 14, 13, 0x10101E);
    gfx_circle_aa(cbx + 14, cby + 14, 13, 1, 0x2A3A5A);
    draw_icon(cbx + 4, cby + 4, ICON_CLOSE, 0x667799, 20.0f);

    // Divider line under header
    gfx_rect(px + 12, py + 60, pw - 24, 1, 0x1A2838);

    // ════════════════════════════════════════════
    //  SECTION LABEL + SCROLL
    // ════════════════════════════════════════════
    int grid_y = py + 68;
    gfx_print(px + 16, grid_y, 0x3A5A8A, "APPLICATIONS");

    // Scroll arrows (right of label)
    int grid_rows = (HOME_APP_COUNT + HOME_COLS - 1) / HOME_COLS;
    if (grid_rows > HOME_VROWS) {
        draw_icon(px + 118, grid_y - 2, ICON_CHEVRON_UP, home_scroll > 0 ? 0x4488FF : 0x1A2030, 14.0f);
        draw_icon(px + 130, grid_y - 2, ICON_CHEVRON_DN, home_scroll + HOME_VROWS < grid_rows ? 0x4488FF : 0x1A2030, 14.0f);
    }

    // ════════════════════════════════════════════
    //  APP GRID — 5 columns, circular icons
    // ════════════════════════════════════════════
    int colw = (pw - 32) / HOME_COLS;       // ~101px per column
    int icon_r = 18;                          // icon circle radius
    int gx = px + 16;

    static const uint32_t app_bg[HOME_APP_COUNT] = {
        0x101820, 0x102048, 0x201040, 0x181820, 0x403808,
        0x082050, 0x381038, 0x083038, 0x102030, 0x083030,
        0x083820, 0x482008, 0x401010, 0x182848, 0x083028,
        0x301040, 0x383010, 0x182840, 0x100830, 0x301030,
        0x084018, 0x282830, 0x182038, 0x282830, 0x403010,
        0x083018, 0x082050, 0x084018, 0x083050, 0x083838,
        0x082018
    };
    static const uint32_t app_accent[HOME_APP_COUNT] = {
        0x44AAAA, 0x5588FF, 0x8866CC, 0x888888, 0xCCAA22,
        0x44AAFF, 0xBB44BB, 0x33BBBB, 0x5588AA, 0x33BBBB,
        0x44BB66, 0xDD8833, 0xDD4444, 0x6699CC, 0x33AA77,
        0xBB66EE, 0xBBAA44, 0x6688AA, 0x5533AA, 0xBB5588,
        0x55CC55, 0xAAAAAA, 0x7788AA, 0x888899, 0xCC9944,
        0x44AA66, 0x4488DD, 0x55CC55, 0x4499CC, 0x33BBBB,
        0x44DD88
    };
    // Material Icon codepoint per app (matching home_app_name order)
    static const int app_icon_cp[HOME_APP_COUNT] = {
        ICON_TERMINAL, ICON_SETTINGS, ICON_BRAIN,    ICON_CALCULATOR, ICON_EDIT,
        ICON_SHOP,     ICON_CAMERA,   ICON_COMPUTER, ICON_CAMERA,     ICON_PLAY,
        ICON_MOVIE,    ICON_CALENDAR, ICON_TIMER,    ICON_CLOUD,      ICON_BRIGHTNESS,
        ICON_PALETTE,  ICON_KEYBOARD, ICON_FOLDER,   ICON_GAME,       ICON_GAMES,
        ICON_GAME,     ICON_GAMES,    ICON_MIC,      ICON_GAME,       ICON_PALETTE,
        ICON_GAME,     ICON_MUSIC_NOTE,ICON_GAME,    ICON_PUBLIC,     ICON_CHAT,
        ICON_PHONE
    };

    for (int i = 0; i < HOME_APP_COUNT; i++) {
        int r = i / HOME_COLS, c = i % HOME_COLS;
        if (r < home_scroll || r >= home_scroll + HOME_VROWS) continue;
        int ix = gx + c * colw + colw / 2;
        int iy = grid_y + 18 + (r - home_scroll) * 72 + icon_r;

        // Selection glow
        if (home_focus == 0 && home_sel == i) {
            gfx_fill_circle_aa(ix, iy, icon_r + 5, 0x1A2848);
            gfx_circle_aa(ix, iy, icon_r + 5, 1, 0x3366AA);
        }

        // Icon circle (shadow + body + rim)
        gfx_fill_circle_aa(ix + 1, iy + 1, icon_r, 0x000000);
        gfx_fill_circle_aa(ix, iy, icon_r, app_bg[i]);
        gfx_circle_aa(ix, iy, icon_r, 1, app_accent[i]);

        // Material Icon inside circle
        {
            int gw = ttf_get_advance(&icon_font, app_icon_cp[i]);
            int gh = 24;
            draw_icon(ix - gw/2, iy - gh/2, app_icon_cp[i], app_accent[i], 24.0f);
        }

        // App name (centered below icon)
        int nl = 0; while (home_app_name[i][nl]) nl++;
        if (nl > 8) nl = 8;
        int name_x = gx + c * colw + (colw - nl * 8) / 2;
        int name_y = iy + icon_r + 5;
        gfx_print(name_x, name_y,
                  home_focus == 0 && home_sel == i ? 0xFFFFFF : 0x4A6A8A,
                  home_app_name[i]);
    }

    // ════════════════════════════════════════════
    //  BOTTOM SECTION — System row (horizontal)
    // ════════════════════════════════════════════
    int bot_y = py + ph - 56;
    gfx_rect(px + 12, bot_y, pw - 24, 1, 0x1A2838);
    gfx_print(px + 16, bot_y + 6, 0x2A4A6A, "SYSTEM");

    int sys_x = px + 16, sys_half = bootloader_unlocked ? (pw - 48) / 3 : (pw - 36) / 2;

    // OEM Unclocker (left)
    { int sy2 = bot_y + 20, sh = 30;
      int focused = (home_focus == 1 && home_sys_sel == 0);
      if (focused) {
          gfx_fill_round_rect(sys_x - 2, sy2 - 2, sys_half + 4, sh + 4, 6, 0x101828);
          gfx_round_rect(sys_x - 2, sy2 - 2, sys_half + 4, sh + 4, 6, 0x3366AA);
      }
      gfx_fill_round_rect(sys_x, sy2, sys_half, sh, 6, 0x0C1018);
      gfx_round_rect(sys_x, sy2, sys_half, sh, 6, 0x1A2A40);
      draw_icon(sys_x + 5, sy2 + 5, ICON_LOCK, 0x4488FF, 20.0f);
      gfx_print(sys_x + 30, sy2 + 8, 0xFFFFFF, "OEM");
      draw_icon(sys_x + sys_half - 16, sy2 + 5, ICON_ARROW_RIGHT, 0x2A4A6A, 20.0f); }

    // BootLoader CLI (middle, when unlocked)
    if (bootloader_unlocked) { int sx2 = sys_x + sys_half + 4, sy2 = bot_y + 20, sh = 30;
      int focused = (home_focus == 1 && home_sys_sel == 1);
      if (focused) {
          gfx_fill_round_rect(sx2 - 2, sy2 - 2, sys_half + 4, sh + 4, 6, 0x081810);
          gfx_round_rect(sx2 - 2, sy2 - 2, sys_half + 4, sh + 4, 6, 0x33AA66);
      }
      gfx_fill_round_rect(sx2, sy2, sys_half, sh, 6, 0x081010);
      gfx_round_rect(sx2, sy2, sys_half, sh, 6, 0x1A3A2A);
      draw_icon(sx2 + 5, sy2 + 5, ICON_TERMINAL, 0x44FF88, 20.0f);
      gfx_print(sx2 + 30, sy2 + 8, 0xFFFFFF, "CLI");
      draw_icon(sx2 + sys_half - 16, sy2 + 5, ICON_ARROW_RIGHT, 0x2A4A6A, 20.0f); }

    // Syslog Viewer (right, when unlocked)
    if (bootloader_unlocked) { int sx3 = sys_x + 2*(sys_half + 4), sy2 = bot_y + 20, sh = 30;
      int focused = (home_focus == 1 && home_sys_sel == 2);
      if (focused) {
          gfx_fill_round_rect(sx3 - 2, sy2 - 2, sys_half + 4, sh + 4, 6, 0x181028);
          gfx_round_rect(sx3 - 2, sy2 - 2, sys_half + 4, sh + 4, 6, 0x6644AA);
      }
      gfx_fill_round_rect(sx3, sy2, sys_half, sh, 6, 0x100C18);
      gfx_round_rect(sx3, sy2, sys_half, sh, 6, 0x2A1A40);
      draw_icon(sx3 + 5, sy2 + 5, ICON_TERMINAL, 0xBB88FF, 20.0f);
      gfx_print(sx3 + 30, sy2 + 8, 0xFFFFFF, "Log");
      draw_icon(sx3 + sys_half - 16, sy2 + 5, ICON_ARROW_RIGHT, 0x2A4A6A, 20.0f); }

    // ════════════════════════════════════════════
    //  FOOTER
    // ════════════════════════════════════════════
    gfx_print(px + 16, py + ph - 14, 0x1A2A40,
              "Arrows: nav  PgUp/PgDn: scroll  Enter: open  Esc: close");
}

static void open_home(void)
{
    int w = gfx_width(), h = gfx_height();
    home_active = 1;
    home_focus = 0; home_sel = 0; home_sys_sel = 0; home_scroll = 0;
    // Panel slides from y=h-70 (bottom) to y=h-70-HOME_PH (final)
    // Restore area: full width of panel + margin, from final top to bottom of screen
    int _rest_x = 0, _rest_y = h - 70 - HOME_PH;
    int _rest_w = HOME_PW + 16, _rest_h = HOME_PH + 70 + 10;
    volatile uint32_t *_fb = gfx_get_fb_addr();
    int _pitch = gfx_get_pitch() / 4;
    // Cubic ease-out: rises fast at first, decelerates into place
    for (int f = 24; f >= 0; f--) {
        // Restore wallpaper over the entire panel region before each frame
        for (int _y = _rest_y; _y < _rest_y + _rest_h && _y < h; _y++) {
            if (_y < 0 || _y >= WP_H) continue;
            for (int _x = _rest_x; _x < _rest_x + _rest_w && _x < w; _x++) {
                if (_x < 0 || _x >= WP_W) continue;
                _fb[_y * _pitch + _x] = wallpaper_data[_y * WP_W + _x];
            }
        }
        int slide = HOME_PH * f * f * f / (24 * 24 * 24);
        draw_home(w, h, slide);
        delay();
    }
    // Final frame: full restore then draw at final position
    for (int _y = _rest_y; _y < _rest_y + _rest_h && _y < h; _y++) {
        if (_y < 0 || _y >= WP_H) continue;
        for (int _x = _rest_x; _x < _rest_x + _rest_w && _x < w; _x++) {
            if (_x < 0 || _x >= WP_W) continue;
            _fb[_y * _pitch + _x] = wallpaper_data[_y * WP_W + _x];
        }
    }
    draw_home(w, h, 0);
}

static void draw_oem_bios(int w, int h)
{
    gfx_clear(0x0000AA);
    gfx_rect(0, 0, w, 1, 0xFFFFFF);
    gfx_rect(0, h - 14, w, 14, 0x000080);
    gfx_print_scaled(w / 2 - 160, 10, 0xFFFFFF, "KAIRO BLUE BIOS v1.0", 2);
    gfx_rect(w / 2 - 240, 36, 480, 1, 0xFFFFFF);
    gfx_print(40, 56, 0x8080FF, "Main    Security    Boot    OEM    Save & Exit");
    gfx_rect(40, 70, 460, 1, 0x8080FF);

    gfx_print(40, 98, 0xFFFF00, "[1]");
    gfx_print(80, 98, 0xFFFFFF, "Unlock OEM");
    gfx_print(40, 124, 0x8080FF, "Unlocks the KairoOS bootloader for advanced commands.");

    gfx_fill_round_rect(w / 2 - 120, h - 110, 240, 44, 8, 0x000080);
    gfx_round_rect(w / 2 - 120, h - 110, 240, 44, 8, 0x44AAFF);
    gfx_print_scaled(w / 2 - 66, h - 98, 0x00FF00, "[ UNLOCK OEM ]", 1);
    gfx_print(60, h - 84, 0x8080FF,
              bootloader_unlocked ? "Bootloader: UNLOCKED" : "Bootloader: LOCKED");
    gfx_print(60, h - 62, 0x8080FF, "Use [1] or click UNLOCK to enable the bootloader command line.");
    gfx_print(40, h - 32, 0x8080FF, "[Esc] back to Home");
}

static void oem_do_unlock(void)
{
    int w = gfx_width(), h = gfx_height();
    // Black screen for a few seconds (simulated unlock write)
    gfx_clear(0x000000);
    for (int i = 0; i < 16; i++) delay();
    // Warning banner for ~5 seconds
    gfx_clear(0x000000);
    gfx_rect(0, 0, w, 1, 0x44FF44);
    gfx_rect(0, h - 1, w, 1, 0x44FF44);
    gfx_print_scaled(w / 2 - 160, h / 2 - 70, 0x44FF44, "BOOTLOADER ABILITATO", 2);
    gfx_print(w / 2 - 300, h / 2, 0xCCCCCC,
              "Nel terminale ora avrai dei commandi specifici.");
    gfx_print(w / 2 - 300, h / 2 + 22, 0xCCCCCC,
              "Apri BootLoader Command Line e digita help per info.");
    gfx_print(w / 2 - 60, h / 2 + 60, 0x2A6A2A, "[ 5s ]");
    for (int i = 0; i < 50; i++) delay();
    // REAL unlock: the bootloader flag persists for the whole session
    bootloader_unlocked = 1;
    oem_active = 0;
    home_active = 0;
}

// ─── Kairo setup wizard helpers ───────────────────────────────────────────
static unsigned wz_blend(unsigned a, unsigned b, int num, int den) {
    int ar=(a>>16)&0xFF, ag=(a>>8)&0xFF, ab=a&0xFF;
    int br=(b>>16)&0xFF, bg=(b>>8)&0xFF, bb=b&0xFF;
    int r=(ar*num+br*(den-num))/den;
    int g=(ag*num+bg*(den-num))/den;
    int bl=(ab*num+bb*(den-num))/den;
    return ((unsigned)r<<16)|((unsigned)g<<8)|(unsigned)bl;
}

static unsigned wz_bg_color(int y, int h) {
    int t = y*255/(h>1?h-1:1);
    return wz_blend(0x0A0A28, 0x040412, t, 255);
}

static unsigned wz_panel_color(int y, int pany, int panh) {
    int yy = y - pany; if (yy < 0) yy = 0; if (yy >= panh) yy = panh-1;
    int t = yy*255/(panh>1?panh-1:1);
    return wz_blend(0x12123C, 0x0A0A26, t, 255);
}

static int wz_text_w(const char *s, int scale) {
    const baked_glyph_t *tab = (scale>=2) ? font_lg : font_reg;
    int factor = (scale>=2) ? (scale/2) : 1; if (factor<1) factor=1;
    int tw = 0;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        int adv = (scale>=2)?16:8;
        if (c>=FONT_CHARSET_START && c<=FONT_CHARSET_END) adv = tab[c-FONT_CHARSET_START].adv*factor;
        tw += adv;
    }
    return tw;
}

static void wz_fill_panel_area(int x, int y, int w, int h, int pany, int panh) {
    for (int yy = y; yy < y+h; yy++)
        gfx_rect(x, yy, w, 1, wz_panel_color(yy, pany, panh));
}

static void wz_centered(int cx, int y, int scale, unsigned col, const char *s) {
    gfx_print_scaled(cx - wz_text_w(s, scale)/2, y, col, s, scale);
}

// ─── KairoWeb (GUI browser) ───────────────────────────────────────────────
// Parser HTML minimale: estrae testo + titoli + link, decodifica entità.
// Il wrapping viene eseguito al render con le metriche reali del font Inter.

#define KW_MAX_SEGS 160
#define KW_SEG_MAX  600
#define KW_LINE_MAX 128
#define KW_MAX_LINES 400
#define KW_MAX_LINKS 96

typedef struct {
    char text[KW_SEG_MAX];
    int  n;
    int  style;            // 0=testo, 1=titolo, 2=link (underline+blu)
    char href[200];        // per i link
} kwseg_t;

static kwseg_t kw_segs[KW_MAX_SEGS];
static int     kw_seg_count = 0;
static int     kw_scroll = 0;
static char    kw_url[160] = {0};
static int     kw_busy = 0;
static char    kw_status[48] = {0};
static char    kw_prev[160] = {0};

// righe renderizzate (cache del wrap) + rect link
static char    kw_rline[KW_MAX_LINES][KW_LINE_MAX];
static int     kw_rstyle[KW_MAX_LINES];
static char    kw_rhref[KW_MAX_LINES][200];
static int     kw_rline_count = 0;

static int kw_lx0[KW_MAX_LINKS], kw_ly0[KW_MAX_LINKS];
static int kw_lx1[KW_MAX_LINKS], kw_ly1[KW_MAX_LINKS];
static char kw_lhref[KW_MAX_LINKS][200];
static int  kw_link_count = 0;

static int kw_rel_x0, kw_rel_y0, kw_rel_x1, kw_rel_y1;   // bottone reload

static int kw_len(const char *s) { int n=0; while (s && s[n]) n++; return n; }
static int kw_eq(const char *a, const char *b) {
    while (*a || *b) { if (*a != *b) return 0; a++; b++; } return 1; }
static void kw_lower(char *s) { for (char *p=s; *p; p++) if (*p>='A'&&*p<='Z') *p+=32; }

static void kw_seg_start(int style, const char *href)
{
    if (kw_seg_count >= KW_MAX_SEGS) return;
    kwseg_t *s = &kw_segs[kw_seg_count];
    s->n = 0; s->text[0] = 0; s->style = style;
    if (href) { int i; for (i=0; href[i] && i<199; i++) s->href[i]=href[i]; s->href[i]=0; }
    else s->href[0] = 0;
}

static void kw_seg_append(kwseg_t *s, char c)
{
    if (s->n < KW_SEG_MAX-1) { s->text[s->n++] = c; s->text[s->n] = 0; }
}

// decodifica entità HTML: ritorna 1 se ne ha consumata una
static int kw_entity(const char *s, int *consumed, char *out)
{
    struct { const char *name; int len; char ch; } table[] = {
        {"&amp;",5,'&'},{"&lt;",4,'<'},{"&gt;",4,'>'},
        {"&quot;",6,'"'},{"&apos;",6,'\''},{"&nbsp;",6,' '},
        {"&#39;",5,'\''},{"&#34;",5,'"'},
    };
    for (int i = 0; i < (int)(sizeof(table)/sizeof(table[0])); i++) {
        int ok = 1;
        for (int j = 0; j < table[i].len; j++) if (s[j] != table[i].name[j]) { ok = 0; break; }
        if (ok) { *consumed = table[i].len; *out = table[i].ch; return 1; }
    }
    // &#NNN;
    if (s[0]=='&' && s[1]=='#') {
        int v=0, i=2;
        while (s[i]>='0'&&s[i]<='9' && i<9) { v = v*10 + s[i]-'0'; i++; }
        if (s[i]==';' && v>=32 && v<=126) { *consumed = i+1; *out = (char)v; return 1; }
    }
    return 0;
}

// estrae testo dal body HTML in kw_segs[]
static void kw_parse(const char *body, int blen)
{
    kw_seg_count = 0;

    int in_skip = 0, in_pre = 0;
    char href[200]; href[0] = 0;

    kw_seg_start(0, 0);
    kwseg_t *cur = &kw_segs[kw_seg_count];

    for (int i = 0; i < blen; i++) {
        char c = body[i];

        if (in_skip) {
            // guarda per </script> </style> </noscript> </head>
            if (c == '<') {
                int j = i+1;
                if (j < blen && body[j] == '/') {
                    char tag[16]; int tn = 0; j++;
                    while (j < blen && body[j] != '>' && tn < 15) tag[tn++] = body[j++];
                    tag[tn] = 0; kw_lower(tag);
                    if (kw_eq(tag,"script")||kw_eq(tag,"style")||kw_eq(tag,"noscript")||kw_eq(tag,"head")) {
                        in_skip = 0;
                        if (kw_eq(tag,"head")) { i = j; continue; }
                    }
                }
            }
            if (in_skip) continue;
        }

        if (c == '<') {
            int j = i+1;
            int closing = 0;
            if (j < blen && body[j] == '/') { closing = 1; j++; }
            char tag[32]; int tn = 0;
            while (j < blen && body[j] != '>' && tn < 31) { tag[tn++] = body[j]; j++; }
            tag[tn] = 0; kw_lower(tag);

            int is_open = !closing;
            if (is_open && kw_eq(tag,"script"))    { in_skip=1; i=j; continue; }
            if (is_open && kw_eq(tag,"style"))     { in_skip=1; i=j; continue; }
            if (is_open && kw_eq(tag,"noscript"))  { in_skip=1; i=j; continue; }
            if (is_open && kw_eq(tag,"head"))      { in_skip=1; i=j; continue; }
            if (is_open && kw_eq(tag,"pre"))       { in_pre=1; i=j; continue; }
            if (is_open && kw_eq(tag,"br"))        { if (cur->n) cur->text[cur->n++] = '\n'; i=j; continue; }

            int inline_tags = kw_eq(tag,"b")||kw_eq(tag,"i")||kw_eq(tag,"em")||kw_eq(tag,"strong")||
                              kw_eq(tag,"span")||kw_eq(tag,"small")||kw_eq(tag,"u")||kw_eq(tag,"font")||
                              kw_eq(tag,"code")||kw_eq(tag,"mark")||kw_eq(tag,"abbr")||kw_eq(tag,"sup")||
                              kw_eq(tag,"sub")||kw_eq(tag,"time")||kw_eq(tag,"label")||kw_eq(tag,"a");
            int block_tags = kw_eq(tag,"p")||kw_eq(tag,"div")||kw_eq(tag,"h1")||kw_eq(tag,"h2")||
                             kw_eq(tag,"h3")||kw_eq(tag,"h4")||kw_eq(tag,"h5")||kw_eq(tag,"h6")||
                             kw_eq(tag,"li")||kw_eq(tag,"ul")||kw_eq(tag,"ol")||kw_eq(tag,"table")||
                             kw_eq(tag,"tr")||kw_eq(tag,"td")||kw_eq(tag,"th")||kw_eq(tag,"section")||
                             kw_eq(tag,"article")||kw_eq(tag,"header")||kw_eq(tag,"footer")||
                             kw_eq(tag,"blockquote")||kw_eq(tag,"hr")||kw_eq(tag,"center")||
                             kw_eq(tag,"aside")||kw_eq(tag,"main")||kw_eq(tag,"nav")||kw_eq(tag,"form");

            (void)inline_tags;

            if (!inline_tags && !block_tags) { i = j; continue; }   // tag sconosciuto: salta

            if (block_tags && !kw_eq(tag,"li") && !kw_eq(tag,"td") && !kw_eq(tag,"th")) {
                // fine paragrafo → nuovo segmento
                if (cur->n > 0 || kw_seg_count == 0) {
                    if (cur->n > 0) kw_seg_count++;
                    if (kw_seg_count >= KW_MAX_SEGS) { i=j; continue; }
                    int st = 0;
                    if (tag[0]=='h') st = 1;
                    kw_seg_start(st, 0);
                    cur = &kw_segs[kw_seg_count];
                }
                if (kw_eq(tag,"hr")) { kw_seg_append(cur,'\n'); kw_seg_append(cur,'-'); kw_seg_append(cur,'\n'); }
                i = j; continue;
            }

            if (closing) {
                // chiusura blocco → chiudi segmento
                if (cur->n > 0) kw_seg_count++;
                if (kw_seg_count < KW_MAX_SEGS) { kw_seg_start(0,0); cur = &kw_segs[kw_seg_count]; }
                i = j; continue;
            }

            // <a href=...> → nuovo segmento link
            if (kw_eq(tag,"a") && is_open) {
                // estrai href
                int hq = j;
                for (; hq < blen && body[hq]!='>'; hq++) {
                    if ((body[hq]=='h'||body[hq]=='H') && (body[hq+1]=='r'||body[hq+1]=='R') &&
                        (body[hq+2]=='e'||body[hq+2]=='E') && (body[hq+3]=='f'||body[hq+3]=='F')) {
                        int qq = hq+4;
                        while (qq < blen && (body[qq]==' '||body[qq]=='\t'||body[qq]=='=')) qq++;
                        if (qq < blen && (body[qq]=='"'||body[qq]=='\'')) qq++;
                        int hs=0;
                        while (qq < blen && hs<199 && body[qq]!='"' && body[qq]!='\'' && body[qq]!='>')
                            href[hs++] = body[qq++];
                        href[hs] = 0;
                        break;
                    }
                }
                if (cur->n > 0) kw_seg_count++;
                if (kw_seg_count < KW_MAX_SEGS) { kw_seg_start(2, href); cur = &kw_segs[kw_seg_count]; }
                i = j; continue;
            }
            i = j; continue;
        }

        // testo
        if (c == '&') {
            int consumed = 0; char e = 0;
            if (kw_entity(&body[i], &consumed, &e)) {
                kw_seg_append(cur, e);
                if (e == ' ' && cur->n >= 2 && cur->text[cur->n-2] == ' ') cur->n--;  // niente doppi spazi
                cur->text[cur->n] = 0;
                i += consumed - 1;
                continue;
            }
        }

        if (c == '\r') continue;
        if (c == '\n' && !in_pre) continue;

        if (c == '\t') c = ' ';
        kw_seg_append(cur, c);
        if (cur->n > 1 && cur->text[cur->n-1]==' ' && cur->text[cur->n-2]==' ')
            cur->n--;                                   // collassa gli spazi
        cur->text[cur->n] = 0;
    }
    if (cur->n > 0 && kw_seg_count < KW_MAX_SEGS) kw_seg_count++;
    else if (kw_seg_count >= KW_MAX_SEGS && cur->n > 0) { /* ok */ }
    if (kw_seg_count == 0) kw_seg_count = 0;
}

// wrapping in righe renderizzabili (con metriche Inter), larghezza maxw
static void kw_wrap(int maxw)
{
    kw_rline_count = 0;
    for (int s = 0; s < kw_seg_count && kw_rline_count < KW_MAX_LINES; s++) {
        kwseg_t *seg = &kw_segs[s];
        const char *t = seg->text;
        int tn = seg->n;
        if (tn == 0) continue;
        int style = seg->style;
        if (style == 1) style = 1;                 // titolo

        // separa per \n
        char par[KW_SEG_MAX]; int pn = 0;
        for (int i = 0; i <= tn; i++) {
            char c = (i < tn) ? t[i] : '\n';
            if (c != '\n' && pn < KW_SEG_MAX-1) { par[pn++] = c; continue; }
            par[pn] = 0;

            // wrap della singola parola/riga
            int li = 0;
            char line[KW_LINE_MAX]; line[0] = 0;
            int lw = 0;
            char word[64]; int wn = 0;
            for (int k = 0; k <= pn; k++) {
                char ch = (k < pn) ? par[k] : ' ';
                if (ch == ' ' || k == pn) {
                    if (wn > 0) {
                        word[wn] = 0;
                        int ww = renderer_text_width(word, 1);
                        int sepw = line[0] ? renderer_text_width(" ",1) : 0;
                        if (line[0] && lw + sepw + ww > maxw) {
                            if (kw_rline_count < KW_MAX_LINES) {
                                int m=0; for(;line[m];m++) kw_rline[kw_rline_count][m]=line[m];
                                kw_rline[kw_rline_count][m]=0;
                                kw_rstyle[kw_rline_count]=style;
                                int hq; for (hq=0;seg->href[hq]&&hq<199;hq++) kw_rhref[kw_rline_count][hq]=seg->href[hq];
                                kw_rhref[kw_rline_count][hq]=0;
                                kw_rline_count++;
                            }
                            line[0]=0; lw=0;
                        }
                        int l = kw_len(line);
                        if (line[0]) line[l++]=' ';
                        for (int wq=0; wq<wn; wq++) line[l++]=word[wq];
                        line[l]=0;
                        lw = renderer_text_width(line, 1);
                        wn = 0;
                    }
                } else if (ch == '\t') {
                    if (wn < 63) word[wn++] = ' ';
                } else {
                    if (wn < 63) word[wn++] = ch;
                }
            }
            if (kw_rline_count < KW_MAX_LINES) {
                int m=0; for(;line[m];m++) kw_rline[kw_rline_count][m]=line[m];
                kw_rline[kw_rline_count][m]=0;
                kw_rstyle[kw_rline_count]=style;
                int hq; for (hq=0;seg->href[hq]&&hq<199;hq++) kw_rhref[kw_rline_count][hq]=seg->href[hq];
                kw_rhref[kw_rline_count][hq]=0;
                kw_rline_count++;
            }
            pn = 0;
        }
    }
}

static int kw_find_body(const char *body, int len)
{
    for (int i = 0; i < len-3; i++)
        if (body[i]=='\r'&&body[i+1]=='\n'&&body[i+2]=='\r'&&body[i+3]=='\n') return i+4;
    for (int i = 0; i < len-1; i++)
        if (body[i]=='\n'&&body[i+1]=='\n') return i+2;
    return 0;
}

// carica un URL via HTTP in-kernel (bloccante), poi prepara i segmenti
static int kw_load(const char *url)
{
    extern int http_get(const char *url);
    extern int http_get_status(void);
    extern const char *http_get_body(void);
    extern int http_get_body_len(void);

    kw_busy = 1;
    int r = http_get(url);
    if (r == 0) {
        const char *body = http_get_body();
        int blen = http_get_body_len();
        int bs = kw_find_body(body, blen);
        kw_parse(body + bs, blen - bs);
        kw_scroll = 0;
        kw_status[0] = 0;
    } else {
        kw_seg_count = 0;
        kw_seg_start(0, 0);
        kwseg_t *c = &kw_segs[0];
        const char *m = "[ Errore rete ] Non riesco a contattare il server.";
        for (int i = 0; m[i] && c->n < KW_SEG_MAX-1; i++) kw_seg_append(c, m[i]);
        kw_seg_count = 1;
        kw_scroll = 0;
        kw_status[0] = 0;
    }
    kw_busy = 0;
    return r;
}

// disegna la GUI del browser nella finestra
static void kw_render(int wx, int wy, int ww, int wh, const char *urlbar, int addr_focus)
{
    const int cxl = wx + 14;                      // colonna testo
    const int tw = ww - 46;                       // larghezza testo
    if (tw < 40) return;

    int content_top = wy + 44;
    int content_bot = wy + wh - 26;
    int content_h = content_bot - content_top;
    if (content_h < 30) content_h = 30;

    // sfondo pagina
    gfx_fill_round_rect(wx+8, content_top, ww-16, content_h, 6, 0xF4F7FB);
    gfx_round_rect(wx+8, content_top, ww-16, content_h, 6, 0xBFCCDE);

    // toolbar
    gfx_fill_round_rect(wx+8, wy+38, ww-16, 26, 6, 0xE7EBF3);
    gfx_round_rect(wx+8, wy+38, ww-16, 26, 6, 0xB4C2D6);

    // bottone back/freccia
    gfx_fill_round_rect(wx+14, wy+42, 18, 18, 5, 0x2A6CD7);
    gfx_print(wx+18, wy+45, 0xFFFFFF, "‹");

    // bottone reload
    int rlx = wx+ww-32, rly = wy+42;
    gfx_fill_round_rect(rlx, rly, 18, 18, 5, 0x7E8DA6);
    gfx_print(rlx+6, rly+3, 0xFFFFFF, "↻");
    kw_rel_x0 = rlx-2; kw_rel_y0 = rly-2; kw_rel_x1 = rlx+20; kw_rel_y1 = rly+20;

    // casella URL
    int box_x = wx+38, box_w = ww-46-26;
    gfx_fill_round_rect(box_x, wy+41, box_w, 20, 5, 0xFFFFFF);
    gfx_round_rect(box_x, wy+41, box_w, 20, 5, addr_focus ? 0x2A6CD7 : 0x9AA7BC);
    if (urlbar[0]) {
        gfx_print(box_x+8, wy+44, addr_focus ? 0x16283A : 0x223344, urlbar);
        if (addr_focus) {
            // cursore lampeggiante dopo il testo
            int cw = renderer_text_width(urlbar, 1);
            gfx_rect(box_x+10+cw, wy+45, 1, 12, 0x2A6CD7);
        }
    } else {
        gfx_print(box_x+8, wy+44, 0x9AA7BC, addr_focus ? "" : "Digita un URL e premi Invio");
    }
    if (kw_busy) gfx_print(box_x+box_w-70, wy+44, 0x2A6CD7, "Loading...");

    // status/footer
    if (kw_status[0]) gfx_print(wx+16, content_bot, 0x6A7A90, kw_status);
    else if (kw_url[0]) gfx_print(wx+16, content_bot, 0x6A7A90, kw_url);

    // wrap con larghezza effettiva
    kw_wrap(tw);

    // righe visibili
    int line_h = FONT_LINE_H_REG;
    int max_lines = content_h / line_h - 1;
    if (max_lines < 1) max_lines = 1;

    int start = kw_scroll;
    if (start > kw_rline_count - max_lines) start = kw_rline_count - max_lines;
    if (start < 0) start = 0;

    kw_link_count = 0;
    int y = content_top + 10;
    for (int li = start; li < kw_rline_count && li < start + max_lines; li++) {
        char *txt = kw_rline[li];
        int style = kw_rstyle[li];
        unsigned int col = 0x223344;
        if (style == 1) { col = 0x1A3A68; line_h = FONT_LINE_H_REG + 4; }
        else if (style == 2) { col = 0x2356B8; line_h = FONT_LINE_H_REG; }
        else line_h = FONT_LINE_H_REG;

        if (style == 2) {
            int w = renderer_text_width(txt, 1);
            if (kw_link_count < KW_MAX_LINKS) {
                kw_lx0[kw_link_count] = cxl; kw_ly0[kw_link_count] = y-2;
                kw_lx1[kw_link_count] = cxl + w; kw_ly1[kw_link_count] = y + line_h + 2;
                int hq;
                for (hq=0; kw_rhref[li][hq] && hq<199; hq++) kw_lhref[kw_link_count][hq] = kw_rhref[li][hq];
                kw_lhref[kw_link_count][hq] = 0;
                kw_link_count++;
            }
        }

        gfx_print(cxl, y, col, txt);
        if (style == 2) gfx_rect(cxl, y + line_h - 1, renderer_text_width(txt,1), 1, col);
        y += line_h;
    }

    // keep scroll bounded post-draw
    if (kw_rline_count > max_lines) {
        int sb_h = content_h - 14;
        int thumb = sb_h * max_lines / kw_rline_count; if (thumb < 12) thumb = 12;
        int th_y = content_top + 7 + (sb_h-thumb)*start/(kw_rline_count-max_lines);
        gfx_fill_round_rect(wx+ww-14, content_top+7, 5, sb_h, 2, 0xD5DDE9);
        gfx_fill_round_rect(wx+ww-14, th_y, 5, thumb, 2, 0x7E8DA6);
    }
}
void kernel_main(void) {

    kernel_early();

    __asm__ volatile("movl 0x70000, %0" : "=r"(g_fb_addr) : : "memory");
    if (!g_fb_addr) g_fb_addr = 0xFD000000;

    int vbe_w = 1024, vbe_h = 768;
    __asm__ volatile("movzwl 0x70004, %0" : "=r"(vbe_w) : : "memory");
    __asm__ volatile("movzwl 0x70008, %0" : "=r"(vbe_h) : : "memory");
    if (vbe_w < 640 || vbe_h < 480) { vbe_w = 1024; vbe_h = 768; }

    kernel_memory(g_fb_addr);

    __asm__ volatile("sti");

    fb_info_t fb;
    fb.framebuffer = (volatile uint32_t*)(uintptr_t)g_fb_addr;
    fb.width = vbe_w;
    fb.height = vbe_h;
    fb.pitch = vbe_w * 4;
    fb.bpp = 32;
    gfx_init(&fb);

    // Finish all init before boot screen
    kernel_drivers();
    kernel_tasks();

    play_startup_melody();

    // Init TTF icon font
    icon_font_init();
    icon_cache_init();

    // Boot screen animation
    boot_screen(vbe_w, vbe_h);

    // Init serial port for social proxy
    serial_init();

    // Mount initial RAM filesystem
    initrd_load();

    // Mount disk filesystem if AHCI disk available
    if (ahci_disk_port >= 0) {
        if (kfs_mount(ahci_disk_port) < 0) {
            fb_write("Nessun FS su disco, formattazione...\n");
            if (kfs_format(ahci_disk_port) == 0)
                kfs_mount(ahci_disk_port);
        }
    }

    // Launch user-mode init from the initrd (real ring3 program).
    // kernel_setjmp saves the kernel context; when /init calls exit,
    // proc_exit switches back to kernel page tables and longjmps here,
    // so control resumes with the desktop GUI below.
    fb_init();

    // KairoWeb HTTP smoke test
    {
        extern int http_get(const char *url);
        extern int http_get_status(void);
        if (http_get("http://example.com/") == 0) {
            fb_write("KAIROWEB HTTP OK STATUS ");
            int st = http_get_status();
            fb_write((char[]){(char)('0'+st/100), (char)('0'+(st/10)%10), (char)('0'+st%10), 0});
            fb_write("\n");
        } else {
            fb_write("KAIROWEB HTTP FAIL\n");
        }
    }

    fb_write("Kairo-OS: launching /init in ring3...\n");
    if (kernel_setjmp(kctx) == 0) {
        elf_load_and_exec("/init");
        fb_write("init returned (error)\n");
        while (1) { __asm__ volatile("hlt"); }
    }
    __asm__ volatile("sti");

    int w = vbe_w, h = vbe_h, win = 0, win_type = 0;
    int search_focus = 1, search_pos = 0, search_sel = 0;
    char search_buf[64] = {0};
    char term_buf[128] = {0}; int term_pos = 0, term_line_count = 0, term_scroll = 0;
    char term_lines[60][80]; for (int _t=0;_t<60;_t++) term_lines[_t][0]=0;
    char chat_buf[128] = {0}; int chat_pos = 0, chat_line_count = 0, chat_scroll = 0;
    char chat_lines[60][80]; for (int _c=0;_c<60;_c++) chat_lines[_c][0]=0;
    // Speech-to-text (Viteza STT — motore custom MFCC+DTW su host)
    int stt_active = 0, stt_train = 0, stt_ticks = 0;
    char br_url[128] = {0}; int br_pos = 0, br_line_count = 0, br_scroll = 0, br_fetching = 0, br_focus = 0;
    char br_lines[60][80]; for (int _b=0;_b<60;_b++) br_lines[_b][0]=0;
    char notes[10][80]; int note_count = 0, note_sel = 0;
    char note_buf[80] = {0}; int note_pos = 0;
    int set_state = 0, set_cat = 0;
    int vol_level = 31, vol_mute = 0;
    // Notification toasts
    char notify_buf[4][80]; int notify_count = 0, notify_tick = 0;
    char account_name[40] = {0};
    int usb_popup = 0;
    int apps_installed[27] = {1,1,1,1,1,0,0,0,1,0,0,0,1,0,1,0,1,1,0,0,0,0,0,0,0,0,1};
    int studio_panel = 0, studio_file = 0, studio_output_count = 0;
    const char *studio_fnames[3] = {"index.xml","index.js","style.css"};
    char code_buf[3][30][60]; int code_lines[3], code_cx[3], code_cy[3], code_scroll[3];
    char studio_output[10][60];
    int vm_count = 0, vm_mode = 0, vm_sel = 0;
    char vm_name[4][32]; int vm_os[4], vm_ram[4], vm_cores[4], vm_disk[4], vm_running[4], vm_cstate[4];
    int vm_creat_step = 0, vm_creat_os = 0, vm_creat_pos = 0, vm_creat_ram = 2048, vm_creat_cores = 2, vm_creat_disk = 32;
char vm_creat_name[32] = {0};
// Calculator
int calc_val = 0, calc_cur = 0, calc_op = 0, calc_state = 0;
// Calendar
int cal_month = 6, cal_year = 2026;
// Pomodoro/Timer
int pom_sec = 0, pom_total = 1500, pom_running = 0, pom_ticks = 0;
// Weather mock
int wthr_sel = 0, wthr_first = 1;
// ASCII Art Gallery
int art_sel = 0; const int art_count = 6;
// Typing Test
const char *type_text = "The quick brown fox jumps over the lazy dog. Pack my box with five dozen liquor jugs.";
int type_pos = 0, type_err = 0, type_ok = 0, type_start = 0, type_done = 0;
// Clipboard
char clip_buf[10][80]; int clip_count = 0, clip_sel = 0;
char clip_save[80]; int clip_save_pos = 0;
// File Manager mock
int fm_sel = 0, fm_scroll = 0, fm_open = 0;
#define FM_COUNT 16
const char *fm_files[FM_COUNT] = {"Documents/","Pictures/","Music/","Videos/","Projects/","System/","README.txt","config.ini","notes.txt","kernel.log","wallpaper.bmp","boot.cfg","themes/","icons/","sdk.zip","changelog.txt"};
int fm_is_dir[FM_COUNT] = {1,1,1,1,1,1,0,0,0,0,0,0,1,1,0,0};
const char *fm_sizes[FM_COUNT] = {"--","--","--","--","--","--","2.4 KB","1.1 KB","0.8 KB","4.0 KB","892 KB","0.5 KB","--","--","3.2 MB","1.2 KB"};
// Command Palette
int cmd_active = 0, cmd_pos = 0; char cmd_buf[64] = {0};
// Control Center
int cc_active = 0, cc_sel = 0;
// Settings panel (system overlay, not a windowed app)
int settings_active = 0;
// Notification Center
int nc_active = 0, nc_sel = 0;
// Wi-Fi panel (scan reale via proxy)
int wifi_active = 0, wifi_sel = 0;
// AI Command Mode
int ai_active = 0, ai_pos = 0, ai_response = 0;
char ai_buf[128] = {0};
// Screensaver
int idle_ticks = 0, ss_active = 0, ss_frame = 0;
// Viteza Wii
int _wii_boot = 0;
// Christmas easter egg
int _natal_mode = 0;
int _natal_snow[64][2];
int _santa_x = -100, _santa_y = 80, _santa_f = 0;
// Menu bar icon positions
int cc_icon_x = 0, nc_icon_x = 0, wifi_icon_x = 0;
const char *_app_name = "Finder";
// Launchpad
int lp_sel_x = 0, lp_sel_y = 0;
// Tetris
int tet_grid[10][18]; int tet_piece = 0, tet_rot = 0, tet_x = 3, tet_y = 0, tet_next = 0, tet_score = 0, tet_lines = 0, tet_drop = 0, tet_gameover = 0;

// Snake
int snake_body[400][2]; int snake_len = 3, snake_dir = 0, snake_food_x = 8, snake_food_y = 8, snake_score = 0, snake_gameover = 0, snake_drop = 0;
// Pong
int pong_py1 = 0, pong_py2 = 0, pong_bx = 0, pong_by = 0, pong_bdx = 2, pong_bdy = 1, pong_s1 = 0, pong_s2 = 0;
// Dock magnification springs (10 icons, scale 1000=normal, 1500=max)
int dock_sz[10] = {1000,1000,1000,1000,1000,1000,1000,1000,1000,1000};
int dock_sv[10] = {0,0,0,0,0,0,0,0,0,0};
// Home button spring (1000=visible at final pos, 0=hidden below dock)
int home_btn_sz = 0, home_btn_sv = 0, home_btn_prev_y = -1;
// Paint
#define _PW 80
#define _PH 60
uint32_t _paint_cv[_PW * _PH];
int _paint_col = 0xFF4444, _paint_lastx = -1, _paint_lasty = -1, _paint_size = 2;
// Maze
#define _MW 16
#define _MH 12
int _maze_map[_MW*_MH], _maze_px=1, _maze_py=1, _maze_ex=14, _maze_ey=10, _maze_win=0;
// Dino
int _dino_y=0,_dino_v=0,_dino_obs=0,_dino_ox=0,_dino_score=0,_dino_dead=0,_dino_t=0;
const char *vm_os_name[3] = {"macOS 15 Sequoia","Windows 11 Pro","Ubuntu 24.04 LTS"};
    uint32_t vm_os_color[3] = {0x8888CC,0x4488FF,0xDD8844};
    gfx_print(80, 336, DIM, "TASKS");
    animate_bar(200, 336, 300, 100);
    gfx_print(510, 336, TEXT, "OK");

    gfx_rect(80, 360, 520, 1, 0x1A1A4E);

    gfx_fill_round_rect(w/2-86, 380, 172, 22, 6, 0x1A1A4E);
    gfx_round_rect(w/2-86, 380, 172, 22, 6, PROG_FG);
    gfx_print_scaled(w/2-56, 383, PROG_FG, "SYSTEM READY", 1);

    delay();

    // Initialize USB subsystem
    int usb_devs = usb_init();
    if (usb_devs > 0) {
        gfx_print(80, 420, 0x00CC44, "USB: ");
        gfx_print(125, 420, DIM, usb_device_name(0));
    } else {
        gfx_print(80, 420, 0xCC4400, "USB: none");
    }

    // Initialize Camera subsystem (real USB UVC detection)
    camera_init();
    if (camera_is_present()) {
        gfx_print(80, 440, 0x00CC44, "Camera: ");
        gfx_print(148, 440, DIM, camera_get_name());
    } else {
        gfx_print(80, 440, 0xCC4400, "Camera: none");
    }

    // Initialize Bluetooth subsystem (real USB detection)
    bt_init();
    if (bt_is_present()) {
        gfx_print(80, 460, 0x00CC44, "BT:    ");
        gfx_print(148, 460, DIM, bt_get_name());
    } else {
        gfx_print(80, 460, 0xCC4400, "BT:    none");
    }

    // ─── First-boot Setup Wizard (Kairo, new design) ───
    #define draw_toggle(tx,ty,ton) do {\
        if(ton){gfx_fill_round_rect(tx,ty,40,20,10,0x006644);gfx_fill_round_rect(tx+20,ty+3,14,14,7,0x00FF88);}\
        else{gfx_fill_round_rect(tx,ty,40,20,10,0x2A2A2A);gfx_fill_round_rect(tx+6,ty+3,14,14,7,0x6A6A6A);}\
    } while(0)

    typedef struct {
        const char *name;
        const char *t_choose, *t_choose_sub;
        const char *step_lang, *step_kb, *step_tests, *step_ready;
        const char *t_kb, *t_kb_sub;
        const char *t_tests, *t_tests_sub;
        const char *test_kb, *test_mic, *test_scr;
        const char *kb_inst, *kb_pass;
        const char *mic_inst, *mic_click, *mic_rec, *mic_said, *mic_ok, *mic_noaudio;
        const char *scr_ask, *scr_ok;
        const char *col[7];
        const char *ready_title, *ready_sub, *ready_enter;
        const char *hint_nav, *hint_esc, *hint_back;
        const char *pass, *todo, *continue_;
    } wz_lang_t;
    static const wz_lang_t wz_lang[6] = {
        {"English",
         "Choose your language", "Select how you want to use Kairo.",
         "Language","Keyboard","Tests","Ready",
         "Keyboard layout", "Select your keyboard type.",
         "Hardware tests", "Verify that everything works.",
         "Keyboard Test","Microphone Test","Screen Test",
         "Type below - if the text appears, the keyboard works.", "Keyboard works!",
         "Press the mic and speak a few words.", "Click the mic button",
         "Recording...", "You said:", "Microphone works!", "No audio detected.",
         "What color is this?", "All colors verified!",
         {"Red","Orange","Yellow","Green","Blue","Purple","White"},
         "You're all set!", "Kairo is ready to go.", "Press [Enter] to start",
         "[Up/Down] select    [Enter] next", "[Esc] skip setup", "[Esc] back",
         "PASS","not tested","Continue"},
        {"Italiano",
         "Scegli la tua lingua", "Scegli come vuoi usare Kairo.",
         "Lingua","Tastiera","Test","Pronto",
         "Layout tastiera", "Scegli il tuo tipo di tastiera.",
         "Test hardware", "Verifica che tutto funzioni.",
         "Test tastiera","Test microfono","Test schermo",
         "Digita qui sotto: se il testo compare, la tastiera funziona.", "Tastiera funzionante!",
         "Premi il microfono e parla qualche parola.", "Premi il pulsante del microfono",
         "Registrazione...", "Hai detto:", "Microfono funzionante!", "Nessun audio rilevato.",
         "Di che colore e' questo?", "Tutti i colori verificati!",
         {"Rosso","Arancio","Giallo","Verde","Blu","Viola","Bianco"},
         "Tutto pronto!", "Kairo e' pronto all'uso.", "Premi [Invio] per iniziare",
         "[Su/Giu] scegli    [Invio] avanti", "[Esc] salta la configurazione", "[Esc] indietro",
         "OK","non testato","Continua"},
        {"Francais",
         "Choisissez votre langue", "Choisissez votre facon d'utiliser Kairo.",
         "Langue","Clavier","Tests","Pret",
         "Disposition du clavier", "Choisissez votre type de clavier.",
         "Tests materiel", "Verifiez que tout fonctionne.",
         "Test clavier","Test micro","Test ecran",
         "Tapez ci-dessous: si le texte apparait, le clavier marche.", "Clavier fonctionne!",
         "Cliquez sur le micro et parlez quelques mots.", "Cliquez sur le bouton micro",
         "Enregistrement...", "Vous avez dit:", "Micro fonctionne!", "Aucun audio detecte.",
         "De quelle couleur est-ce?", "Toutes les couleurs verifiees!",
         {"Rouge","Orange","Jaune","Vert","Bleu","Violet","Blanc"},
         "Tout est pret!", "Kairo est pret.", "Appuyez sur [Entree] pour demarrer",
         "[Haut/Bas] choisir    [Entree] suivant", "[Echap] ignorer", "[Echap] retour",
         "OK","non teste","Continuer"},
        {"Deutsch",
         "Waehle deine Sprache", "Waehle, wie du Kairo nutzen moechtest.",
         "Sprache","Tastatur","Tests","Fertig",
         "Tastaturlayout", "Waehle deinen Tastaturtyp.",
         "Hardware-Tests", "Pruefe, ob alles funktioniert.",
         "Tastaturtest","Mikrofontest","Bildschirmtest",
         "Tippe unten: wenn der Text erscheint, funktioniert die Tastatur.", "Tastatur funktioniert!",
         "Klicke das Mikrofon und sprich ein paar Woerter.", "Klicke den Mikrofon-Button",
         "Aufnahme...", "Du sagtest:", "Mikrofon funktioniert!", "Kein Audio erkannt.",
         "Welche Farbe ist das?", "Alle Farben geprueft!",
         {"Rot","Orange","Gelb","Gruen","Blau","Lila","Weiss"},
         "Alles bereit!", "Kairo ist einsatzbereit.", "Druecke [Enter] zum Starten",
         "[Auf/Ab] waehlen    [Enter] weiter", "[Esc] ueberspringen", "[Esc] zurueck",
         "OK","nicht getestet","Weiter"},
        {"Espanol",
         "Elige tu idioma", "Elige como quieres usar Kairo.",
         "Idioma","Teclado","Pruebas","Listo",
         "Disposicion del teclado", "Elige tu tipo de teclado.",
         "Pruebas de hardware", "Comprueba que todo funciona.",
         "Prueba de teclado","Prueba de microfono","Prueba de pantalla",
         "Escribe abajo: si el texto aparece, el teclado funciona.", "Teclado funciona!",
         "Haz clic en el micro y di algunas palabras.", "Haz clic en el boton del micro",
         "Grabando...", "Dijiste:", "Microfono funciona!", "No se detecto audio.",
         "De que color es esto?", "Todos los colores verificados!",
         {"Rojo","Naranja","Amarillo","Verde","Azul","Morado","Blanco"},
         "Todo listo!", "Kairo esta listo.", "Pulsa [Intro] para empezar",
         "[Arriba/Abajo] elegir    [Intro] siguiente", "[Esc] omitir", "[Esc] atras",
         "OK","sin probar","Continuar"},
        {"Portugues",
         "Escolha o seu idioma", "Escolha como quer usar o Kairo.",
         "Idioma","Teclado","Testes","Pronto",
         "Disposicao do teclado", "Escolha o seu tipo de teclado.",
         "Testes de hardware", "Verifique se tudo funciona.",
         "Teste de teclado","Teste de microfone","Teste de tela",
         "Digite abaixo: se o texto aparecer, o teclado funciona.", "Teclado funciona!",
         "Clique no micro e fale algumas palavras.", "Clique no botao do micro",
         "Gravando...", "Voce disse:", "Microfone funciona!", "Nenhum audio detectado.",
         "De que cor e' isso?", "Todas as cores verificadas!",
         {"Vermelho","Laranja","Amarelo","Verde","Azul","Roxo","Branco"},
         "Tudo pronto!", "O Kairo esta pronto.", "Pressione [Enter] para comecar",
         "[Cima/Baixo] escolher    [Enter] proximo", "[Esc] pular", "[Esc] voltar",
         "OK","nao testado","Continuar"},
    };
    static const char *wz_layouts[5] = {"QWERTY  (US)","QWERTY  (UK)","QWERTY  (IT)","AZERTY  (FR)","QWERTZ  (DE)"};
    static const unsigned wz_scr_cols[7] = {0xFF5555,0xFF9944,0xFFCC44,0x44CC77,0x4499FF,0xBB66DD,0xEDEDED};
    static const int wz_scr_perm[7] = {2,5,0,3,6,1,4};

    const int wz_m = 40, wz_top = 80, wz_bot = 40;
    const int wz_lx = 40, wz_lw = 220;
    const int wz_rx = wz_lx + wz_lw + 36;
    const int wz_rw = w - wz_rx - wz_m;
    const int wz_hh = h - wz_top - wz_bot;
    const int wz_icx = wz_lx + wz_lw/2, wz_icy = wz_top + 148;
    const int wz_cx = wz_rx + 44, wz_cy = wz_top + 36;
    const int wz_cw = wz_rw - 88, wz_ch = wz_hh - 80;

    // static background: gradient + panels + icon + brand
    for (int _yy = 0; _yy < h; _yy++) gfx_rect(0, _yy, w, 1, wz_bg_color(_yy, h));
    gfx_fill_round_rect_aa(wz_lx, wz_top, wz_lw, wz_hh, 18, 0x0F0F30);
    gfx_fill_round_rect_aa(wz_rx, wz_top, wz_rw, wz_hh, 18, 0x0F0F30);
    for (int _yy = wz_top; _yy < wz_top+wz_hh; _yy++) {
        unsigned _pc = wz_panel_color(_yy, wz_top, wz_hh);
        gfx_rect(wz_lx+2, _yy, wz_lw-4, 1, _pc);
        gfx_rect(wz_rx+2, _yy, wz_rw-4, 1, _pc);
    }
    gfx_round_rect(wz_lx, wz_top, wz_lw, wz_hh, 18, 0x2E4E96);
    gfx_round_rect(wz_rx, wz_top, wz_rw, wz_hh, 18, 0x2E4E96);
    for (int _xx = wz_lx+14; _xx < wz_lx+wz_lw-14; _xx++) gfx_rect(_xx, wz_top+2, 1, 1, 0x2A3A6A);
    for (int _xx = wz_rx+14; _xx < wz_rx+wz_rw-14; _xx++) gfx_rect(_xx, wz_top+2, 1, 1, 0x2A3A6A);

    // Kairo round icon
    gfx_fill_circle_aa(wz_icx, wz_icy, 64, 0x3F7FE0);
    gfx_fill_circle_aa(wz_icx, wz_icy, 57, 0x2F5FC4);
    gfx_fill_circle_aa(wz_icx, wz_icy, 50, 0x244AA8);
    gfx_fill_circle_aa(wz_icx, wz_icy, 40, 0x1B3A90);
    gfx_fill_circle_aa(wz_icx, wz_icy, 30, 0x162E78);
    gfx_circle_aa(wz_icx, wz_icy, 63, 2, 0x6AA4FF);
    gfx_fill_circle_aa(wz_icx-22, wz_icy-26, 10, 0x88BBFF);
    {
        int _kw = wz_text_w("K", 3);
        gfx_print_scaled(wz_icx - _kw/2, wz_icy - 16, 0xEAF2FF, "K", 3);
    }
    gfx_print_scaled(wz_lx + wz_lw/2 - wz_text_w("KAIRO",2)/2, wz_icy + 72, 0xC6D6FF, "KAIRO", 2);
    gfx_print(wz_lx + wz_lw/2 - wz_text_w("SETUP",1)/2, wz_icy + 110, 0x6A7A9E, "SETUP");

    int setup_done = 0, setup_step = 0;
    int lang_sel = 0, kb_sel = 0, test_sel = 0;
    int cfg_lang = 0, cfg_layout = 0;
    int kb_pass = 0, mic_pass = 0, scr_pass = 0;
    char kb_buf[48] = {0}; int kb_len = 0;
    int scr_active = 0, scr_ans = 0, scr_ok_n = 0;
    int mic_on = 0, mic_prev = 0, mic_said = -1, mic_last_frames = 0;
    int frame = 0, mouse_was = 0;

    ac97_music_start();

    while (!setup_done) {
        const wz_lang_t *L = &wz_lang[setup_step==0 ? lang_sel : cfg_lang];
        int _clk = mouse_clicked(), _mkedge = _clk && !mouse_was; mouse_was = _clk;

        // left dynamic: step list
        wz_fill_panel_area(wz_lx+12, wz_top+380, wz_lw-24, 160, wz_top, wz_hh);
        {
            int _act = setup_step==0?0 : setup_step==1?1 : (setup_step>=2&&setup_step<=5)?2 : 3;
            const char *_labels[4] = {L->step_lang, L->step_kb, L->step_tests, L->step_ready};
            for (int _i=0;_i<4;_i++){
                int _y = wz_top+388+_i*42;
                if (_i==_act){ gfx_fill_circle_aa(wz_lx+26, _y+6, 5, 0x4499FF); gfx_print(wz_lx+42, _y, 0xFFFFFF, _labels[_i]); }
                else { gfx_circle_aa(wz_lx+26, _y+6, 5, 2, 0x2A3A5A); gfx_print(wz_lx+42, _y, 0x6A7A9E, _labels[_i]); }
            }
        }

        // right content
        wz_fill_panel_area(wz_cx, wz_cy, wz_cw, wz_ch, wz_top, wz_hh);

        if (setup_step==0){
            wz_centered(wz_cx + wz_cw/2, wz_cy, 2, 0xEAF2FF, L->t_choose);
            gfx_print(wz_cx + wz_cw/2 - wz_text_w(L->t_choose_sub,1)/2, wz_cy+40, 0x7A8AB0, L->t_choose_sub);
            int _ry0 = wz_cy + 86;
            for (int _i=0;_i<6;_i++){
                int _ry = _ry0 + _i*54;
                if (_i==lang_sel){
                    gfx_fill_round_rect_aa(wz_cx, _ry, wz_cw, 44, 10, 0x16306E);
                    gfx_round_rect(wz_cx, _ry, wz_cw, 44, 10, 0x3A6AD0);
                }
                wz_centered(wz_cx+wz_cw/2, _ry+13, 1, _i==lang_sel?0xFFFFFF:0x8A9ACE, wz_lang[_i].name);
            }
            gfx_print(wz_cx + wz_cw/2 - wz_text_w(L->hint_nav,1)/2, wz_top+wz_hh-44, 0x5A6A8E, L->hint_nav);
        }
        else if (setup_step==1){
            wz_centered(wz_cx + wz_cw/2, wz_cy, 2, 0xEAF2FF, L->t_kb);
            gfx_print(wz_cx + wz_cw/2 - wz_text_w(L->t_kb_sub,1)/2, wz_cy+40, 0x7A8AB0, L->t_kb_sub);
            int _ry0 = wz_cy + 96;
            for (int _i=0;_i<5;_i++){
                int _ry = _ry0 + _i*58;
                if (_i==kb_sel){
                    gfx_fill_round_rect_aa(wz_cx, _ry, wz_cw, 48, 10, 0x16306E);
                    gfx_round_rect(wz_cx, _ry, wz_cw, 48, 10, 0x3A6AD0);
                }
                wz_centered(wz_cx+wz_cw/2, _ry+16, 1, _i==kb_sel?0xFFFFFF:0x8A9ACE, wz_layouts[_i]);
            }
            gfx_print(wz_cx + wz_cw/2 - wz_text_w(L->hint_nav,1)/2, wz_top+wz_hh-44, 0x5A6A8E, L->hint_nav);
        }
        else if (setup_step==2){
            wz_centered(wz_cx + wz_cw/2, wz_cy, 2, 0xEAF2FF, L->t_tests);
            gfx_print(wz_cx + wz_cw/2 - wz_text_w(L->t_tests_sub,1)/2, wz_cy+40, 0x7A8AB0, L->t_tests_sub);
            const char *_tn[4] = {L->test_kb, L->test_mic, L->test_scr, L->continue_};
            int _tpass[4] = {kb_pass, mic_pass, scr_pass, 1};
            int _ry0 = wz_cy + 92;
            for (int _i=0;_i<4;_i++){
                int _ry = _ry0 + _i*58;
                if (_i==test_sel){
                    gfx_fill_round_rect_aa(wz_cx, _ry, wz_cw, 48, 10, 0x16306E);
                    gfx_round_rect(wz_cx, _ry, wz_cw, 48, 10, 0x3A6AD0);
                }
                gfx_print(wz_cx+20, _ry+16, _i==test_sel?0xFFFFFF:0x8A9ACE, _tn[_i]);
                if (_i<3){
                    const char *_st = _tpass[_i] ? L->pass : L->todo;
                    gfx_print(wz_cx+wz_cw-20-wz_text_w(_st,1), _ry+16, _tpass[_i]?0x33CC77:0x556688, _st);
                } else {
                    gfx_print(wz_cx+wz_cw-40, _ry+16, 0x44AAFF, "->");
                }
            }
            gfx_print(wz_cx + wz_cw/2 - wz_text_w(L->hint_nav,1)/2, wz_top+wz_hh-44, 0x5A6A8E, L->hint_nav);
        }
        else if (setup_step==3){
            wz_centered(wz_cx + wz_cw/2, wz_cy, 2, 0xEAF2FF, L->test_kb);
            gfx_print(wz_cx + wz_cw/2 - wz_text_w(L->kb_inst,1)/2, wz_cy+42, 0x7A8AB0, L->kb_inst);
            gfx_fill_round_rect_aa(wz_cx, wz_cy+84, wz_cw, 52, 12, 0x0A0A24);
            gfx_round_rect(wz_cx, wz_cy+84, wz_cw, 52, 12, 0x3A6AD0);
            char _dis[56]; int _di=0;
            for (; _di<kb_len && _di<40; _di++) _dis[_di] = kb_buf[_di];
            _dis[_di]=0;
            if (_di==0) _dis[_di]='|', _dis[_di+1]=0;
            gfx_print(wz_cx+16, wz_cy+100, kb_pass?0x33CC77:0xEAF2FF, _dis);
            if (kb_pass) gfx_print(wz_cx+wz_cw-160, wz_cy+96, 0x33CC77, L->kb_pass);
            gfx_print(wz_cx + wz_cw/2 - wz_text_w(L->hint_back,1)/2, wz_top+wz_hh-44, 0x5A6A8E, L->hint_back);
        }
        else if (setup_step==4){
            wz_centered(wz_cx + wz_cw/2, wz_cy, 2, 0xEAF2FF, L->test_mic);
            gfx_print(wz_cx + wz_cw/2 - wz_text_w(L->mic_inst,1)/2, wz_cy+42, 0x7A8AB0, L->mic_inst);
            int _mcx = wz_cx + wz_cw/2, _mcy = wz_cy + 150, _mr = 52;
            gfx_fill_circle_aa(_mcx, _mcy, _mr, mic_on?0x2266CC:0x16306E);
            gfx_circle_aa(_mcx, _mcy, _mr, 3, mic_on?0x66CCFF:0x3A6AD0);
            gfx_fill_round_rect_aa(_mcx-9, _mcy-24, 18, 34, 9, mic_on?0xEAF2FF:0x8A9ACE);
            gfx_fill_round_rect_aa(_mcx-16, _mcy+18, 32, 6, 3, mic_on?0xEAF2FF:0x8A9ACE);
            gfx_circle_aa(_mcx, _mcy-28, 10, 3, mic_on?0xEAF2FF:0x8A9ACE);
            gfx_print(_mcx - wz_text_w(L->mic_click,1)/2, _mcy+66, 0x8A9ACE, L->mic_click);
            if (mic_on){
                gfx_fill_circle_aa(_mcx-16, _mcy+96, 5, (frame/8)%2?0xFF4444:0xAA2222);
                gfx_print(_mcx+8, _mcy+90, 0xFF7777, L->mic_rec);
            } else if (mic_pass) {
                gfx_print(_mcx - wz_text_w(L->mic_ok,1)/2, _mcy+88, 0x33CC77, L->mic_ok);
            }
            int _lmx = wz_cx + 60, _lmw = wz_cw - 120, _lmy = wz_cy + 320, _lmh = 36;
            int _lv = mic_on ? ac97_stt_level() : 0;
            for (int _b=0;_b<24;_b++){
                int _bw = (_lmw-23*4)/24;
                int _bx = _lmx + _b*(_bw+4);
                int _h = (_lv * _lmh) / 32000; if (_h>_lmh)_h=_lmh;
                if (_h==0 && _lv>0) _h=2;
                unsigned _c = _b < 8 ? 0x33CC77 : _b < 16 ? 0xFFCC44 : 0xFF4444;
                if (!mic_on) _c = 0x1A2A4A;
                gfx_fill_round_rect_aa(_bx, _lmy+_lmh-_h, _bw, _h? _h:3, 2, _h>0?_c:0x0A0A24);
            }
            if (mic_on && (frame%10)==0){
                int _fr = ac97_stt_valid_frames();
                if (_fr > mic_last_frames){
                    int _chunk = _fr - mic_last_frames; if (_chunk > 44100) _chunk = 44100;
                    const int16_t *_buf = ac97_stt_buffer();
                    int _peak = ac97_stt_level();
                    if (_peak > 1500 || (frame/10)%2){
                        int _r = stt_recognize_capture(_buf + mic_last_frames*2, _chunk);
                        if (_r >= 0){ mic_said = _r; mic_pass = 1; }
                    }
                    mic_last_frames = _fr;
                }
            }
            if (mic_said >= 0){
                gfx_print(wz_cx + wz_cw/2 - wz_text_w(L->mic_said,1)/2, wz_cy+392, 0x8A9ACE, L->mic_said);
                gfx_print(wz_cx + wz_cw/2 - wz_text_w(stt_phrase_name(mic_said),1)/2, wz_cy+416, 0x66CCFF, stt_phrase_name(mic_said));
            } else if (!mic_on) {
                gfx_print(wz_cx + wz_cw/2 - wz_text_w(L->mic_noaudio,1)/2, wz_cy+392, 0x3A4A6A, L->mic_noaudio);
            }
            gfx_print(wz_cx + wz_cw/2 - wz_text_w(L->hint_back,1)/2, wz_top+wz_hh-44, 0x5A6A8E, L->hint_back);
        }
        else if (setup_step==5){
            wz_centered(wz_cx + wz_cw/2, wz_cy, 2, 0xEAF2FF, L->test_scr);
            gfx_print(wz_cx + wz_cw/2 - wz_text_w(L->scr_ask,1)/2, wz_cy+42, 0x7A8AB0, L->scr_ask);
            int _sw = (wz_cw - 6*10)/7;
            int _sy = wz_cy + 78, _sh = 56;
            for (int _i=0;_i<7;_i++){
                int _sx = wz_cx + _i*(_sw+10);
                if (_i==scr_active) gfx_fill_round_rect_aa(_sx-3, _sy-3, _sw+6, _sh+6, 12, 0xFFFFFF);
                gfx_fill_round_rect_aa(_sx, _sy, _sw, _sh, 10, wz_scr_cols[_i]);
                if (_i < scr_ok_n) gfx_print(_sx+_sw-16, _sy+_sh-18, 0x004422, "OK");
            }
            int _ary0 = _sy + _sh + 24;
            for (int _i=0;_i<7;_i++){
                int _p = wz_scr_perm[_i];
                int _ary = _ary0 + _i*34;
                if (_i==scr_ans){
                    gfx_fill_round_rect_aa(wz_cx, _ary, wz_cw, 28, 7, 0x16306E);
                    gfx_round_rect(wz_cx, _ary, wz_cw, 28, 7, 0x3A6AD0);
                }
                gfx_print(wz_cx+16, _ary+7, _i==scr_ans?0xFFFFFF:0x8A9ACE, L->col[_p]);
            }
            gfx_print(wz_cx + wz_cw/2 - wz_text_w(L->hint_back,1)/2, wz_top+wz_hh-44, 0x5A6A8E, L->hint_back);
        }
        else if (setup_step==6){
            wz_centered(wz_cx + wz_cw/2, wz_cy, 2, 0xEAF2FF, L->ready_title);
            gfx_print(wz_cx + wz_cw/2 - wz_text_w(L->ready_sub,1)/2, wz_cy+42, 0x7A8AB0, L->ready_sub);
            gfx_fill_round_rect_aa(wz_cx, wz_cy+84, wz_cw, 120, 12, 0x0A0A24);
            gfx_round_rect(wz_cx, wz_cy+84, wz_cw, 120, 12, 0x2A3A6A);
            gfx_print(wz_cx+24, wz_cy+98, 0x8A9ACE, L->step_lang);
            gfx_print(wz_cx+24, wz_cy+134, 0x8A9ACE, L->step_kb);
            gfx_print(wz_cx+wz_cw-24-wz_text_w(wz_lang[cfg_lang].name,1), wz_cy+98, 0xEAF2FF, wz_lang[cfg_lang].name);
            gfx_print(wz_cx+wz_cw-24-wz_text_w(wz_layouts[cfg_layout],1), wz_cy+134, 0xEAF2FF, wz_layouts[cfg_layout]);
            gfx_print(wz_cx+wz_cw/2 - wz_text_w(L->t_tests,1)/2, wz_cy+228, 0x8A9ACE, L->t_tests);
            int _bx = wz_cx + wz_cw/2 - 42;
            int _done[3] = {kb_pass, mic_pass, scr_pass};
            for (int _i=0;_i<3;_i++)
                gfx_fill_circle_aa(_bx+_i*42, wz_cy+254, 9, _done[_i]?0x33CC77:0xCC4444);
            gfx_fill_round_rect_aa(wz_cx+wz_cw/2-140, wz_cy+300, 280, 48, 24, 0x2266CC);
            gfx_round_rect(wz_cx+wz_cw/2-140, wz_cy+300, 280, 48, 24, 0x4A8AFF);
            wz_centered(wz_cx+wz_cw/2, wz_cy+312, 1, 0xFFFFFF, L->ready_enter);
        }

        draw_cursor();
        ac97_music_poll();
        frame++;

        char k = keyboard_last_char();

        if (setup_step==0){
            if (k==KEY_UP) lang_sel = (lang_sel+5)%6;
            if (k==KEY_DOWN) lang_sel = (lang_sel+1)%6;
            if (k=='\n'){ cfg_lang = lang_sel; setup_step = 1; }
            if (k==27) setup_done = 1;
        }
        else if (setup_step==1){
            if (k==KEY_UP && kb_sel>0) kb_sel--;
            if (k==KEY_DOWN && kb_sel<4) kb_sel++;
            if (k=='\n'){ cfg_layout = kb_sel; setup_step = 2; }
            if (k==27) setup_done = 1;
        }
        else if (setup_step==2){
            if (k==KEY_UP && test_sel>0) test_sel--;
            if (k==KEY_DOWN && test_sel<3) test_sel++;
            if (k=='\n'){
                if (test_sel==0){ setup_step=3; kb_len=0; kb_buf[0]=0; kb_pass=0; }
                else if (test_sel==1){ setup_step=4; mic_on=0; mic_said=-1; mic_last_frames=0; ac97_stt_stop(); }
                else if (test_sel==2){ setup_step=5; scr_active=0; scr_ans=0; scr_ok_n=0; }
                else setup_step=6;
            }
            if (k==27) setup_done=1;
        }
        else if (setup_step==3){
            if (k=='\b'){ if (kb_len>0){ kb_len--; kb_buf[kb_len]=0; } }
            else if (k==KEY_UP||k==KEY_DOWN||k==KEY_LEFT||k==KEY_RIGHT){
                if (kb_len<44){ kb_buf[kb_len++] = (k==KEY_UP)?'^':(k==KEY_DOWN)?'v':(k==KEY_LEFT)?'<':'>'; kb_buf[kb_len]=0; }
            }
            else if (k>=' ' && k<='~' && k!='\n'){ if (kb_len<44){ kb_buf[kb_len++]=k; kb_buf[kb_len]=0; } }
            if (kb_len>=5) kb_pass=1;
            if (k=='\n'||k==27) setup_step=2;
        }
        else if (setup_step==4){
            if (_mkedge && mouse_get_x()>wz_cx+wz_cw/2-70 && mouse_get_x()<wz_cx+wz_cw/2+70 &&
                mouse_get_y()>wz_cy+150-70 && mouse_get_y()<wz_cy+150+70) mic_on = !mic_on;
            if (k=='\n'||k==' '){
                if (mic_on && k=='\n'){ mic_on=0; }
                else mic_on = !mic_on;
            }
            if (mic_on != mic_prev){
                if (mic_on){ ac97_stt_start(); mic_said=-1; mic_last_frames=0; }
                else ac97_stt_stop();
                mic_prev = mic_on;
            }
            if (k==27 || (k=='\n' && !mic_on)){ ac97_stt_stop(); mic_on=0; mic_prev=0; setup_step=2; }
            if (mic_on){ if (ac97_stt_level()>1800) mic_pass=1; }
        }
        else if (setup_step==5){
            if (k==KEY_UP && scr_ans>0) scr_ans--;
            if (k==KEY_DOWN && scr_ans<6) scr_ans++;
            if (k=='\n'){
                if (wz_scr_perm[scr_ans]==scr_active){ scr_ok_n++; if (scr_ok_n>=7) scr_pass=1; }
                if (scr_ok_n<7) scr_active=(scr_active+1)%7;
            }
            if (k==27 || (k=='\n' && scr_pass)) setup_step=2;
        }
        else if (setup_step==6){
            if (k=='\n'||k==27) setup_done=1;
        }

        asm volatile("hlt");
    }
    keyboard_last_char();

    // Apply setup results
    if (!account_name[0]) {
        const char *_def = "KairoUser";
        int _di = 0; for (; _def[_di] && _di < 39; _di++) account_name[_di] = _def[_di];
        account_name[_di] = 0;
    }
    set_state |= 1;   // dark mode on by default

    delay();

    // Startup sound when the desktop is reached
    play_startup_melody();

    // ─── Desktop (macOS style) ───
redraw_desktop:
    cursor_hide();
    int dm = set_state & 1;
    // Blit wallpaper image (fast direct copy)
    {
        volatile uint32_t *fb = gfx_get_fb_addr();
        int copy_w = (w < WP_W) ? w : WP_W;
        int copy_h = (h < WP_H) ? h : WP_H;
        for (int y = 0; y < copy_h; y++) {
            for (int x = 0; x < copy_w; x++) {
                fb[y * (gfx_get_pitch() / 4) + x] = wallpaper_data[y * WP_W + x];
            }
        }
    }
    // Snow ground when natal
    if (_natal_mode) {
        for (int y = h-40; y < h; y++) {
            int _sn = h-y;
            int _sr = 0xAA + _sn*3; if (_sr > 255) _sr = 255;
            int _sg = 0xCC + _sn*2; if (_sg > 255) _sg = 255;
            int _sb = 0xEE + _sn; if (_sb > 255) _sb = 255;
            gfx_rect(0, y, w, 1, (_sr<<16)|(_sg<<8)|_sb);
        }
    }
    // Wide radial glow from top-center (softer, wider)
    if (!dm) for (int i = 0; i < 120; i++) {
        int a = (120-i)*2; if (a > 50) a = 50;
        int ww = i*7;
        gfx_rect(w/2 - ww/2, h/6 - i/5, ww, 2, (a*2/3<<16)|(a/3<<8)|a);
        gfx_rect(w/2 - ww/2, h/6 + i/5, ww, 2, (a*2/3<<16)|(a/3<<8)|a);
    }
    // Stars with varying sizes and brightness
    int n_stars = dm ? 40 : 120;
    for (int i = 0; i < n_stars; i++) {
        int sx = (i*709+53)%w, sy = (i*997+31)%(h*3/5);
        int sb = (i*311+17)%10;
        if (sb < 2) continue;
        int sz = (sb > 7) ? 2 : 1;
        uint32_t sc;
        if (_natal_mode) {
            if (sb > 7) sc = 0xFF4444;
            else if (sb > 4) sc = 0x44FF44;
            else sc = 0xFFFFFF;
        } else {
            if (sb > 7) sc = dm ? 0x446688 : 0xCCDDFF;
            else if (sb > 4) sc = dm ? 0x335577 : 0x99AACC;
            else sc = dm ? 0x224466 : 0x667799;
        }
        gfx_fill_round_rect(sx-sz+1, sy-sz+1, sz*2-1, sz*2-1, sz, sc);
        // Brighter core for larger stars
        if (sz > 1) gfx_putpixel(sx, sy, dm ? 0x88BBEE : 0xFFFFFF);
    }
    // Christmas easter egg: snowflakes + tree
    if (_natal_mode) {
        // Falling snowflakes
        static int _nsf = 0; _nsf++;
        for (int _nf = 0; _nf < 64; _nf++) {
            _natal_snow[_nf][1] = (_natal_snow[_nf][1] + 1 + (_nf % 3)) % h;
            _natal_snow[_nf][0] = (_natal_snow[_nf][0] + ((_nf * 7 + _nsf / 20) % 3) - 1) % w;
            if (_natal_snow[_nf][0] < 0) _natal_snow[_nf][0] += w;
            gfx_putpixel(_natal_snow[_nf][0], _natal_snow[_nf][1], 0xFFFFFF);
            if (_nf % 5 == 0) gfx_putpixel(_natal_snow[_nf][0] + 1, _natal_snow[_nf][1], 0xFFFFFF);
        }
        // Christmas tree (bottom-left)
        int _tx = 30, _ty = h - 180;
        for (int _tr = 0; _tr < 5; _tr++) {
            int _tw = 18 + _tr * 8, _th = 25;
            uint32_t _tc = _tr % 2 == 0 ? 0x228833 : 0x1A7722;
            for (int _y = 0; _y < _th; _y++) {
                int _tlw = _tw - (_y * _tw / _th) / 2;
                gfx_rect(_tx - _tlw / 2, _ty + _tr * _th + _y, _tlw, 1, _tc & 0xFEFEFE);
            }
        }
        // Trunk
        gfx_rect(_tx - 4, _ty + 125, 8, 20, 0x664422);
        gfx_rect(_tx - 3, _ty + 126, 6, 18, 0x885533);
        // Ornaments on tree
        gfx_fill_round_rect(_tx - 8, _ty + 10, 6, 6, 3, 0xFF2222);
        gfx_fill_round_rect(_tx + 5, _ty + 35, 6, 6, 3, 0x2222FF);
        gfx_fill_round_rect(_tx - 10, _ty + 55, 6, 6, 3, 0xFFDD00);
        gfx_fill_round_rect(_tx + 8, _ty + 70, 6, 6, 3, 0xFF4444);
        gfx_fill_round_rect(_tx - 6, _ty + 95, 6, 6, 3, 0x44AAFF);
        // Star on top
        gfx_fill_round_rect(_tx - 4, _ty - 10, 8, 8, 4, 0xFFDD00);
        gfx_print_scaled(_tx - 3, _ty - 20, 0xFFDD00, "*", 1);
        // Candles near the tree
        static int _cfl = 0; _cfl++;
        for (int _cn = 0; _cn < 3; _cn++) {
            int _cx = _tx + 70 + _cn * 22, _cy = _ty + 100 + _cn * 8;
            int _ch = 25 + _cn * 5;
            gfx_rect(_cx-2, _cy-_ch, 4, _ch, 0xDDCC88);
            gfx_rect(_cx-1, _cy-_ch, 2, _ch-2, 0xEEDDAA);
            gfx_fill_round_rect(_cx-3, _cy-_ch-6, 6, 6, 3, 0xFF6600);
            gfx_putpixel(_cx, _cy-_ch-8, 0xFFFF44);
            int _gf = (_cfl/20+_cn*7)%4;
            gfx_fill_round_rect(_cx-5-_gf, _cy-_ch-9-_gf, 10+_gf*2, 10+_gf*2, 6, 0xFF660022);
        }
        // Merry Christmas text
        gfx_print_scaled(w/2 - 90, 28, 0xFF4444, "BUON NATALE!", 2);
        gfx_print_scaled(w/2 - 80, 52, 0x44FF44, "Merry Christmas", 1);
    }

    // Subtle vignette: darken the edges for depth (costs ~4k px)
    for (int _vi = 0; _vi < 40; _vi++) {
        int _va = (39 - _vi) * 2;          // alpha 0..78, stronger toward edges
        gfx_rect_alpha(0, h - 40 + _vi, w, 1, 0x000000, _va > 12 ? 12 : _va);
        gfx_rect_alpha(0, _vi, w, 1, 0x000000, _va > 10 ? 10 : _va);
    }
    for (int _vi = 0; _vi < 40; _vi++) {
        int _va = (39 - _vi) * 2;
        gfx_rect_alpha(w - 40 + _vi, 0, 1, h, 0x000000, _va > 10 ? 10 : _va);
        gfx_rect_alpha(_vi, 0, 1, h, 0x000000, _va > 8 ? 8 : _va);
    }

    // ─── Top menu bar — REMOVED ───
    int mby = 0, mbh = 0;
    dm = set_state & 1;

    // ─── Dock (macOS style) ───
redraw_dock:
    // Restore wallpaper over dock+home region when entering via goto
    {
        volatile uint32_t *_rfb = gfx_get_fb_addr();
        int _rpitch = gfx_get_pitch() / 4;
        for (int _ry = h-180; _ry < h; _ry++) {
            for (int _rx = 0; _rx < w; _rx++) {
                if (_ry >= 0 && _ry < 720 && _rx >= 0 && _rx < 1280)
                    _rfb[_ry * _rpitch + _rx] = wallpaper_data[_ry * 1280 + _rx];
            }
        }
    }
    int dc_w = 640, dc_h = 60, dc_r = 30;
    int dc_x = w/2 - dc_w/2, dc_y = h - dc_h - 10;
    // Deep shadow
    gfx_fill_round_rect(dc_x+4, dc_y+4, dc_w, dc_h, dc_r, 0x000000);
    gfx_fill_round_rect(dc_x+2, dc_y+2, dc_w, dc_h, dc_r, 0x000000);
    // Dock background (glass-like)
    uint32_t dk_bg = _natal_mode ? 0x080400 : (dm ? 0x04040E : 0x080820);
    uint32_t dk_bd = _natal_mode ? 0x660000 : (dm ? 0x1A1A3A : 0x2A2A5A);
    uint32_t dk_bd2 = _natal_mode ? 0x440000 : (dm ? 0x0E0E2A : 0x1A1A4A);
    gfx_fill_round_rect(dc_x, dc_y, dc_w, dc_h, dc_r, dk_bg);
    gfx_round_rect(dc_x, dc_y, dc_w, dc_h, dc_r, dk_bd);
    gfx_round_rect(dc_x+1, dc_y+1, dc_w-2, dc_h-2, dc_r-1, dk_bd2);
    // Glass highlight line (brighter at center, fading to edges)
    for (int i = 0; i < dc_w-80; i += 4) {
        int a = 40 - (i < (dc_w-80)/2 ? i : (dc_w-80)-i) * 30 / ((dc_w-80)/2);
        if (a < 8) a = 8;
        uint32_t hl = (a*3/4<<16)|(a/2<<8)|a;
        gfx_rect(dc_x+40+i, dc_y+3, 4, 1, hl);
    }
    gfx_rect(dc_x+40, dc_y+4, dc_w-80, 1, dm ? 0x121A3A : 0x2A3A6A);
    // Subtle glow beneath dock
    for (int i = 0; i < 6; i++) {
        int a = (6-i) * 3;
        uint32_t gl = (a/2<<16)|(a/4<<8)|a;
        gfx_rect(dc_x+30+i*5, dc_y+dc_h+i, dc_w-60-i*10, 1, gl);
    }

    // ─── Home button (left of dock) — slide-up animation ───
    {
    // home_btn_sz: 0=hidden (below dock), 1000=final position
    int _hs = home_btn_sz;
    int _final_y = dc_y + 4;
    int _start_y = h + 10;  // below screen
    int _cur_y = _start_y - (_start_y - _final_y) * _hs / 1000;
    int _hb_x = dc_x - 76, _hb_w = 56, _hb_h = 52;
    if (_hs > 5) { // draw only when visible
    // Shadow
    gfx_fill_round_rect(_hb_x+2, _cur_y+2, _hb_w, _hb_h, 16, 0x000000);
    // Body
    gfx_fill_round_rect(_hb_x, _cur_y, _hb_w, _hb_h, 16, 0x0C0C2A);
    gfx_round_rect(_hb_x, _cur_y, _hb_w, _hb_h, 16, 0x3A6AFF);
    gfx_round_rect(_hb_x+1, _cur_y+1, _hb_w-2, _hb_h-2, 15, 0x2A4ABE);
    // Triangle + icon
    { int _hcx = _hb_x + _hb_w/2, _hcy = _cur_y + 12;
      draw_icon_centered(_hb_x, _cur_y, _hb_h, ICON_HOME, 0x88BBFF, 28.0f); }
    home_btn_prev_y = _cur_y; // sync prev position after normal dock draw
    }
    }

    // ─── Dock Icons — macOS magnification ───
    int di_y = dc_y + 8, di_base_sz = 40, di_sp = 56;
    int di_base = dc_x + (dc_w - 10*di_sp)/2;
    int di_x, di_cy;

    // Per-icon magnification: compute size and Y for each icon
    int di_sz_arr[10], di_yy_arr[10], di_xx_arr[10];
    for (int _mi = 0; _mi < 10; _mi++) {
        int _msz = di_base_sz * dock_sz[_mi] / 1000;
        di_sz_arr[_mi] = _msz;
        di_yy_arr[_mi] = dc_y + 8 + (di_base_sz - _msz) / 2;
        di_xx_arr[_mi] = di_base + _mi * di_sp + (di_base_sz - _msz) / 2;
    }

    #define di_sh(_i,_c) do {\
        di_x = di_xx_arr[_i]; di_cy = di_yy_arr[_i];\
        int _sz = di_sz_arr[_i];\
        gfx_fill_round_rect(di_x+1,di_cy+1,_sz,_sz,9,0x000000);\
        if (_natal_mode) {\
            for (int _ni = 0; _ni < 4; _ni++) {\
                uint32_t _gac = (_i%2==0)?(0x440000+_ni*0x004400):(0x000044+_ni*0x440000);\
                gfx_fill_round_rect(di_x-2-_ni,di_cy-2-_ni,_sz+4+_ni*2,_sz+4+_ni*2,12+_ni*2,_gac);\
            }\
        }\
        gfx_fill_round_rect(di_x,di_cy,_sz,_sz,10,_c);\
        gfx_round_rect(di_x,di_cy,_sz,_sz,10,_natal_mode?0xFF4444:0x4A6AAF);\
    } while(0)

    // PASS 1: Draw all icon backgrounds (shadows, fills, borders)
    di_sh(0, 0x2A4A2A);
    di_sh(1, 0x4488FF);
    di_sh(2, 0x6622AA);
    di_sh(3, 0x5A3A8A);
    di_sh(4, 0x3A3A4A);
    di_sh(5, 0x886622);
    di_sh(6, 0x2266AA);
    di_sh(7, 0x3A5A8A);
    di_sh(8, 0x3A1A5A);
    di_sh(9, 0x3A3A5A);

    // PASS 2: Draw all Material Icons ON TOP (never covered by adjacent boxes)
    static const int _dock_cp[] = {ICON_PUBLIC, ICON_MUSIC_NOTE, ICON_GAME, ICON_CHAT, ICON_CALCULATOR, ICON_EDIT, ICON_SHOP, ICON_CLOUD, ICON_GAMES, ICON_COMPUTER};
    static const uint32_t _dock_clr[] = {0x66CC66, 0xFFFFFF, 0xFF88FF, 0xBBAAFF, 0x88CC88, 0xFFEE88, 0xFFFFFF, 0xCCDDEE, 0xBB44EE, 0xFFFFFF};
    for (int _mi = 0; _mi < 10; _mi++) {
        int ix = di_xx_arr[_mi];
        int iy = di_yy_arr[_mi];
        int isz = di_sz_arr[_mi];
        float isz_f = (float)isz * 24.0f / 40.0f;
        draw_icon_centered(ix, iy, isz, _dock_cp[_mi], _dock_clr[_mi], isz_f);
    }
    
    // App indicator dots under active icons (macOS style: small, bright for active)
    int _active_di = -1;
    if(win){
        int _wt = win_type;
        if(_wt==34)_active_di=0; else if(_wt==33)_active_di=1;
        else if(_wt==25)_active_di=2; else if(_wt==3)_active_di=3;
        else if(_wt==6)_active_di=4; else if(_wt==32)_active_di=5;
        else if(_wt==30)_active_di=6; else if(_wt==31)_active_di=7;
        else if(_wt==26)_active_di=8; else if(_wt==24)_active_di=9;
    }
    for (int _di = 0; _di < 10; _di++) {
        int _dot_x = di_base+_di*di_sp+16;
        int _dot_w = 8;
        if(_di == _active_di){
            gfx_fill_round_rect(_dot_x-1, dc_y+dc_h-5, _dot_w+2, 4, 2, 0x4488FF);
            gfx_fill_round_rect(_dot_x, dc_y+dc_h-4, _dot_w, 2, 1, 0x88BBFF);
        } else {
            gfx_fill_round_rect(_dot_x+1, dc_y+dc_h-4, _dot_w-2, 2, 1, 0x2A3A5A);
        }
    }

    // Separator + Trash on right side (macOS style)
    int dc_tr = dc_x+dc_w-38;
    gfx_rect(dc_tr-6, dc_y+12, 1, dc_h-24, dm ? 0x1A1A3A : 0x2A2A5A);
    // Trash bin
    gfx_round_rect(dc_tr, dc_y+16, 12, 14, 3, dm ? 0x3A4A6A : 0x4A5A7A);
    gfx_fill_round_rect(dc_tr+1, dc_y+15, 10, 4, 2, dm ? 0x3A4A6A : 0x4A5A7A);
    gfx_rect(dc_tr+3, dc_y+19, 6, 8, dm ? 0x1A2A3A : 0x2A3A4A);
    gfx_rect(dc_tr+5, dc_y+20, 2, 6, dm ? 0x3A4A5A : 0x5A6A7A);

    // Notes widget on desktop (initial draw)
    gfx_fill_round_rect(30,90,210,140,8,0x080820);
    gfx_round_rect(30,90,210,140,8,0x2A3A6A);
    gfx_print(42,95,0x6A8ABE,"Notes");
    gfx_fill_round_rect(210,94,18,14,7,0xCC4444);
    gfx_print(216,94,0xFFFFFF,"0");
    gfx_print(42,126,0x3A4A6A,"No notes  [7]");

    // Desktop analog clock widget (right side)
    int dc_x2 = w - 170, dc_y2 = 80;
    int clk_r = 55, dc_cx = dc_x2 + 70, dc_cy = dc_y2 + 60;
    gfx_fill_circle_aa(dc_cx, dc_cy, clk_r + 4, 0x080820);
    gfx_circle_aa(dc_cx, dc_cy, clk_r + 4, 3, 0x2A3A6A);
    gfx_fill_circle_aa(dc_cx, dc_cy, clk_r, 0x0C0C2A);
    for (int _hi = 0; _hi < 12; _hi++) {
        float _ha = (float)_hi * 6.28318f / 12.0f - 1.57079f;
        float _hx1 = (float)dc_cx + k_cosf(_ha) * (clk_r - 10);
        float _hy1 = (float)dc_cy + k_cosf(_ha - 1.57079f) * (clk_r - 10);
        float _hx2 = (float)dc_cx + k_cosf(_ha) * (clk_r - 3);
        float _hy2 = (float)dc_cy + k_cosf(_ha - 1.57079f) * (clk_r - 3);
        gfx_line((int)_hx1, (int)_hy1, (int)_hx2, (int)_hy2, 2, 0x4488FF);
    }
    int _dhs = 0, _dms = 0, _dss = 0; rtc_read_time(&_dhs, &_dms, &_dss);
    float _ha_h = ((float)_dhs + (float)_dms / 60.0f) * 6.28318f / 12.0f - 1.57079f;
    float _ha_m = ((float)_dms + (float)_dss / 60.0f) * 6.28318f / 60.0f - 1.57079f;
    float _ha_s = (float)_dss * 6.28318f / 60.0f - 1.57079f;
    gfx_line(dc_cx, dc_cy, (int)(dc_cx + k_cosf(_ha_h) * 28), (int)(dc_cy + k_cosf(_ha_h - 1.57079f) * 28), 3, 0xFFFFFF);
    gfx_line(dc_cx, dc_cy, (int)(dc_cx + k_cosf(_ha_m) * 38), (int)(dc_cy + k_cosf(_ha_m - 1.57079f) * 38), 2, 0x88BBFF);
    gfx_line(dc_cx, dc_cy, (int)(dc_cx + k_cosf(_ha_s) * 42), (int)(dc_cy + k_cosf(_ha_s - 1.57079f) * 42), 1, 0xFF4444);
    gfx_fill_circle_aa(dc_cx, dc_cy, 3, 0xFF4444);
    // Weather widget (below clock)
    int _wwx = w - 170, _wwy = 160;
    gfx_fill_round_rect(_wwx, _wwy, 140, 80, 8, 0x080820);
    gfx_round_rect(_wwx, _wwy, 140, 80, 8, 0x2A3A6A);
    gfx_print(_wwx+10, _wwy+6, 0x6688BB, "Meteo");
    // Cloud icon
    gfx_fill_round_rect(_wwx+16, _wwy+28, 40, 18, 9, 0x445577);
    gfx_fill_round_rect(_wwx+26, _wwy+18, 20, 28, 10, 0x445577);
    // Sun
    gfx_fill_round_rect(_wwx+72, _wwy+20, 18, 18, 9, 0xFFCC44);
    gfx_putpixel(_wwx+93, _wwy+18, 0xFFCC44);
    gfx_putpixel(_wwx+93, _wwy+32, 0xFFCC44);
    gfx_putpixel(_wwx+78, _wwy+14, 0xFFCC44);
    gfx_putpixel(_wwx+88, _wwy+14, 0xFFCC44);
    gfx_putpixel(_wwx+78, _wwy+38, 0xFFCC44);
    gfx_putpixel(_wwx+88, _wwy+38, 0xFFCC44);
    gfx_putpixel(_wwx+70, _wwy+20, 0xFFCC44);
    gfx_putpixel(_wwx+70, _wwy+30, 0xFFCC44);

    // Search in top bar instead of dock
    int sb_x = w/2 - 120, sb_y = mbh+3, sb_w = 240, sb_h = 20;
    gfx_fill_round_rect(sb_x, sb_y, sb_w, sb_h, 10, 0x0A0A28);
    gfx_round_rect(sb_x, sb_y, sb_w, sb_h, 10, search_focus ? 0x00E5FF : 0x3A5A8A);
    if (search_buf[0]) gfx_print_shadow(sb_x+10, sb_y+3, TEXT, search_buf);
    else if (search_focus) gfx_print_shadow(sb_x+10, sb_y+3, 0x5A7AAA, "Cerca app...");
    else gfx_print_shadow(sb_x+10, sb_y+3, 0x3A5A8A, "Search...");
    // NIC status indicator
    if (nic_ready) {
        gfx_fill_round_rect(sb_x+sb_w+12, sb_y, 110, sb_h, 10, 0x082808);
        gfx_round_rect(sb_x+sb_w+12, sb_y, 110, sb_h, 10, 0x2A5A2A);
        gfx_print(sb_x+sb_w+16, sb_y+2, 0x44CC44, "NIC ready");
    } else {
        gfx_fill_round_rect(sb_x+sb_w+12, sb_y, 110, sb_h, 10, 0x280808);
        gfx_round_rect(sb_x+sb_w+12, sb_y, 110, sb_h, 10, 0x5A2A2A);
        gfx_print(sb_x+sb_w+16, sb_y+2, 0xCC4444, "NIC: none");
    }

    // Search focused by default
    search_focus = 1; int _tick = 0;

    // Clock refresh helper (desktop widget only — top bar removed)
    #define clock_refresh() do {\
        int _c_hr = 12, _c_mn = 0, _c_sc = 0; rtc_read_time(&_c_hr, &_c_mn, &_c_sc);\
        if(!win){\
            int _dmc = set_state & 1;\
            gfx_fill_circle_aa(dc_cx, dc_cy, clk_r + 4, _dmc?0x040410:0x080820);\
            gfx_circle_aa(dc_cx, dc_cy, clk_r + 4, 3, _dmc?0x1A1A3A:0x2A3A6A);\
            gfx_fill_circle_aa(dc_cx, dc_cy, clk_r, _dmc?0x060618:0x0C0C2A);\
            for(int _hi2=0;_hi2<12;_hi2++){\
                float _ha2=(float)_hi2*6.28318f/12.0f-1.57079f;\
                float _hx1_2=(float)dc_cx+k_cosf(_ha2)*(clk_r-10);\
                float _hy1_2=(float)dc_cy+k_cosf(_ha2-1.57079f)*(clk_r-10);\
                float _hx2_2=(float)dc_cx+k_cosf(_ha2)*(clk_r-3);\
                float _hy2_2=(float)dc_cy+k_cosf(_ha2-1.57079f)*(clk_r-3);\
                gfx_line((int)_hx1_2,(int)_hy1_2,(int)_hx2_2,(int)_hy2_2,2,_dmc?0x3366AA:0x4488FF);\
            }\
            float _ah=((float)_c_hr+(float)_c_mn/60.0f)*6.28318f/12.0f-1.57079f;\
            float _am=((float)_c_mn+(float)_c_sc/60.0f)*6.28318f/60.0f-1.57079f;\
            float _as=(float)_c_sc*6.28318f/60.0f-1.57079f;\
            gfx_line(dc_cx,dc_cy,(int)(dc_cx+k_cosf(_ah)*28),(int)(dc_cy+k_cosf(_ah-1.57079f)*28),3,0xFFFFFF);\
            gfx_line(dc_cx,dc_cy,(int)(dc_cx+k_cosf(_am)*38),(int)(dc_cy+k_cosf(_am-1.57079f)*38),2,0x88BBFF);\
            gfx_line(dc_cx,dc_cy,(int)(dc_cx+k_cosf(_as)*42),(int)(dc_cy+k_cosf(_as-1.57079f)*42),1,0xFF4444);\
            gfx_fill_circle_aa(dc_cx,dc_cy,3,0xFF4444);\
        }\
        /* Desktop star twinkling */\
        if(!win && !cc_active && !nc_active){\
            int _isdm = set_state & 1;\
            int _nst = _isdm ? 20 : 60;\
            for(int _si=0;_si<3;_si++){\
                int _sidx = ((_tick*13+_si*17)*7+_tick/10)%_nst;\
                int _sx = (_sidx*691+47)%w;\
                int _sy = (_sidx*983+19)%(h*3/5);\
                int _phase = (_tick + _sidx*3 + _si*7) % 18;\
                uint32_t _sc;\
                if(_phase < 4) _sc = _isdm ? 0x1A3355 : 0x445577;\
                else if(_phase < 11) _sc = _isdm ? 0x4466AA : 0xAABBEE;\
                else _sc = _isdm ? 0x6688CC : 0xCCDDFF;\
                gfx_fill_round_rect(_sx-1,_sy-1,3,3,1,_sc);\
            }\
        }\
    } while(0)

    // Settings overlay panel draw (centered, not windowed)
    #define settings_redraw() do {\
        int _dmc = set_state & 1;\
        int _sw = 600, _sh = 420;\
        int _sx = (w - _sw) / 2, _sy = (h - _sh) / 2;\
        /* panel shadow */\
        for(int _si=0;_si<6;_si++){int _sa=(6-_si)*3;uint32_t _sc=(_sa<<16)|(_sa<<8)|_sa;gfx_rect(_sx+4+_si,_sy+4+_si,_sw,_sh,_sc);}\
        /* panel body */\
        gfx_fill_round_rect(_sx,_sy,_sw,_sh,12,0x0A0A24);\
        gfx_round_rect(_sx,_sy,_sw,_sh,12,0x3A6AFF);\
        gfx_round_rect(_sx+1,_sy+1,_sw-2,_sh-2,11,0x1A2A5A);\
        /* title bar */\
        gfx_fill_round_rect(_sx,_sy,_sw,36,12,0x0E1A3A);\
        gfx_rect(_sx,_sy+36,_sw,1,0x2A3A6A);\
        gfx_print(_sx+16,_sy+10,0xFFFFFF,"Settings");\
        /* close button */\
        gfx_fill_round_rect(_sx+_sw-32,_sy+6,24,24,12,0x1A1A3A);\
        gfx_round_rect(_sx+_sw-32,_sy+6,24,24,12,0x4A5A8A);\
        gfx_print(_sx+_sw-24,_sy+11,0xFFFFFF,"X");\
        /* sidebar */\
        int _sbw = 130;\
        gfx_rect(_sx+8,_sy+44,_sbw,_sh-52,_dmc?0x050512:0x0A0A24);\
        const char *_cats[] = {"General","Display","Network","Sound","About","Parental"};\
        for(int _ci=0;_ci<6;_ci++){\
            int _cy = _sy + 50 + _ci*32;\
            if(_ci==set_cat){\
                gfx_fill_round_rect(_sx+10,_cy,_sbw-4,26,6,0x1A3A8A);\
                gfx_print(_sx+20,_cy+6,0xFFFFFF,_cats[_ci]);\
            }else{\
                gfx_print(_sx+20,_cy+6,_dmc?0x4A5A7E:0x6A7A9E,_cats[_ci]);\
            }\
        }\
        /* divider */\
        gfx_rect(_sx+_sbw+8,_sy+44,1,_sh-52,_dmc?0x0E0E2A:0x1A1A4A);\
        /* content area */\
        int _cx = _sx+_sbw+24, _cy2 = _sy+58;\
        if(set_cat==0){\
            gfx_print(_cx,_cy2,0x4488FF,"General");_cy2+=24;\
            draw_toggle(_cx,_cy2,set_state&1);\
            gfx_print(_cx+32,_cy2-1,set_state&1?0x4488FF:0x8A9ACE,"Dark Mode");_cy2+=28;\
            draw_toggle(_cx,_cy2,set_state&2);\
            gfx_print(_cx+32,_cy2-1,0x8A9ACE,"Notifications");_cy2+=28;\
            draw_toggle(_cx,_cy2,set_state&4);\
            gfx_print(_cx+32,_cy2-1,0x8A9ACE,"Developer Mode");_cy2+=28;\
        }else if(set_cat==1){\
            gfx_print(_cx,_cy2,0x4488FF,"Display");_cy2+=24;\
            gfx_print(_cx,_cy2,_dmc?0x4A5A7E:0x6A7A9E,"1280x720");_cy2+=20;\
            gfx_print(_cx,_cy2,_dmc?0x2A3A5E:0x4A5A7E,"Brightness");_cy2+=16;\
            gfx_rect(_cx,_cy2,160,4,0x222244);\
            gfx_rect(_cx,_cy2,120,4,0x4488FF);\
            gfx_rect(_cx+118,_cy2-1,6,6,0x66BBFF);_cy2+=24;\
            gfx_print(_cx,_cy2,0x3A4A6A,"VBE Bochs Graphics");_cy2+=16;\
            gfx_print(_cx,_cy2,0x3A4A6A,"32-bit color");\
        }else if(set_cat==2){\
            gfx_print(_cx,_cy2,0x4488FF,"Network");_cy2+=24;\
            draw_toggle(_cx,_cy2,set_state&8);\
            gfx_print(_cx+32,_cy2-1,0x8A9ACE,"Wi-Fi");_cy2+=28;\
            gfx_print(_cx,_cy2,_dmc?0x2A3A5E:0x4A5A7E,"Status:");\
            gfx_print(_cx+56,_cy2,set_state&8?0x44FF44:0xFF6644,set_state&8?"Connected":"Disconnected");_cy2+=20;\
            gfx_print(_cx,_cy2,0x3A4A6A,"IP: 10.0.2.15");_cy2+=16;\
            gfx_print(_cx,_cy2,0x3A4A6A,"via serial proxy");\
        }else if(set_cat==3){\
            extern void ac97_set_volume(int); extern void ac97_set_mute(int);\
            extern int ac97_get_volume(void); extern int ac97_get_mute(void);\
            gfx_print(_cx,_cy2,0x4488FF,"Sound");_cy2+=24;\
            gfx_print(_cx,_cy2,_dmc?0x4A5A7E:0x6A7A9E,"Output: AC97");_cy2+=20;\
            gfx_print(_cx,_cy2,_dmc?0x2A3A5E:0x4A5A7E,"Master Volume");_cy2+=16;\
            int _vw = vol_level * 160 / 31;\
            gfx_rect(_cx,_cy2,160,6,0x222244);\
            gfx_rect(_cx,_cy2,_vw,6,vol_mute?0x666666:0x4488FF);\
            gfx_rect(_cx+_vw-3,_cy2-2,6,10,vol_mute?0x888888:0x66BBFF);_cy2+=16;\
            char _vd[8]; _vd[0]='0'+vol_level/10; _vd[1]='0'+vol_level%10; _vd[2]='%'; _vd[3]=0;\
            gfx_print(_cx,_cy2,vol_mute?0x666666:0x4488FF,_vd);_cy2+=20;\
            draw_toggle(_cx,_cy2,vol_mute);\
            gfx_print(_cx+32,_cy2-1,vol_mute?0xFF6644:0x8A9ACE,"Mute");_cy2+=28;\
            gfx_print(_cx,_cy2,0x3A4A6A,"[</> ] Vol  [M] Mute  [Space] Test");\
        }else if(set_cat==4){\
            gfx_print(_cx,_cy2,0x4488FF,"About Viteza OS");_cy2+=24;\
            gfx_print(_cx,_cy2,0x8A9ACE,"Version: 1.0");_cy2+=20;\
            gfx_print(_cx,_cy2,_dmc?0x4A5A7E:0x6A7A9E,"Kernel: x86_64 Long Mode");_cy2+=20;\
            gfx_print(_cx,_cy2,_dmc?0x4A5A7E:0x6A7A9E,"RAM: 256 MB");_cy2+=20;\
            gfx_print(_cx,_cy2,_dmc?0x4A5A7E:0x6A7A9E,"Display: VBE 1280x720");_cy2+=20;\
            gfx_print(_cx,_cy2,_dmc?0x4A5A7E:0x6A7A9E,"AI: OreoAI + Ollama");_cy2+=20;\
            gfx_print(_cx,_cy2,_dmc?0x2A3A5A:0x4A5A6A,"Built with love");_cy2+=20;\
        }else if(set_cat==5){\
            gfx_print(_cx,_cy2,0xFF6644,"Parental Control");_cy2+=24;\
            draw_toggle(_cx,_cy2,set_state&16);\
            gfx_print(_cx+32,_cy2-1,set_state&16?0xFF6644:0x8A9ACE,"App Lock");_cy2+=28;\
            draw_toggle(_cx,_cy2,set_state&32);\
            gfx_print(_cx+32,_cy2-1,set_state&32?0xFF6644:0x8A9ACE,"Website Filter");_cy2+=28;\
            draw_toggle(_cx,_cy2,set_state&64);\
            gfx_print(_cx+32,_cy2-1,set_state&64?0xFF6644:0x8A9ACE,"Time Limits");_cy2+=28;\
            draw_toggle(_cx,_cy2,set_state&128);\
            gfx_print(_cx+32,_cy2-1,set_state&128?0xFF6644:0x8A9ACE,"Content Rating");_cy2+=28;\
        }\
        gfx_print(_sx+8,_sy+_sh-16,_dmc?0x18182A:0x2A3A5A,"[U/D] nav  [1-8] toggle  [Esc] close");\
    } while(0)

    #define MAX_SEARCH 25
    const char *sitems[MAX_SEARCH] = {"This PC","Network","Terminal","Settings","OreoAI","Calculator","Notes","App Store","Kairo Studio","KairoVM","Camera","Kairo Player","True Video","Calendar","Pomodoro","Weather","Disk Usage","ASCII Art","Typing Test","Clipboard","File Manager","Tetris","Kairo Games","Snake","Web Browser"};
    int saction[MAX_SEARCH] = {1,2,3,4,5,6,7,8,9,10,11,12,14,16,17,18,19,20,21,22,23,24,25,26,35};

    #define APP_COUNT 26
    const char *app_names[APP_COUNT] = {
        "Kairo Studio","Terminal","Calculator","Notes","OreoAI",
        "File Manager","Image Viewer","Text Editor","Settings",
        "System Monitor","Code Compiler","Web Browser","Kairo Player",
        "Music Player","True Video","Clock","KairoVM","Camera",
        "Calendar","Pomodoro","Weather","Disk Usage","ASCII Art","Typing Test","Tetris",
        "Net Browser"
    };
    const char *app_cats[APP_COUNT] = {
        "Development","System","Utilities","Productivity","AI",
        "System","Multimedia","Productivity","System",
        "System","Development","Internet","Multimedia",
        "Multimedia","Multimedia","Utilities","Virtualization","Multimedia",
        "Productivity","Productivity","Utilities","System","Entertainment","Productivity","Game",
        "Internet"
    };
    uint32_t app_colors[APP_COUNT] = {
        0x6A5ACD,0x00CC44,0x4A9EFF,0xFFAA00,0xBB88FF,
        0x4A7AFF,0xFF8844,0x88AACC,0x8A8A9A,
        0x44CCAA,0xFF6644,0x44AAFF,0xCC4466,
        0xFF66AA,0xFF4444,0x88AACC,0xCC4444,0x66DDFF,
        0xDD8844,0xFF5544,0x44CCDD,0x44FFAA,0xFF88CC,0x88CC44,0x44DDFF,
        0x44AAFF
    };
    // Clipboard app (stored separately)

    #define _notes_open() do {\
        gfx_fill_round_rect(wx+16,wy+50,ww-32,20,4,0x12124A);gfx_print(wx+24,wy+53,0x4A9EFF,"My Notes");\
        for(int _ni=0;_ni<note_count&&_ni<8;_ni++){char _ns[4];_ns[0]='0'+(_ni+1)%10;_ns[1]='.';_ns[2]=' ';_ns[3]=0;gfx_print(wx+20,wy+78+_ni*16,0x8899CC,_ns);char _nt[44];note_short(_ni,_nt);gfx_print(wx+40,wy+78+_ni*16,0x6A8ABE,_nt);}\
        gfx_rect(wx+12,wy+210,ww-24,1,0x2A3A6A);gfx_print(wx+16,wy+218,0x4A6A8A,"> ");print_note_buf();\
        gfx_print(wx+16,wy+248,0x3A4A6A,"[Enter] save  [Esc] back");\
    } while(0)
    #define _store_open() do {\
        gfx_fill_round_rect(wx+16,wy+50,ww-32,20,4,0x12124A);gfx_print(wx+24,wy+53,0x4A9EFF,"Available Apps");\
        for(int _asi=0;_asi<APP_COUNT&&_asi<10;_asi++){int _asy=wy+78+_asi*20;\
            gfx_fill_round_rect(wx+16,_asy-2,ww-32,18,3,app_colors[_asi]);gfx_rect(wx+16,_asy-2,4,18,app_colors[_asi]);\
            gfx_print(wx+28,_asy+1,0xFFFFFF,app_names[_asi]);gfx_print(wx+180,_asy+1,0x8080AA,app_cats[_asi]);\
            if(apps_installed[_asi]){gfx_print(wx+300,_asy+1,0x44FF44,"[Installed]");}\
            else{gfx_print(wx+300,_asy+1,0x808080,"[ ");char _ak[2];_ak[0]='0'+_asi%10;_ak[1]=0;gfx_print(wx+312,_asy+1,0xFFAA00,_ak);gfx_print(wx+324,_asy+1,0x808080," ]");}\
        }gfx_print(wx+16,wy+240,0x3A4A6A,"[0-9] install/uninstall  [Esc] back");\
    } while(0)
    #define _vm_open() do {\
        vm_mode=0;\
        gfx_fill_round_rect(wx+16,wy+50,ww-32,20,4,0x12124A);gfx_print(wx+24,wy+53,0xFF6644,"KairoVM - Virtual Machine Manager");\
        if(vm_count==0){gfx_print(wx+60,wy+100,0x6A7A9E,"No virtual machines yet.");gfx_print(wx+60,wy+124,0x4A9EFF,"Press [c] to create one.");}\
        else{\
            gfx_print(wx+16,wy+76,0x8A8A9A,"Name                  OS                    RAM   CPUs  Disk  Status");\
            gfx_rect(wx+16,wy+92,ww-32,1,0x2A2A4A);\
            for(int _vi=0;_vi<vm_count&&_vi<4;_vi++){\
                int _vy=wy+96+_vi*40;\
                if(_vi==vm_sel){gfx_fill_round_rect(wx+14,_vy-2,ww-28,36,4,0x1A1A4A);}\
                char _vs[2];_vs[0]='0'+(_vi+1)%10;_vs[1]=0;gfx_print(wx+20,_vy+2,0x8899CC,_vs);gfx_print(wx+40,_vy+2,0xFFFFFF,vm_name[_vi]);\
                gfx_print(wx+190,_vy+2,vm_os_color[vm_os[_vi]],vm_os_name[vm_os[_vi]]);\
                char _vr[8];int _ri=0,_rn=vm_ram[_vi];do{_vr[_ri++]='0'+_rn%10;_rn/=10;}while(_rn);_vr[_ri]=0;\
                for(int _rk=0;_rk<_ri/2;_rk++){char _rt=_vr[_rk];_vr[_rk]=_vr[_ri-1-_rk];_vr[_ri-1-_rk]=_rt;}\
                gfx_print(wx+310,_vy+2,0x88AACC,_vr);gfx_print(wx+336,_vy+2,0x6A7A9E,"MB");\
                char _vc[2];_vc[0]='0'+vm_cores[_vi]%10;_vc[1]=0;gfx_print(wx+370,_vy+2,0x88AACC,_vc);\
                char _vd[8];int _di=0,_dn=vm_disk[_vi];do{_vd[_di++]='0'+_dn%10;_dn/=10;}while(_dn);_vd[_di]=0;\
                for(int _dk=0;_dk<_di/2;_dk++){char _dt=_vd[_dk];_vd[_dk]=_vd[_di-1-_dk];_vd[_di-1-_dk]=_dt;}\
                gfx_print(wx+400,_vy+2,0x88AACC,_vd);gfx_print(wx+420,_vy+2,0x6A7A9E,"GB");\
                if(vm_running[_vi]){gfx_print(wx+450,_vy+2,0x44FF44,"Running");}else{gfx_print(wx+450,_vy+2,0x808080,"Stopped");}\
            }gfx_print(wx+16,wy+260,0x3A4A6A,"[Up/Down] select  [Enter] start/stop  [c] create  [d] delete");\
        }\
    } while(0)
    // Tool: open app by action ID (used by Command Palette)
    #define _launch_app(_act) do {\
        if(_act==1){open_app(1);draw_mac_title("This PC");\
            gfx_fill_round_rect(wx+16,wy+50,ww-32,20,4,0x12124A);gfx_print(wx+24,wy+53,0x4A9EFF,"System Information");\
            gfx_print(wx+24,wy+84,0x8A9ACE,"Kernel:    Viteza v1.0");\
            gfx_print(wx+24,wy+102,0x8A9ACE,"CPU:       x86_64 Long Mode");\
            gfx_print(wx+24,wy+120,0x8A9ACE,"RAM:       256 MB");\
            gfx_print(wx+24,wy+138,0x8A9ACE,"Display:   1024x768");\
            gfx_print(wx+24,wy+162,0x6A7A9E,"No drives detected");\
            int _udc=usb_device_count();\
            if(_udc>0){gfx_print(wx+24,wy+184,0x4A9EFF,"USB Devices:");for(int _uj=0;_uj<_udc&&_uj<2;_uj++)gfx_print(wx+24,wy+204+_uj*16,0x6A8ABE,usb_device_name(_uj));}\
            else{gfx_print(wx+24,wy+184,0x6A7A9E,"No USB devices");}\
            gfx_print(wx+24,wy+240,0x3A4A6A,"[Esc] to close");\
        }else if(_act==2){open_app(2);draw_mac_title("Network");\
            gfx_fill_round_rect(wx+16,wy+50,ww-32,20,4,0x12124A);gfx_print(wx+24,wy+53,0x4A9EFF,"Wi-Fi Status");\
            gfx_print(wx+24,wy+84,0x8A9ACE,"SSID:     HOME-5G");\
            gfx_print(wx+24,wy+102,0x8A9ACE,"Signal:   Excellent");\
            gfx_print(wx+24,wy+120,0x8A9ACE,"Security: WPA2-PSK");\
            gfx_print(wx+24,wy+144,0x6A7A9E,"IP: 0.0.0.0 (pending)");\
            gfx_print(wx+24,wy+220,0x3A4A6A,"[Esc] to close");\
        }else if(_act==3){open_app(3);draw_mac_title("Terminal");\
            term_line_count=0;term_scroll=0;\
            term_add("[Viteza Terminal v1.0]",0);term_add("Type 'help' for commands.",0);term_redraw();\
        }else if(_act==4){settings_active=1;set_cat=0;settings_redraw();\
        }else if(_act==5){open_app(5);draw_mac_title("OreoAI Assistant");\
            chat_line_count=0;chat_scroll=0;chat_pos=0;\
            chat_add("[OreoAI v1.0 - Ask me anything!]");chat_add("Try: hello, who are you, help");chat_redraw();\
        }else if(_act==6){open_app(6);draw_mac_title("Calculator");\
            calc_val=0;calc_cur=0;calc_op=0;calc_state=0;calc_redraw();\
        }else if(_act==7){open_app(7);draw_mac_title("Notes");_notes_open();\
        }else if(_act==8){open_app(8);draw_mac_title("App Store");_store_open();\
        }else if(_act==9){open_app(9);draw_mac_title("Kairo Studio");\
        }else if(_act==10){open_app(10);draw_mac_title("KairoVM");_vm_open();\
        }else if(_act==11){open_app(11);draw_mac_title("Camera");gfx_print(wx+16,wy+44,0x4A9EFF,"Camera - Hardware Status");\
        }else if(_act==12){open_app(12);draw_mac_title("Kairo Player");\
            gfx_fill_round_rect(wx+20,wy+48,ww-40,130,4,0x000000);gfx_round_rect(wx+20,wy+48,ww-40,130,4,0x3A5A8A);\
            gfx_print(wx+28,wy+56,0xFFFFFF,"Kairo Visual Engine");\
            for(int _vy=0;_vy<100;_vy++)for(int _vx=0;_vx<320;_vx++){int _vc=((_vx*5)^(_vy*7))&0xFF;gfx_putpixel(wx+30+_vx,wy+70+_vy,(_vc<<16)|(_vc<<8)|_vc);}\
            gfx_round_rect(wx+30,wy+70,320,100,2,0x4A6ADF);gfx_print(wx+ww-130,wy+155,0x00E5FF,"Kairo Audio");\
            gfx_print(wx+ww-130,wy+163,0x3A5A8A,"Ready");\
            gfx_print(wx+24,wy+196,0x8A9ACE,"[P] Play  [S] Stop");\
            gfx_print(wx+20,wy+wh-18,0x3A4A6A,"Powered by Kairo Visual & Kairo Audio");\
        }else if(_act==14){open_app(14);draw_mac_title("True Video");\
        }else if(_act==16){open_app(16);draw_mac_title("Calendar");\
        }else if(_act==17){open_app(17);draw_mac_title("Pomodoro Timer");pom_sec=pom_total;pom_running=0;pom_ticks=0;\
        }else if(_act==18){open_app(18);draw_mac_title("Weather");wthr_first=1;\
        }else if(_act==19){open_app(19);draw_mac_title("System Monitor");\
        }else if(_act==20){open_app(20);draw_mac_title("ASCII Art Gallery");art_sel=0;\
        }else if(_act==21){open_app(21);draw_mac_title("Typing Test");type_pos=0;type_err=0;type_ok=0;type_start=0;type_done=0;\
        }else if(_act==22){open_app(22);draw_mac_title("Clipboard");clip_save_pos=0;clip_save[0]=0;\
        }else if(_act==23){open_app(23);draw_mac_title("File Manager");fm_sel=0;fm_scroll=0;\
        }else if(_act==24){open_app(24);draw_mac_title("Tetris");\
            tet_score=0;tet_lines=0;tet_gameover=0;tet_drop=0;\
            for(int _ty=0;_ty<18;_ty++)for(int _tx=0;_tx<10;_tx++)tet_grid[_tx][_ty]=0;\
            tet_next=((_tick*7+_tick/3)%7+7)%7;tet_piece=((_tick*5+_tick/2)%7+7)%7;tet_rot=0;tet_x=3;tet_y=0;\
        }else if(_act==25){open_app(25);draw_mac_title("Kairo Games");\
        }else if(_act==26){open_app(26);draw_mac_title("Snake");\
            snake_len=3;snake_dir=0;snake_score=0;snake_gameover=0;snake_drop=0;\
            snake_body[0][0]=5;snake_body[0][1]=9;\
            snake_body[1][0]=4;snake_body[1][1]=9;\
            snake_body[2][0]=3;snake_body[2][1]=9;\
            snake_food_x=8;snake_food_y=8;\
        }else if(_act==28){open_app(28);draw_mac_title("Viteza Wii");_wii_boot=1;\
        }else if(_act==29){ac97_start_capture();open_app(29);draw_mac_title("Mic Test");\
        }else if(_act==30){open_app(30);draw_mac_title("Pong");\
        }else if(_act==31){open_app(31);draw_mac_title("Paint");\
        }else if(_act==32){open_app(32);draw_mac_title("Maze");\
        }else if(_act==33){open_app(33);draw_mac_title("Music Player");\
        }else if(_act==34){open_app(34);draw_mac_title("Dino Runner");\
        }else if(_act==36){open_app(36);draw_mac_title("Kairo Chat");\
        }else if(_act==35){open_app(35);ww=w-80;wh=h-80;wx=w/2-(w-80)/2;wy=h/2-(h-80)/2+8;draw_mac_title("KairoWeb");\
            br_url[0]=0;br_pos=0;br_line_count=0;br_scroll=0;br_fetching=0;br_focus=1;\
            kw_url[0]=0; kw_scroll=0;\
            kwseg_t *_h = &kw_segs[0]; _h->n=0; _h->style=0; _h->href[0]=0;\
            { const char *_ht = "Benvenuto in KairoWeb!\n\nDigita un indirizzo nella barra qui sopra e premi Invio.\nSuggerimento: http://example.com\n\n\nQuesta GUI browser usa il client HTTP/1.0 del kernel.\n";\
                for (int _qj=0;_ht[_qj]&&_h->n<590;_qj++){ _h->text[_h->n++]=_ht[_qj]; } _h->text[_h->n]=0; \
            }\
            kw_seg_count=1;\
            kw_render(wx,wy,ww,wh,"",1);\
        }else if(_act==37){open_app(37);draw_mac_title("BootLoader Command Line");\
            bl_open();bl_redraw();\
        }else if(_act==38){open_app(38);draw_mac_title("Syslog");syslog_open();syslog_redraw();\
        }else if(_act==39){open_app(39);draw_mac_title("Phone");phone_pos=0;phone_num[0]=0;phone_sel=0;phone_calling=0;voip_state=VOIP_IDLE;udp_set_callback(voip_udp_callback);voip_ssrc=(_tick*7+0xAABBCCDD);phone_redraw(set_state&1);\
        }\
    } while(0)

    // Helper to close window (restore macOS wallpaper background + stars)
     #define close_win() do {\
        win=0;win_type=0;\
        int _cx1=wx,_cy1=wy,_cx2=wx+ww+8,_cy2=wy+wh+8;\
        int _dmc = set_state & 1;\
        if(_cx2>w){_cx2=w;}if(_cy2>h){_cy2=h;}\
        for(int _y=_cy1;_y<_cy2;_y++){\
            int _t=_y*255/h,_r,_g,_b;\
            if(_dmc){\
                _r=2+_t/60;_g=1+_t/40;_b=8+_t/30;\
                if(_y<180){int _f=180-_y;_r=4+_f/40;_g=2+_f/60;_b=12+_f/15;}\
                if(_r>8){_r=8;}if(_g>6){_g=6;}if(_b>20){_b=20;}\
            }else{\
                _r=8+_t/30;_g=6+_t/20;_b=24+_t/10;\
                if(_y<180){int _f=180-_y;_r=18+_f/12;_g=10+_f/20;_b=50+_f/6;}\
                if(_r>32){_r=32;}if(_g>28){_g=28;}if(_b>60){_b=60;}\
            }\
            gfx_rect(_cx1,_y,_cx2-_cx1,1,(_r<<16)|(_g<<8)|_b);\
        }\
        for(int _i=0;_i<60;_i++){\
            int _sx=(_i*691+47)%w,_sy=(_i*983+19)%(h*3/5);\
            if(_sx>=_cx1&&_sx<_cx2&&_sy>=_cy1&&_sy<_cy2){\
                int _sb=(_i*257+13)%6;if(_sb<2)continue;\
                uint32_t _sc=_dmc?0x224466:0x8899CC;\
                gfx_fill_round_rect(_sx-1,_sy-1,3,3,1,_sc);\
            }\
        }\
    } while(0)

    // Truncate note for display in app list (max 38 chars)
    #define note_short(_i,_buf) do {\
        int _np;for(_np=0;notes[_i][_np]&&_np<38;_np++)_buf[_np]=notes[_i][_np];\
        if(notes[_i][_np]){_buf[_np++]='.';_buf[_np++]='.';_buf[_np++]='.';}_buf[_np]=0;\
    } while(0)

    // Truncate input buffer for display (max 42 chars)
    #define print_note_buf() do {\
        char _nb[48];int _nbi;for(_nbi=0;note_buf[_nbi]&&_nbi<42;_nbi++)_nb[_nbi]=note_buf[_nbi];_nb[_nbi]=0;\
        gfx_rect(wx+28,wy+218,340,10,0x141452);\
        gfx_print(wx+28,wy+218,0x88AACC,_nb);\
    } while(0)

    // Notes widget — desktop panel with badge + content + arrows
    int nw_x = 30, nw_y = 90, nw_w = 210, nw_h = 140;

    #define note_widget_draw() do {\
        int _dmc = set_state & 1;\
        gfx_fill_round_rect(nw_x,nw_y,nw_w,nw_h,8,_dmc?0x040410:0x080820);\
        gfx_round_rect(nw_x,nw_y,nw_w,nw_h,8,_dmc?0x18183A:0x2A3A6A);\
        gfx_rect(nw_x+10,nw_y+26,nw_w-20,1,_dmc?0x0E0E2A:0x1A2A5A);\
        gfx_print(nw_x+12,nw_y+5,_dmc?0x4A5A7E:0x6A8ABE,"Notes");\
        gfx_fill_round_rect(nw_x+nw_w-30,nw_y+4,18,14,7,0xCC4444);\
        if(note_count<10){char _nb[2];_nb[0]='0'+note_count;_nb[1]=0;gfx_print(nw_x+nw_w-26,nw_y+4,0xFFFFFF,_nb);}\
        else{char _nb[3];_nb[0]='0'+note_count/10;_nb[1]='0'+note_count%10;_nb[2]=0;gfx_print(nw_x+nw_w-26,nw_y+4,0xFFFFFF,_nb);}\
        if(note_count>0&&note_sel<note_count){\
            char _nn[26];int _ni;for(_ni=0;notes[note_sel][_ni]&&_ni<21;_ni++)_nn[_ni]=notes[note_sel][_ni];\
            if(notes[note_sel][_ni]){_nn[_ni++]='.';_nn[_ni++]='.';_nn[_ni++]='.';}_nn[_ni]=0;\
            gfx_print(nw_x+12,nw_y+30,_dmc?0x6677AA:0x8899CC,_nn);\
            gfx_print(nw_x+10,nw_y+nw_h-18,_dmc?0x2A3A5A:0x4A6A8A,"<");\
            if(note_sel+1<10){char _np[3];_np[0]='0'+(note_sel+1);_np[1]='/';_np[2]=0;gfx_print(nw_x+80,nw_y+nw_h-20,_dmc?0x3A4A6A:0x5A6A8A,_np);}\
            else{char _np[4];_np[0]='0'+(note_sel+1)/10;_np[1]='0'+(note_sel+1)%10;_np[2]='/';_np[3]=0;gfx_print(nw_x+76,nw_y+nw_h-20,_dmc?0x3A4A6A:0x5A6A8A,_np);}\
            if(note_count<10){char _nt[2];_nt[0]='0'+note_count;_nt[1]=0;gfx_print(nw_x+92,nw_y+nw_h-20,_dmc?0x3A4A6A:0x5A6A8A,_nt);}\
            else{char _nt[3];_nt[0]='0'+note_count/10;_nt[1]='0'+note_count%10;_nt[2]=0;gfx_print(nw_x+92,nw_y+nw_h-20,_dmc?0x3A4A6A:0x5A6A8A,_nt);}\
            gfx_print(nw_x+nw_w-22,nw_y+nw_h-18,_dmc?0x2A3A5A:0x4A6A8A,">");\
        }else{\
            gfx_print(nw_x+12,nw_y+36,_dmc?0x1A2A4A:0x3A4A6A,"No notes  [7]");\
        }\
    } while(0)

    // Window chrome — nuovo renderer font8x8 (ui/renderer/renderer.c)
    // Regola finestra: sfondo bianco → componenti binari dentro la finestra in altri colori
    #define draw_mac_title(k_) do {\
        uint32_t _tb, _bb, _tt;\
        if (_natal_mode) { _tb = 0x1A0404; _bb = 0xE8E0C8; _tt = 0x2A0404; }\
        else { _tb = 0x2D2D34; _bb = 0xDCDCE0; _tt = 0xFFFFFF; }\
        renderer_draw_window(wx, wy, ww, wh, (k_), _tb, _bb, _tt);\
        g_win_title = (k_);\
        g_win.x = wx; g_win.y = wy; g_win.w = ww; g_win.h = wh;\
    } while(0)

     #define open_app(n) do {\
        if (!g_win_init) { wx = w/2-200; wy = h/2-160; g_win_init = 1; }\
        ww = 400; wh = 280;\
        g_win.x = wx; g_win.y = wy; g_win.w = ww; g_win.h = wh;\
        win=1;win_type=n;\
        int _dmc = set_state & 1;\
        /* Set app name for menu bar */\
        switch(n){ case 3:_app_name="Terminal";break; case 4:_app_name="Settings";break; case 5:_app_name="OreoAI";break; case 6:_app_name="Calculator";break; case 7:_app_name="Notes";break; case 8:_app_name="Store";break; case 9:_app_name="Studio";break; case 10:_app_name="KairoVM";break; case 11:_app_name="Camera";break; case 12:_app_name="Player";break; case 14:_app_name="TrueVideo";break; case 16:_app_name="Calendar";break; case 17:_app_name="Pomodoro";break; case 18:_app_name="Weather";break; case 19:_app_name="Monitor";break; case 20:_app_name="Art";break; case 21:_app_name="Typing";break; case 22:_app_name="Clipboard";break; case 23:_app_name="Files";break; case 24:_app_name="Tetris";break; case 25:_app_name="Games";break; case 26:_app_name="Snake";break; case 28:_app_name="Wii";break; case 29:_app_name="Mic Test";break; case 30:_app_name="Pong";break; case 31:_app_name="Paint";break; case 32:_app_name="Maze";break;         case 33:_app_name="Music";break; case 34:_app_name="Dino";break; case 35:_app_name="Browser";break; case 36:_app_name="Chat";break;          case 37:_app_name="BootLoader";break; case 38:_app_name="Syslog";break; case 39:_app_name="Phone";break;\
        }\
        /* Update menu bar app name */\
        gfx_rect(83,4,140,16,_dmc?0x06060E:0x0A0A1C);\
        gfx_rect(85,6,2,12,_dmc?0x1A1A3A:0x2A2A5A);\
        gfx_print_shadow(92,5,_dmc?0x8899CC:0xAABBEE,_app_name);\
    } while(0)

    // Restore wallpaper over a rect (used when dragging windows)
    // Covers shadow (6px all sides) + title bar (36px above) + 8px padding
    #define restore_win_area(px,py,pw,ph) do {\
        int _cx1=(px)-8,_cy1=(py)-40,_cx2=(px)+(pw)+10,_cy2=(py)+(ph)+10;\
        if(_cx2>w){_cx2=w;}if(_cy2>h){_cy2=h;}\
        if(_cx1<0)_cx1=0;if(_cy1<0)_cy1=0;\
        volatile uint32_t *_rfb=gfx_get_fb_addr();\
        int _rpitch=gfx_get_pitch()/4;\
        for(int _ry=_cy1;_ry<_cy2;_ry++){\
            for(int _rx=_cx1;_rx<_cx2;_rx++){\
                if(_rx>=0&&_rx<1280&&_ry>=0&&_ry<720)\
                    _rfb[_ry*_rpitch+_rx]=wallpaper_data[_ry*1280+_rx];\
            }\
        }\
    } while(0)

    // Ridisegna corpo+ombra finestra senza animazione (per il drag)
    #define open_app_redraw() do {\
        int _dmc = set_state & 1;\
        gfx_rect(83,4,140,16,_dmc?0x06060E:0x0A0A1C);\
        gfx_rect(85,6,2,12,_dmc?0x1A1A3A:0x2A2A5A);\
        gfx_print_shadow(92,5,_dmc?0x8899CC:0xAABBEE,_app_name);\
    } while(0)

    // Ridisegna il contenuto dell'app dopo uno spostamento
    #define win_content_redraw() do {\
        if (win_type == 3) term_redraw();\
        else if (win_type == 5 || win_type == 36) chat_redraw();\
        else if (win_type == 6) calc_redraw();\
        else if (win_type == 7) _notes_open();\
        else if (win_type == 8) _store_open();\
        else if (win_type == 10) vm_redraw();\
        else if (win_type == 35) br_redraw();\
        else if (win_type == 37) bl_redraw();\
        else if (win_type == 38) syslog_redraw();\
        else if (win_type == 39) phone_redraw(set_state&1);\
    } while(0)

    // Add a wrapped terminal line (max 38 chars per line, properly multi-line)
    #define term_add(fmt,lbl) do {\
        if(term_line_count<60){\
            const char *_cp = fmt;\
            while(*_cp && term_line_count<60){\
                int _n; for(_n=0;_cp[_n];_n++);\
                int _take = _n;\
                if(_take > 38){\
                    _take = 38; while(_take>0 && _cp[_take]!=' ')_take--;\
                    if(_take<1)_take=38;\
                }\
                int _i; for(_i=0;_i<_take;_i++)term_lines[term_line_count][_i]=_cp[_i];\
                term_lines[term_line_count][_i]=0;\
                term_line_count++;\
                _cp += _take;\
                if(*_cp==' ')_cp++;\
            }\
            if(term_scroll==term_line_count-1||term_scroll==term_line_count-2)term_scroll=term_line_count-1;\
            if(term_scroll>term_line_count-1)term_scroll=term_line_count-1;\
        }\
    } while(0)

    // Add a wrapped chat line (max 38 chars per line, properly multi-line)
    #define chat_add(fmt) do {\
        if(chat_line_count<60){\
            const char *_cp = fmt;\
            while(*_cp && chat_line_count<60){\
                int _n; for(_n=0;_cp[_n];_n++);\
                int _take = _n;\
                if(_take > 38){\
                    _take = 38; while(_take>0 && _cp[_take]!=' ')_take--;\
                    if(_take<1)_take=38;\
                }\
                int _i; for(_i=0;_i<_take;_i++)chat_lines[chat_line_count][_i]=_cp[_i];\
                chat_lines[chat_line_count][_i]=0;\
                chat_line_count++;\
                _cp += _take;\
                if(*_cp==' ')_cp++;\
            }\
            if(chat_scroll==chat_line_count-1||chat_scroll==chat_line_count-2)chat_scroll=chat_line_count-1;\
            if(chat_scroll>chat_line_count-1)chat_scroll=chat_line_count-1;\
        }\
    } while(0)

    // Redraw terminal window — green-on-black hacker style
    #define term_redraw() do {\
        if (_natal_mode) {\
            gfx_fill_round_rect(wx+8,wy+44,ww-16,wh-56,6,0x080400);\
            gfx_round_rect(wx+8,wy+44,ww-16,wh-56,6,0x660000);\
        } else {\
            gfx_fill_round_rect(wx+8,wy+44,ww-16,wh-56,6,0x050508);\
            gfx_round_rect(wx+8,wy+44,ww-16,wh-56,6,0x0A3A0A);\
        }\
        /* header terminal */\
        if (_natal_mode) {\
            gfx_fill_round_rect(wx+12,wy+48,7,7,3,0xFF4444);\
            gfx_print(wx+24,wy+46,0xFF6644,"Terminal");\
            gfx_print(wx+ww-70,wy+46,0x88AA44,"user@viteza");\
            gfx_rect(wx+8,wy+57,ww-16,1,0x442200);\
        } else {\
            gfx_fill_round_rect(wx+12,wy+48,7,7,3,0x00CC44);\
            gfx_print(wx+24,wy+46,0x00CC44,"Terminal");\
            gfx_print(wx+ww-70,wy+46,0x0A5A2A,"user@viteza:~");\
            gfx_rect(wx+8,wy+57,ww-16,1,0x0A2A0A);\
        }\
        char _pb[80]; int _pi,_pn=0;\
        char _pfix[40]; int _pf=0;\
        {char _ws[]="user@viteza:~$ ";for(_pf=0;_ws[_pf];_pf++)_pfix[_pf]=_ws[_pf];_pfix[_pf]=0;}\
        for(_pi=0;_pfix[_pi];_pi++)_pb[_pn++]=_pfix[_pi];\
        for(_pi=0;_pi<term_pos&&_pi<44;_pi++)_pb[_pn++]=term_buf[_pi];\
        _pb[_pn]=0;\
        int _tprow = wy+44+wh-56-20;\
        gfx_print(wx+16,_tprow,_natal_mode?0xFF4444:0x00FF44,_pb);\
        /* caret lampeggiante */\
        if((_tick/30)%2==0){\
            int _cxw = renderer_text_width(_pb,1);\
            gfx_rect(wx+17+_cxw,_tprow+2,8,14,0xFFFFFF);\
        }\
        int _ml = (wh-72)/20 - 1; if(_ml<1) _ml=1;\
        int _start = term_scroll - _ml + 1; if(_start<0) _start=0;\
        int _ty = wy+62;\
        for(int _i=_start; _i<=term_scroll && _i<term_line_count; _i++){\
            uint32_t _tc = _natal_mode?0xFF6644:0x00CC44;\
            if(term_lines[_i][0]=='['){_tc=_natal_mode?0x44FF44:0x00AA44;}if(term_lines[_i][0]=='#'){_tc=0xFF4444;}\
            if(term_lines[_i][0]=='%'){_tc=0xFFFF44;}\
            if(_natal_mode&&_i==term_line_count-1&&term_lines[_i][0]==0xE2){_tc=0x44FF44;}\
            gfx_print(wx+16,_ty,_tc,term_lines[_i]); _ty+=20;\
        }\
    } while(0)

    // KairoWeb (GUI browser)
    #define br_add(fmt) do { (void)(fmt); } while(0)
    #define br_redraw() do { \
        char _btmp[160]; int _bti; \
        if (br_focus) { int _bn=0; for(_bti=0;_bti<br_pos&&_bti<126;_bti++) _btmp[_bn++]=br_url[_bti]; _btmp[_bn]=0; } \
        else { int _bn=0; for(_bti=0;kw_url[_bti]&&_bti<126;_bti++) _btmp[_bn++]=kw_url[_bti]; _btmp[_bn]=0; } \
        kw_render(wx, wy, ww, wh, _btmp, br_focus); \
    } while(0)

    // Redraw chat window — bubble-style
    #define chat_redraw() do {\
        gfx_fill_round_rect(wx+8,wy+44,ww-16,wh-56,6,0x08081C);\
        gfx_round_rect(wx+8,wy+44,ww-16,wh-56,6,0x1A1A4E);\
        /* header con titolo e stato */\
        gfx_rect(wx+8,wy+52,ww-16,1,0x2A2A6E);\
        gfx_fill_round_rect(wx+14,wy+47,7,7,3,0x00E5FF);\
        gfx_print(wx+26,wy+45,0x66AAFF,"OreoAI Assistant");\
        gfx_print(wx+ww-90,wy+45,0x3A5A8A,"online");\
        int _ml = (wh-72)/16 - 1; if(_ml<1) _ml=1;\
        int _start = chat_scroll - _ml + 1; if(_start<0) _start=0;\
        int _cy = wy+60;\
        for(int _i=_start; _i<=chat_scroll && _i<chat_line_count; _i++){\
            char *_cl = chat_lines[_i];\
            uint32_t _cc = TEXT;\
            if(_cl[0]=='Y'){ _cc=0x00E5FF; } \
            else if(_cl[0]=='O'){ _cc=0xBB88FF; } \
            else if(_cl[0]=='['){ _cc=0x8888CC; }\
            gfx_print(wx+16,_cy,_cc,_cl); _cy+=16;\
        }\
        char _cb[80]; int _ci,_cn=0;\
        char _cfx[]="OreoAI> ";\
        for(_ci=0;_cfx[_ci];_ci++)_cb[_cn++]=_cfx[_ci];\
        for(_ci=0;_ci<chat_pos&&_ci<36;_ci++)_cb[_cn++]=chat_buf[_ci];\
        _cb[_cn]=0;\
        /* barra input definita */\
        int _iby = wy+44+wh-56-16;\
        gfx_fill_round_rect(wx+12,_iby-3,ww-60,24,4,0x0A0A28);\
        gfx_round_rect(wx+12,_iby-3,ww-60,24,4,0x2A2A5A);\
        gfx_print(wx+16,_iby,0xBB88FF,_cb);\
        /* Mic button + STT recorder UI */\
        if (stt_active) {\
            int _riy = wy+44+wh-56-16-24;\
            gfx_fill_round_rect(wx+16,_riy,ww-32,20,4,0x220000);\
            gfx_round_rect(wx+16,_riy,ww-32,20,4,0xFF4444);\
            gfx_print(wx+22,_riy+2,0xFF6666, stt_train ? "● TRAIN — ripeti la frase scritta qui" : "● REC");\
            int _bars = 12; int _bw = 6, _gap = 5;\
            int _bx = wx + ww - 40 - (_bars*(_bw+_gap));\
            int _step = (stt_ticks/8) % _bars;\
            for (int _bi=0;_bi<_bars;_bi++){\
                int _bh = 4 + ((_bi*7 + stt_ticks/4) % 13);\
                uint32_t _bc = (_bi==_step) ? 0xFFAA44 : 0x88FF88;\
                gfx_rect(_bx+_bi*(_bw+_gap), _riy+16-_bh, _bw, _bh, _bc);\
            }\
            gfx_print(wx+22, _riy+22, 0x3A5A6A, "[Invio] stop  [Esc] annulla");\
        } else {\
            int _mby = wy+44+wh-56-18;\
            int _mbx = wx+ww-42;\
            gfx_fill_round_rect(_mbx,_mby,30,20,6, stt_train?0x4A2A0A:0x1A1A4A);\
            gfx_round_rect(_mbx,_mby,30,20,6, stt_train?0xFFAA44:0x3A5A8A);\
            gfx_fill_round_rect(_mbx+11,_mby+2,8,10,4, stt_train?0xFFAA44:0x88BBFF);\
            gfx_rect(_mbx+6,_mby+11,18,2, stt_train?0xFFAA44:0x88BBFF);\
            gfx_print(_mbx-1, _mby+21, 0x3A4A6A, stt_train?"[Ctrl+T]":"[Ctrl+K]");\
        }\
    } while(0)

    // Invia il contenuto di chat_buf a OreoAI via proxy seriale
    #define chat_submit() do {\
        chat_buf[chat_pos] = 0;\
        if (chat_pos > 0) {\
            chat_add(""); /* blank to separate */\
            char _tmp[80]; int _ti;\
            for(_ti=0;chat_buf[_ti];_ti++) _tmp[_ti]=chat_buf[_ti];\
            _tmp[_ti]=0;\
            chat_add(_tmp);\
            int _got = 0;\
            while (serial_available()) serial_read();\
            serial_puts("AI|");\
            serial_puts(_tmp);\
            serial_write('\n');\
            for (int _tw = 0; _tw < 30000; _tw++) {\
                if (serial_available()) break;\
                for (volatile int _d = 0; _d < 300000; _d++);\
            }\
            if (serial_available()) {\
                char _rline[200];\
                while (1) {\
                    int _ri = 0;\
                    while (_ri < 199) {\
                        if (!serial_read_timeout(&_rline[_ri], 5000)) break;\
                        if (_rline[_ri] == '\n' || _rline[_ri] == '\r') break;\
                        _ri++;\
                    }\
                    _rline[_ri] = 0;\
                    if (_rline[0]=='E'&&_rline[1]=='N'&&_rline[2]=='D'&&_rline[3]==0) break;\
                    if (_rline[0]=='R'&&_rline[1]=='E'&&_rline[2]=='S'&&_rline[3]=='P'&&_rline[4]=='|') {\
                        char *_rt = _rline + 5;\
                        if (_rt[0]) { chat_add(_rt); _got = 1; }\
                    }\
                }\
            }\
            if (!_got) {\
                chat_add("[AI proxy not connected. Run: python3 social_proxy.py]");\
            }\
        }\
        chat_pos = 0; chat_buf[0] = 0;\
        chat_redraw();\
    } while(0)

#define calc_redraw() do {\
    int _cx=wx+16,_cy=wy+50,_cw=ww-32,_ch=wh-66;\
    gfx_fill_round_rect(_cx,_cy,_cw,_ch,8,0x0A0A22);\
    gfx_round_rect(_cx,_cy,_cw,_ch,8,0x3A5A9A);\
    int _ddy=_cy+8,_ddh=34;\
    gfx_fill_round_rect(_cx+8,_ddy,_cw-16,_ddh,6,0x0A0A16);\
    gfx_round_rect(_cx+8,_ddy,_cw-16,_ddh,6,0x4A6AAA);\
    gfx_rect(_cx+8,_ddy+_ddh/2,_cw-16,1,0x1A1A3A);\
    int _disp=(calc_cur!=0||!calc_state||!calc_op)?calc_cur:calc_val;\
    char _db[32];int _di=0,_dn=_disp<0?-_disp:_disp;\
    do{_db[_di++]='0'+_dn%10;_dn/=10;}while(_dn);\
    if(_disp<0)_db[_di++]='-';\
    _db[_di]=0;\
    for(int _rk=0;_rk<_di/2;_rk++){char _rt=_db[_rk];_db[_rk]=_db[_di-1-_rk];_db[_di-1-_rk]=_rt;}\
    gfx_print_scaled(_cx+_cw-16-_di*10,_ddy+10,0x44FF88,_db,1);\
    /* riga espressione/frase */\
    gfx_print(_cx+16,_ddy+4,0x3A4A6A,"Kairo Calc");\
    const char *_bl[16]={"7","8","9","/","4","5","6","*","1","2","3","-","C","0","=","+"};\
    int _bw=(_cw-32)/4,_bh=28,_by2=_ddy+_ddh+8;\
    for(int _bi=0;_bi<16;_bi++){\
        uint32_t _bcol=0x2A3A6A;\
        if(_bl[_bi][0]=='/'||_bl[_bi][0]=='*'||_bl[_bi][0]=='-')_bcol=0x2A4A8A;\
        if(_bl[_bi][0]=='+'||_bl[_bi][0]=='=')_bcol=0x3A7AFF;\
        if(_bl[_bi][0]=='C')_bcol=0xCC4444;\
        int _br=_bi/4,_bc=_bi%4;\
        int _bx=_cx+12+_bc*(_bw+4);\
        int _by3=_by2+_br*(_bh+4);\
        gfx_fill_round_rect(_bx+1,_by3+1,_bw,_bh,5,0x000000);\
        gfx_fill_round_rect(_bx,_by3,_bw,_bh,5,_bcol);\
        gfx_round_rect(_bx,_by3,_bw,_bh,5,(_bl[_bi][0]=='=')?0x8AB4FF:0x4A6ADF);\
        gfx_print(_bx+_bw/2-5,_by3+7,0xFFFFFF,_bl[_bi]);\
    }\
    gfx_print(_cx+8,_cy+_ch-14,0x2A3A5A,"[Keys] 0-9 + - * / Enter C Bksp");\
} while(0)

    // VM library redraw helper
    #define vm_redraw() do {\
        gfx_fill_round_rect(wx+8,wy+44,ww-16,wh-70,6,0x0A0A1A);\
        gfx_print(wx+24,wy+53,0xFF6644,"KairoVM — Virtual Machine Manager");\
        if(vm_count==0){\
            gfx_print(wx+60,wy+100,0x6A7A9E,"No virtual machines yet.");\
            gfx_print(wx+60,wy+124,0x4A9EFF,"Press [c] to create one.");\
        }else{\
            gfx_print(wx+16,wy+76,0x8A8A9A,"Name                  OS                    RAM   CPUs  Disk  Status");\
            gfx_rect(wx+16,wy+92,ww-32,1,0x2A2A4A);\
            for(int _vi=0;_vi<vm_count&&_vi<4;_vi++){\
                int _vy=wy+96+_vi*40;\
                if(_vi==vm_sel){gfx_fill_round_rect(wx+14,_vy-2,ww-28,36,4,0x1A1A4A);}\
                char _vs[2];_vs[0]='0'+(_vi+1)%10;_vs[1]=0;\
                gfx_print(wx+20,_vy+2,0x8899CC,_vs);gfx_print(wx+40,_vy+2,0xFFFFFF,vm_name[_vi]);\
                gfx_print(wx+190,_vy+2,vm_os_color[vm_os[_vi]],vm_os_name[vm_os[_vi]]);\
                char _vr[8];int _ri=0,_rn=vm_ram[_vi];do{_vr[_ri++]='0'+_rn%10;_rn/=10;}while(_rn);_vr[_ri]=0;\
                for(int _rk=0;_rk<_ri/2;_rk++){char _rt=_vr[_rk];_vr[_rk]=_vr[_ri-1-_rk];_vr[_ri-1-_rk]=_rt;}\
                gfx_print(wx+310,_vy+2,0x88AACC,_vr);gfx_print(wx+336,_vy+2,0x6A7A9E,"MB");\
                char _vc[2];_vc[0]='0'+vm_cores[_vi]%10;_vc[1]=0;\
                gfx_print(wx+370,_vy+2,0x88AACC,_vc);\
                char _vd[8];int _di=0,_dn=vm_disk[_vi];do{_vd[_di++]='0'+_dn%10;_dn/=10;}while(_dn);_vd[_di]=0;\
                for(int _dk=0;_dk<_di/2;_dk++){char _dt=_vd[_dk];_vd[_dk]=_vd[_di-1-_dk];_vd[_di-1-_dk]=_dt;}\
                gfx_print(wx+400,_vy+2,0x88AACC,_vd);gfx_print(wx+420,_vy+2,0x6A7A9E,"GB");\
                if(vm_running[_vi]){gfx_print(wx+450,_vy+2,0x44FF44,"Running");}else{gfx_print(wx+450,_vy+2,0x808080,"Stopped");}\
            }\
            gfx_print(wx+16,wy+260,0x3A4A6A,"[Up/Down] select  [Enter] start/stop  [c] create  [d] delete");\
        }\
    } while(0)

    // Search dropdown drawing — sleek (appears below search bar)
    #define dropdown_draw(m_,mc_,sel_) do {\
        int _dd_h = (mc_)*24 + 12;\
        int _dd_x = sb_x, _dd_y = sb_y + sb_h + 2;\
        gfx_fill_round_rect(_dd_x-2,_dd_y-2,sb_w+4,_dd_h+4,8,0x000000);\
        gfx_fill_round_rect(_dd_x,_dd_y,sb_w,_dd_h,6,0x0A0A28);\
        gfx_round_rect(_dd_x,_dd_y,sb_w,_dd_h,6,0x2A4A7A);\
        for(int _i=0;_i<mc_;_i++){\
            int _iy=_dd_y+8+_i*24;\
            if(_i==sel_){gfx_fill_round_rect(_dd_x+4,_iy-2,sb_w-8,22,4,0x1A1A5E);gfx_round_rect(_dd_x+4,_iy-2,sb_w-8,22,4,0x3A6AFF);}\
            gfx_print(_dd_x+14,_iy,(_i==sel_)?0x00E5FF:0x6A8ABE,sitems[m_[_i]]);\
        }\
    } while(0)

    mouse_poll();

    // If the Home menu was just opened, draw it over the freshly redrawn desktop
    if (home_active) draw_home(w, h, 0);

    // Play startup chime
    { extern void ac97_play_startup(void);
      if (ac97_is_init()) ac97_play_startup(); }

    // Start ambient background music after startup
    { extern void ac97_ambient_start(void);
      if (ac97_is_init()) ac97_ambient_start(); }

    // Kick home button spring (starts at 0, springs to 1000)
    home_btn_sv = 1400;
    home_btn_prev_y = h + 10; // starts off-screen

    char k;
    while (1) {
    // Launch an app after a full desktop redraw (Home overlay path)
    if (_pending_launch) {
        int _pla = _pending_launch;
        _pending_launch = 0;
        _launch_app(_pla);
    }
    mouse_poll();
    // ─── Cursor type switching based on context ───
    { extern void mouse_set_cursor(enum cursor_type);
      if (win && (win_type == 3 || win_type == 5 || win_type == 7 || win_type == 9 || win_type == 37))
          mouse_set_cursor(CURSOR_IBEAM);
      else if (win && win_type == 31)
          mouse_set_cursor(CURSOR_CROSS);
      else
          mouse_set_cursor(CURSOR_ARROW); }
    // ─── Dock magnification spring update ───
    {
        int _dmx = mouse_get_x(), _dmy = mouse_get_y();
        int _dock_center_x = w/2;
        int _dock_bot = h - 10;
        int _dock_active = (!home_active && !oem_active && _dmy >= _dock_bot - 80);
        for (int _di = 0; _di < 10; _di++) {
            int _icon_cx = _dock_center_x - 320 + 40 + _di * 56;
            int _dist = _dmx - _icon_cx;
            if (_dist < 0) _dist = -_dist;
            int _target = 1000;
            if (_dock_active && _dist < 180) {
                int _d2 = 180 - _dist;
                _target = 1000 + 500 * _d2 * _d2 / (180 * 180);
            }
            int _force = (_target - dock_sz[_di]) * 8 / 10;
            dock_sv[_di] = (dock_sv[_di] + _force) * 7 / 10;
            dock_sz[_di] += dock_sv[_di];
            if (dock_sz[_di] < 1000) dock_sz[_di] = 1000;
            if (dock_sz[_di] > 1500) dock_sz[_di] = 1500;
        }
    }
    // ─── Home button spring (scale 0→1000) ───
    {
        int _hb_target = 1000;
        int _hbf = (_hb_target - home_btn_sz) * 9 / 10;
        home_btn_sv = (home_btn_sv + _hbf) * 7 / 10;
        home_btn_sz += home_btn_sv;
        if (home_btn_sz < 0) home_btn_sz = 0;
        if (home_btn_sz > 1000) home_btn_sz = 1000;
    }
        net_poll_all();

        // ─── VoIP audio streaming (during active call) ───
        if (voip_state == VOIP_ACTIVE && win_type == 39) {
            static int _voip_tick = 0;
            _voip_tick++;

            // Every ~60ms: capture mic → downsample 44100→8000 → send RTP
            if (_voip_tick % 4 == 0 && ac97_capture_is_active()) {
                int16_t raw[VOIP_FRAME_SIZE * 2];
                int16_t frame[VOIP_FRAME_SIZE];
                static int _cap_off = 0;
                ac97_capture_read(raw, _cap_off, VOIP_FRAME_SIZE);
                _cap_off += VOIP_FRAME_SIZE;
                // Downsample 44100→8000 (take every ~5.5th sample)
                for (int i = 0; i < VOIP_FRAME_SIZE; i++) {
                    int si = i * 5; // approximate downsampling
                    if (si < VOIP_FRAME_SIZE) frame[i] = raw[si * 2];
                    else frame[i] = 0;
                }
                voip_send_audio(frame, VOIP_FRAME_SIZE);
            }

            // Play received audio from jitter buffer
            if (voip_jitter_count > 0) {
                int16_t *frame = voip_jitter_buf[voip_jitter_head];
                ac97_play_raw(frame, VOIP_FRAME_SIZE);
                voip_jitter_head = (voip_jitter_head + 1) % VOIP_JITTER_BUF;
                voip_jitter_count--;
            }
        }
        int k_ctrl = 0;
        // Dock hover magnification + click
        static int _pdh = -2;
        int _mhx = mouse_get_x(), _mhy = mouse_get_y();
        int _ddcx = w/2 - 320, _ddcy = h - 70, _dhov = -1;
        int _dhbase = _ddcx + (640 - 10*56)/2;
        if (!home_active && !oem_active && _mhy >= _ddcy - 5) {
            for (int _di = 0; _di < 10; _di++) {
                int _ix = _dhbase + _di*56;
                int _isz = 40 + (dock_sz[_di] - 1000) * 12 / 1000;
                if (_mhx >= _ix - 6 && _mhx <= _ix + _isz + 6) { _dhov = _di; break; }
            }
        }
        // Capture click BEFORE hover redraw so we don't lose it
        if (mouse_clicked() && !win && !home_active && !oem_active && !settings_active && !cc_active && !nc_active && !wifi_active) {
            int _hmx = mouse_get_x(), _hmy = mouse_get_y();
            int _hb_x = w/2 - 396, _hb_y = h - 66;
            if (_hmx >= _hb_x && _hmx < _hb_x + 56 && _hmy >= _hb_y && _hmy < _hb_y + 52) {
                open_home();
                goto redraw_desktop;
            }
        }
        if (mouse_clicked() && !win && !home_active && !oem_active && !settings_active && !cc_active && !nc_active && !wifi_active && _dhov >= 0) {
            static const int _dk_acts[] = {34, 33, 25, 3, 6, 32, 30, 31, 26, 24};
            extern void ac97_play_click(void);
            if (ac97_is_init()) ac97_play_click();
            _launch_app(_dk_acts[_dhov]);
            goto redraw_desktop;
        }
        // Redraw when hovered icon changes
        {
            // Home button slide: restore only previous frame's small rect + redraw inline
            if (home_btn_sv > 5 || home_btn_sv < -5) {
                int _dc_x = w/2 - 320, _dc_y = h - 70;
                int _final_y = _dc_y + 4;
                int _start_y = h + 10;
                int _cur_y = _start_y - (_start_y - _final_y) * home_btn_sz / 1000;
                int _hb_x = _dc_x - 76, _hb_w = 56, _hb_h = 52;
                // Restore only the PREVIOUS frame's button area (small rect)
                if (home_btn_prev_y >= 0 && home_btn_prev_y != _cur_y) {
                    volatile uint32_t *_fb2 = gfx_get_fb_addr();
                    int _pitch2 = gfx_get_pitch() / 4;
                    for (int _ry = home_btn_prev_y - 4; _ry < home_btn_prev_y + _hb_h + 8 && _ry < h; _ry++) {
                        if (_ry < 0) continue;
                        for (int _rx = _hb_x - 4; _rx < _hb_x + _hb_w + 8 && _rx < w; _rx++) {
                            if (_rx < 0 || _rx >= WP_W) continue;
                            _fb2[_ry * _pitch2 + _rx] = wallpaper_data[_ry * WP_W + _rx];
                        }
                    }
                }
                home_btn_prev_y = _cur_y;
                // Draw home button at current position
                if (home_btn_sz > 5) {
                    gfx_fill_round_rect(_hb_x+2, _cur_y+2, _hb_w, _hb_h, 16, 0x000000);
                    gfx_fill_round_rect(_hb_x, _cur_y, _hb_w, _hb_h, 16, 0x0C0C2A);
                    gfx_round_rect(_hb_x, _cur_y, _hb_w, _hb_h, 16, 0x3A6AFF);
                    gfx_round_rect(_hb_x+1, _cur_y+1, _hb_w-2, _hb_h-2, 15, 0x2A4ABE);
                    draw_icon_centered(_hb_x, _cur_y, _hb_h, ICON_HOME, 0x88BBFF, 28.0f);
                }
            }
            if (!win && !home_active && !oem_active) {
                int _dock_anim = 0;
                for (int _ds = 0; _ds < 10; _ds++) {
                    if (dock_sv[_ds] > 5 || dock_sv[_ds] < -5) { _dock_anim = 1; break; }
                    if (dock_sz[_ds] != 1000 && dock_sv[_ds] > -5 && dock_sv[_ds] < 5) {
                        dock_sz[_ds] = 1000; dock_sv[_ds] = 0;
                    }
                }
                if (_dhov != _pdh || _dock_anim) {
                    if (_dhov != _pdh && _dhov >= 0) {
                        extern void ac97_play_hover(void);
                        if (ac97_is_init()) ac97_play_hover();
                    }
                    _pdh = _dhov;
                    // Inline dock redraw (no goto — prevents cursor flicker)
                    {
                        cursor_hide();
                        volatile uint32_t *_rfb = gfx_get_fb_addr();
                        int _rpitch = gfx_get_pitch() / 4;
                        for (int _ry = h-180; _ry < h; _ry++) {
                            for (int _rx = 0; _rx < w; _rx++) {
                                if (_ry >= 0 && _ry < 720 && _rx >= 0 && _rx < 1280)
                                    _rfb[_ry * _rpitch + _rx] = wallpaper_data[_ry * 1280 + _rx];
                            }
                        }
                        int _idc_w = 640, _idc_h = 60, _idc_r = 30;
                        int _idc_x = w/2 - _idc_w/2, _idc_y = h - _idc_h - 10;
                        gfx_fill_round_rect(_idc_x+4, _idc_y+4, _idc_w, _idc_h, _idc_r, 0x000000);
                        gfx_fill_round_rect(_idc_x+2, _idc_y+2, _idc_w, _idc_h, _idc_r, 0x000000);
                        uint32_t _idk_bg = _natal_mode ? 0x080400 : (dm ? 0x04040E : 0x080820);
                        uint32_t _idk_bd = _natal_mode ? 0x660000 : (dm ? 0x1A1A3A : 0x2A2A5A);
                        uint32_t _idk_bd2 = _natal_mode ? 0x440000 : (dm ? 0x0E0E2A : 0x1A1A4A);
                        gfx_fill_round_rect(_idc_x, _idc_y, _idc_w, _idc_h, _idc_r, _idk_bg);
                        gfx_round_rect(_idc_x, _idc_y, _idc_w, _idc_h, _idc_r, _idk_bd);
                        gfx_round_rect(_idc_x+1, _idc_y+1, _idc_w-2, _idc_h-2, _idc_r-1, _idk_bd2);
                        for (int _i = 0; _i < _idc_w-80; _i += 4) {
                            int _a = 40 - (_i < (_idc_w-80)/2 ? _i : (_idc_w-80)-_i) * 30 / ((_idc_w-80)/2);
                            if (_a < 8) _a = 8;
                            uint32_t _hl = (_a*3/4<<16)|(_a/2<<8)|_a;
                            gfx_rect(_idc_x+40+_i, _idc_y+3, 4, 1, _hl);
                        }
                        int _idi_sp = 56, _idi_base_sz = 40;
                        int _idi_base = _idc_x + (_idc_w - 10*_idi_sp)/2;
                        int _idi_sz_arr[10], _idi_yy[10], _idi_xx[10];
                        for (int _mi = 0; _mi < 10; _mi++) {
                            int _msz = _idi_base_sz * dock_sz[_mi] / 1000;
                            _idi_sz_arr[_mi] = _msz;
                            _idi_yy[_mi] = _idc_y + 8 + (_idi_base_sz - _msz) / 2;
                            _idi_xx[_mi] = _idi_base + _mi * _idi_sp + (_idi_base_sz - _msz) / 2;
                        }
                        // Pass 1: all backgrounds
                        uint32_t _idbg[] = {0x2A4A2A,0x4488FF,0x6622AA,0x5A3A8A,0x3A3A4A,0x886622,0x2266AA,0x3A5A8A,0x3A1A5A,0x3A3A5A};
                        for (int _mi = 0; _mi < 10; _mi++) {
                            int _ix = _idi_xx[_mi], _iy = _idi_yy[_mi], _isz = _idi_sz_arr[_mi];
                            gfx_fill_round_rect(_ix+1,_iy+1,_isz,_isz,9,0x000000);
                            gfx_fill_round_rect(_ix,_iy,_isz,_isz,10,_idbg[_mi]);
                            gfx_round_rect(_ix,_iy,_isz,_isz,10,_natal_mode?0xFF4444:0x4A6AAF);
                        }
                        // Pass 2: all Material Icons on top (scaled with box)
                        static const int _idock_cp[] = {ICON_PUBLIC,ICON_MUSIC_NOTE,ICON_GAME,ICON_CHAT,ICON_CALCULATOR,ICON_EDIT,ICON_SHOP,ICON_CLOUD,ICON_GAMES,ICON_COMPUTER};
                        static const uint32_t _idock_cl[] = {0x66CC66,0xFFFFFF,0xFF88FF,0xBBAAFF,0x88CC88,0xFFEE88,0xFFFFFF,0xCCDDEE,0xBB44EE,0xFFFFFF};
                        for (int _mi = 0; _mi < 10; _mi++) {
                            int _ix = _idi_xx[_mi], _iy = _idi_yy[_mi], _isz = _idi_sz_arr[_mi];
                            float _isz_f = (float)_isz * 24.0f / 40.0f;
                            draw_icon_centered(_ix, _iy, _isz, _idock_cp[_mi], _idock_cl[_mi], _isz_f);
                        }
                        // Redraw home button (minidock) after wallpaper restore
                        {
                            int _ihb_x = _idc_x - 76, _ihb_w = 56, _ihb_h = 52;
                            int _ihb_y = _idc_y + 4;
                            gfx_fill_round_rect(_ihb_x+2, _ihb_y+2, _ihb_w, _ihb_h, 16, 0x000000);
                            gfx_fill_round_rect(_ihb_x, _ihb_y, _ihb_w, _ihb_h, 16, 0x0C0C2A);
                            gfx_round_rect(_ihb_x, _ihb_y, _ihb_w, _ihb_h, 16, 0x3A6AFF);
                            gfx_round_rect(_ihb_x+1, _ihb_y+1, _ihb_w-2, _ihb_h-2, 15, 0x2A4ABE);
                            draw_icon_centered(_ihb_x, _ihb_y, _ihb_h, ICON_HOME, 0x88BBFF, 28.0f);
                        }
                    }
                }
            }
        }
        k = keyboard_last_char();
        k_ctrl = keyboard_last_ctrl() || is_ctrl_pressed();
        ac97_music_poll();
        { extern void ac97_ambient_poll(void); ac97_ambient_poll(); }
        if (k) {
            extern void ac97_play_click(void);
            extern void ac97_play_confirm(void);
            ac97_play_click();
        }
_dock_go:

        // ─── Draw cursor ON TOP of everything ───
        christmas_cursor = _natal_mode;
        draw_cursor();

        // ─── HOME START MENU (compact panel) ───
        if (home_active) {
            int _px = 10, _pw = HOME_PW, _py = h - 70 - HOME_PH, _ph = HOME_PH;
            // Mouse scroll wheel support
            { int _scr = mouse_get_scroll();
              int _grid_rows2 = (HOME_APP_COUNT + HOME_COLS - 1) / HOME_COLS;
              if (_scr != 0 && _grid_rows2 > HOME_VROWS) {
                  home_scroll -= _scr;  // scroll up = negative = show earlier rows
                  if (home_scroll < 0) home_scroll = 0;
                  if (home_scroll + HOME_VROWS > _grid_rows2) home_scroll = _grid_rows2 - HOME_VROWS;
                  draw_home(w, h, 0); continue; } }
            if (mouse_clicked()) {
                int _hmx = mouse_get_x(), _hmy = mouse_get_y();
                // Close button (circle at top-right)
                int _cbx = _px + _pw - 32 + 14, _cby = _py + 12 + 14;
                int _cdx = _hmx - _cbx, _cdy = _hmy - _cby;
                if (_cdx*_cdx + _cdy*_cdy <= 13*13) {
                    home_active = 0; goto redraw_desktop;
                }
                // App grid click — 5 cols, circular icons
                int _gx = _px + 16;
                int _colw = (_pw - 32) / HOME_COLS;
                int _grid_y = _py + 68 + 18;  // grid_y + 18 (first icon center offset)
                int _gr = (_hmx - _gx) / _colw;
                int _row = (_hmy - _grid_y) / 72;
                if (_gr >= 0 && _gr < HOME_COLS && _row >= 0 && _row < HOME_VROWS) {
                    int _idx = (_row + home_scroll) * HOME_COLS + _gr;
                    if (_idx >= 0 && _idx < HOME_APP_COUNT) {
                        home_active = 0;
                        _pending_launch = home_app_act[_idx];
                        goto redraw_desktop;
                    }
                }
                // System items (horizontal at bottom)
                int _bot_y = _py + _ph - 56;
                int _sys_x = _px + 16, _sys_half = bootloader_unlocked ? (_pw - 48) / 3 : (_pw - 36) / 2;
                int _sy = _bot_y + 20, _sh = 30;
                // OEM (left half)
                if (_hmx >= _sys_x && _hmx < _sys_x + _sys_half && _hmy >= _sy && _hmy < _sy + _sh) {
                    home_active = 0; oem_active = 1;
                    draw_oem_bios(w, h);
                    continue;
                }
                // BootLoader CLI (right half)
                if (bootloader_unlocked) {
                    int _sx2 = _sys_x + _sys_half + 4;
                    if (_hmx >= _sx2 && _hmx < _sx2 + _sys_half && _hmy >= _sy && _hmy < _sy + _sh) {
                        home_active = 0;
                        _pending_launch = 37;
                        goto redraw_desktop;
                    }
                    // Syslog Viewer (far right)
                    int _sx3 = _sys_x + 2*(_sys_half + 4);
                    if (_hmx >= _sx3 && _hmx < _sx3 + _sys_half && _hmy >= _sy && _hmy < _sy + _sh) {
                        home_active = 0;
                        _pending_launch = 38;
                        goto redraw_desktop;
                    }
                }
                // Click outside panel = close
                if (_hmx < _px || _hmx >= _px + _pw || _hmy < _py || _hmy >= _py + _ph) {
                    home_active = 0; goto redraw_desktop;
                }
                draw_home(w, h, 0);
                continue;
            }
            if (k == 27) { home_active = 0; goto redraw_desktop; }
            if (k == '\t') { home_focus = !home_focus; draw_home(w, h, 0); continue; }
            // Page Up / Down — scroll grid without moving selection
            { int _grows2 = (HOME_APP_COUNT + HOME_COLS - 1) / HOME_COLS;
              if (k == KEY_PGUP && home_scroll > 0) {
                  home_scroll -= 1; if (home_scroll < 0) home_scroll = 0;
                  draw_home(w, h, 0); continue; }
              if (k == KEY_PGDN && home_scroll + HOME_VROWS < _grows2) {
                  home_scroll += 1;
                  draw_home(w, h, 0); continue; }
              if (k == KEY_HOME) { home_scroll = 0; draw_home(w, h, 0); continue; }
              if (k == KEY_END) { home_scroll = _grows2 - HOME_VROWS; if (home_scroll < 0) home_scroll = 0;
                  draw_home(w, h, 0); continue; } }
            if (home_focus == 0) {
                if (k == KEY_UP && home_sel >= HOME_COLS) {
                    home_sel -= HOME_COLS;
                    if (home_sel / HOME_COLS < home_scroll) home_scroll = home_sel / HOME_COLS;
                    draw_home(w, h, 0); continue;
                }
                if (k == KEY_DOWN && home_sel + HOME_COLS < HOME_APP_COUNT) {
                    home_sel += HOME_COLS;
                    if (home_sel / HOME_COLS >= home_scroll + HOME_VROWS) home_scroll = home_sel / HOME_COLS - HOME_VROWS + 1;
                    draw_home(w, h, 0); continue;
                }
                if (k == KEY_LEFT && home_sel > 0) { home_sel--; draw_home(w, h, 0); continue; }
                if (k == KEY_RIGHT && home_sel < HOME_APP_COUNT - 1) { home_sel++; draw_home(w, h, 0); continue; }
                if (k == '\n' || k == ' ') { home_active = 0; _pending_launch = home_app_act[home_sel]; goto redraw_desktop; }
            } else {
                if (k == KEY_UP && home_sys_sel > 0) { home_sys_sel--; draw_home(w, h, 0); continue; }
                if (k == KEY_DOWN && home_sys_sel < (bootloader_unlocked ? 2 : 0)) { home_sys_sel++; draw_home(w, h, 0); continue; }
                if (k == '\n' || k == ' ') {
                    home_active = 0;
                    if (home_sys_sel == 0) { oem_active = 1; draw_oem_bios(w, h); continue; }
                    else if (home_sys_sel == 1 && bootloader_unlocked) { _pending_launch = 37; goto redraw_desktop; }
                    else if (home_sys_sel == 2 && bootloader_unlocked) { _pending_launch = 38; goto redraw_desktop; }
                    continue;
                }
            }
            if (!k) { asm volatile("hlt"); continue; }
            continue;
        }

        // ─── OEM UNLOCKER — KAIRO BLUE BIOS ───
        if (oem_active) {
            if (mouse_clicked()) {
                int _hmx = mouse_get_x(), _hmy = mouse_get_y();
                if (_hmx >= w/2 - 120 && _hmx < w/2 + 120 && _hmy >= h - 110 && _hmy < h - 66) {
                    oem_do_unlock();
                    goto redraw_desktop;
                }
            }
            if (k == 27) { oem_active = 0; goto redraw_desktop; }
            if (k == '1' || k == '\n') { oem_do_unlock(); goto redraw_desktop; }
            if (!k) { asm volatile("hlt"); continue; }
            continue;
        }

        // ─── Drag finestra (window_t, title bar) ───
        if (win && g_win_title && (mouse_get_buttons() & 1)) {
            int _mhx = mouse_get_x(), _mhy = mouse_get_y();
            int _th = renderer_window_title_h(g_win_title);
            if (g_win.dragging ||
                (_mhy >= wy && _mhy < wy + _th && _mhx >= wx && _mhx < wx + ww)) {
                int _ox = wx, _oy = wy;
                g_win.x = wx; g_win.y = wy; g_win.w = ww; g_win.h = wh;
                window_handle_input(&g_win, _mhx, _mhy, mouse_get_buttons());
                if (g_win.x != _ox || g_win.y != _oy) {
                    wx = g_win.x; wy = g_win.y;
                    goto redraw_desktop;
                }
            } else {
                g_win.dragging = 0;
            }
        } else if (win) {
            g_win.dragging = 0;
        }

        // ─── Window traffic-light button clicks ───
        if (win && g_win_title && mouse_clicked()) {
            int _mhx = mouse_get_x(), _mhy = mouse_get_y();
            int _ly = wy + (36 - 12) / 2;
            int _btn_y1 = _ly, _btn_y2 = _ly + 12;
            // Close (red) — circle at wx+16
            if (_mhx >= wx+10 && _mhx <= wx+22 && _mhy >= _btn_y1 && _mhy <= _btn_y2) {
                close_win(); goto redraw_desktop;
            }
            // Minimize (yellow) — circle at wx+36
            if (_mhx >= wx+30 && _mhx <= wx+42 && _mhy >= _btn_y1 && _mhy <= _btn_y2) {
                close_win(); goto redraw_desktop;
            }
            // Maximize (green) — circle at wx+56
            if (_mhx >= wx+50 && _mhx <= wx+62 && _mhy >= _btn_y1 && _mhy <= _btn_y2) {
                close_win(); goto redraw_desktop;
            }
        }

        // ─── AI Command Mode Overlay ───
        if (ai_active) {
            if (k == 27) { ai_active = 0; goto redraw_desktop; }
            if (k == '\n' && ai_pos > 0) {
                ai_buf[ai_pos] = 0;
                while (serial_available()) serial_read();
                serial_puts("AI|"); serial_puts(ai_buf); serial_write('\n');
                for (int _tw = 0; _tw < 30000; _tw++) {
                    if (serial_available()) break;
                    for (volatile int _d = 0; _d < 300000; _d++);
                }
                ai_response = serial_available() ? 1 : 0;
                ai_pos = 0; ai_buf[0] = 0;
            }
            if (k == '\b' && ai_pos > 0) { ai_pos--; ai_buf[ai_pos] = 0; }
            if (ai_pos < 127 && k >= ' ' && k <= '~') { ai_buf[ai_pos++] = k; ai_buf[ai_pos] = 0; }
            {
                int _abx = w/2-200, _aby = h/4, _abw = 400, _abh = 30;
                for (int _ay = 0; _ay < h; _ay += 2) gfx_rect(0, _ay, w, 1, 0x000020);
                gfx_fill_round_rect(_abx-4,_aby-4,_abw+8,_abh+8,8,0x000000);
                gfx_fill_round_rect(_abx,_aby,_abw,_abh,6,0x0E0E30);
                gfx_round_rect(_abx,_aby,_abw,_abh,6,0x4488FF);
                if (ai_pos == 0) gfx_print(_abx+10,_aby+8,0x4A5A7E,"Ask OreoAI...");
                else { char _ab[52];int _abi;for(_abi=0;ai_buf[_abi]&&_abi<50;_abi++)_ab[_abi]=ai_buf[_abi];_ab[_abi]=0; gfx_print(_abx+10,_aby+8,0xFFFFFF,_ab); }
                if (ai_response) { gfx_fill_round_rect(_abx-4,_aby+_abh+8,_abw+8,28,8,0x0A0A1A); gfx_print(_abx+10,_aby+_abh+16,0xBB88FF,"Response sent (check OreoAI)"); }
                gfx_print(_abx+10,_aby+_abh+40,0x3A4A6A,"[Enter] send  [Esc] close");
            }
            if (!k) { asm volatile("hlt"); continue; }
            continue;
        }

        // ─── Command Palette Overlay (Ctrl+Shift+P) ───
        if (cmd_active) {
            if (k == 27) { cmd_active = 0; goto redraw_desktop; }
            if (k == '\b' && cmd_pos > 0) { cmd_pos--; cmd_buf[cmd_pos] = 0; }
            if (k == '\n' && cmd_pos > 0) {
                cmd_buf[cmd_pos] = 0;
                int _cmd_idx = -1;
                const char *_cmds[] = {"Terminal","Settings","OreoAI","Calculator","Notes","Calendar","Pomodoro","Weather","DiskUsage","ASCIIArt","TypingTest","Clipboard","FileManager","Launchpad","Screensaver","Camera","KairoPlayer","TrueVideo","KairoStudio","KairoVM",0};
                int _cmds_act[] = {3,4,5,6,7,16,17,18,19,20,21,22,23,15,0,11,12,14,9,10};
                for (int _ci = 0; _cmds[_ci]; _ci++) {
                    int _m = 1;
                    for (int _cj = 0; cmd_buf[_cj]; _cj++) {
                        char c1 = cmd_buf[_cj], c2 = _cmds[_ci][_cj];
                        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
                        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
                        if (c1 != c2) { _m = 0; break; }
                    }
                    if (_m) { _cmd_idx = _ci; break; }
                }
                if (_cmd_idx >= 0) {
                    int _act = _cmds_act[_cmd_idx];
                    cmd_active = 0;
                    if (_act == 15) {
                        win=1;win_type=15;lp_sel_x=0;lp_sel_y=0;
                        gfx_clear(0x000008);
                        for(int _ly=0;_ly<h;_ly+=4){gfx_rect(0,_ly,w,2,0x080820);gfx_rect(0,_ly+2,w,1,0x0A0A30);}
                        gfx_print_scaled(w/2-80,20,0x4488FF,"Launchpad",2);
                        int _ic=0;
                        for(int _r=0;_r<4;_r++){for(int _c=0;_c<3;_c++){
                            if(_ic>=12)break;
                            int _ix=w/2-160+_c*110,_iy=100+_r*130;
                            gfx_fill_round_rect(_ix,_iy,80,80,16,app_colors[_ic]);
                            gfx_round_rect(_ix,_iy,80,80,16,0x4A6ADF);
                            gfx_print(_ix+40-(int)app_names[_ic][0]*4,_iy+86,0x8A9ACE,app_names[_ic]);
                            _ic++;
                        }}
                        gfx_print(w/2-130,h-60,0x3A4A6A,"[Arrows] navigate  [Enter] launch  [Esc] close");
                    } else if (_act == 0) { ss_active = 1; }
                    else { _launch_app(_act); }
                    goto redraw_desktop;
                }
                cmd_active = 0; cmd_buf[0] = 0; cmd_pos = 0;
                goto redraw_desktop;
            }
            if (cmd_pos < 63 && k >= ' ' && k <= '~') { cmd_buf[cmd_pos++] = k; cmd_buf[cmd_pos] = 0; }
            {
                int _cbx = w/2-180, _cby = h/3-60, _cbw = 360, _cbh = 40;
                for (int _cy = 0; _cy < h; _cy += 2) gfx_rect(0, _cy, w, 1, 0x000020);
                gfx_fill_round_rect(_cbx-4,_cby-4,_cbw+8,_cbh+8,8,0x000000);
                gfx_fill_round_rect(_cbx,_cby,_cbw,_cbh-8,6,0x0E0E30);
                gfx_round_rect(_cbx,_cby,_cbw,_cbh-8,6,0x4488FF);
                gfx_print(_cbx+10,_cby+8,0x4A5A7E,"> ");
                if (cmd_pos == 0) gfx_print(_cbx+30,_cby+8,0x3A4A6A,"Type a command...");
                else { char _cb[52];int _cbi;for(_cbi=0;cmd_buf[_cbi]&&_cbi<50;_cbi++)_cb[_cbi]=cmd_buf[_cbi];_cb[_cbi]=0; gfx_print(_cbx+30,_cby+8,0xFFFFFF,_cb); }
                int _mcnt = 0;
                const char *_cmds[] = {"Terminal","Settings","OreoAI","Calculator","Notes","Calendar","Pomodoro","Weather","DiskUsage","ASCIIArt","TypingTest","Clipboard","FileManager","Launchpad","Screensaver","Camera","KairoPlayer","TrueVideo","KairoStudio","KairoVM",0};
                int _my = _cby + _cbh + 4;
                for (int _ci = 0; _cmds[_ci]; _ci++) {
                    int _mm = 1;
                    for (int _cj = 0; cmd_buf[_cj]; _cj++) {
                        char c1 = cmd_buf[_cj], c2 = _cmds[_ci][_cj];
                        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
                        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
                        if (c1 != c2) { _mm = 0; break; }
                    }
                    if (_mm && cmd_pos > 0) {
                        gfx_fill_round_rect(_cbx,_my,_cbw,18,3,0x12123A);
                        gfx_print(_cbx+8,_my+3,0x8899CC,_cmds[_ci]);
                        _my += 20; _mcnt++;
                        if (_mcnt > 8) break;
                    }
                }
                gfx_print(_cbx+10,_cby+_cbh+10+_mcnt*20+4,0x3A4A6A,"[Enter] run  [Esc] close");
            }
            if (!k) { asm volatile("hlt"); continue; }
            continue;
        }

        // ─── Screensaver ───
        if (k == 0 && !win && !cc_active && !nc_active && !ai_active && !cmd_active) {
            idle_ticks++;
            if (idle_ticks > 50000 && !ss_active) { ss_active = 1; idle_ticks = 0; }
        } else {
            if (ss_active) { ss_active = 0; idle_ticks = 0; goto redraw_desktop; }
            if (k != 0) idle_ticks = 0;
        }
        if (ss_active) {
            static int dv_x = 32, dv_y = 16, dv_dx = 1, dv_dy = 1, dv_c = 0;
            static uint32_t dv_colors[] = {0xFF4444,0x44FF44,0x4488FF,0xFFDD44,0xFF44FF,0x44FFDD,0xFF8844,0xFFFFFF};
            static int dv_inited = 0;
            if (!dv_inited) { dv_x=32; dv_y=16; dv_dx=1; dv_dy=1; dv_c=0; dv_inited=1; }
            ss_frame++;
            if (_natal_mode) {
                gfx_clear(0x00081C);
                for (int _si = 0; _si < 64; _si++) {
                    static int _snx[64], _sny[64], _sni=0;
                    if (!_sni) { _snx[_si]=_si*20+(_si*7)%w; _sny[_si]=(_si*13)%h; }
                    _sny[_si] += 1 + (_si%3);
                    if (_sny[_si] > h) { _sny[_si] = -10; _snx[_si] = (_si*37 + ss_frame*2) % w; }
                    gfx_putpixel(_snx[_si], _sny[_si], 0xDDEEFF);
                    gfx_putpixel(_snx[_si]+(_si%3)-1, _sny[_si]+1, 0xCCDDFF);
                }
                int _tx = w/2-20, _ty = h-120;
                for (int _ti = 0; _ti < 10; _ti++) {
                    int _tw = 60 - _ti*4;
                    gfx_fill_round_rect(_tx-_tw/2+_ti*2, _ty+_ti*6, _tw, 8, 2, 0x007700+_ti*0x001100);
                }
                gfx_fill_round_rect(w/2-15, _ty-15, 30, 20, 4, 0xFFDD00);
                gfx_rect(w/2-3, _ty-18, 6, 6, 0xFFFF44);
                gfx_print_scaled(20, h-50, 0x44AA44, "BUON NATALE!", 3);
                gfx_print_scaled(w-280, h-50, 0x4488FF, "❄", 4);
            } else {
                gfx_clear(0x000008);
                for (int _gi = 0; _gi < w; _gi += 40) gfx_rect(_gi, 0, 1, h, 0x080818);
                for (int _gj = 0; _gj < h; _gj += 40) gfx_rect(0, _gj, w, 1, 0x080818);
                dv_x += dv_dx; dv_y += dv_dy;
                int dv_w = 160, dv_h = 40;
                if (dv_x <= 0 || dv_x + dv_w >= w) { dv_dx = -dv_dx; dv_c = (dv_c+1)%8; }
                if (dv_y <= 0 || dv_y + dv_h >= h) { dv_dy = -dv_dy; dv_c = (dv_c+1)%8; }
                if (dv_x < 0) dv_x = 0; if (dv_x + dv_w > w) dv_x = w - dv_w;
                if (dv_y < 0) dv_y = 0; if (dv_y + dv_h > h) dv_y = h - dv_h;
                uint32_t dv_color = dv_colors[dv_c];
                gfx_fill_round_rect(dv_x+3, dv_y+3, dv_w, dv_h, 10, 0x000000);
                gfx_fill_round_rect(dv_x, dv_y, dv_w, dv_h, 10, dv_color);
                gfx_round_rect(dv_x, dv_y, dv_w, dv_h, 10, 0xFFFFFF);
                gfx_print_scaled(dv_x+20, dv_y+6, 0xFFFFFF, "DVD", 2);
                gfx_print(dv_x+10, dv_y+26, 0x000000, "KAIRO  OS");
            }
            if (!k) { asm volatile("hlt"); continue; }
            continue;
        }

        // ─── System Toggle Panels ───
        if (cc_active) {
            int _cxp = w-210, _cyp = 24, _cwp = 200, _chp = 170;
            gfx_rect(0,0,w,24,0x0A0A1C); gfx_rect(0,23,w,1,0x1A1A3A);
            gfx_print(10,5,0x8899CC,"Viteza");
            gfx_fill_round_rect(_cxp,_cyp,_cwp,_chp,8,0x0E0E30);
            gfx_round_rect(_cxp,_cyp,_cwp,_chp,8,0x4488FF);
            gfx_rect(_cxp+8,_cyp+32,_cwp-16,1,0x2A3A6A);
            gfx_print(_cxp+16,_cyp+10,0x8899CC,"Control Center");
            const char *_cc[4]={"Dark Mode","Wi-Fi","Notifications","Developer"};
            for(int _ci=0;_ci<4;_ci++){
                int _ciy = _cyp+40+_ci*32;
                if(_ci==cc_sel)gfx_fill_round_rect(_cxp+4,_ciy-2,_cwp-8,26,4,0x1A2A5A);
                draw_toggle(_cxp+_cwp-56,_ciy+4,set_state&(1<<_ci));
                gfx_print(_cxp+16,_ciy+6,(_ci==cc_sel)?0xFFFFFF:0x8A9ACE,_cc[_ci]);
            }
            gfx_print(_cxp+10,_cyp+_chp-18,0x3A4A6A,"[U/D] toggle  [Esc]");
            if(k==27){cc_active=0;goto redraw_desktop;}
            if(k==KEY_UP&&cc_sel>0){cc_sel--;continue;}
            if(k==KEY_DOWN&&cc_sel<3){cc_sel++;continue;}
            if(k==' '||k=='\n'){set_state^=(1<<cc_sel);continue;}
            if(!k){asm volatile("hlt");continue;}
            continue;
        }
        // ─── Settings overlay (system panel, centered) ───
        if (settings_active) {
            settings_redraw();
            if(k==27){settings_active=0;goto redraw_desktop;}
            if(k==KEY_UP&&set_cat>0){set_cat--;continue;}
            if(k==KEY_DOWN&&set_cat<5){set_cat++;continue;}
            if(k=='1'){set_state^=1;continue;}
            if(k=='2'){set_state^=2;continue;}
            if(k=='3'){set_state^=4;continue;}
            if(k=='4'){set_state^=8;continue;}
            if(k=='5'){set_state^=16;continue;}
            if(k=='6'){set_state^=32;continue;}
            if(k=='7'){set_state^=64;continue;}
            if(k=='8'){set_state^=128;continue;}
            if(set_cat==3){
                if(k==',' || k==KEY_LEFT){if(vol_level>0){vol_level--;ac97_set_volume(vol_level);}continue;}
                if(k=='.' || k==KEY_RIGHT){if(vol_level<31){vol_level++;ac97_set_volume(vol_level);}continue;}
                if(k=='m'||k=='M'){vol_mute=!vol_mute;ac97_set_mute(vol_mute);continue;}
                if(k==' '){play_sweep(400,800,300);continue;}
            }
            if(k==' '){play_sweep(400,800,300);continue;}
            if(!k){asm volatile("hlt");continue;}
            continue;
        }
        if (nc_active) {
            int _nxp = w-290, _nyp = 24, _nwp = 280, _nhp = 170;
            gfx_rect(0,0,w,24,0x0A0A1C); gfx_rect(0,23,w,1,0x1A1A3A);
            gfx_print(10,5,0x8899CC,"Viteza");
            gfx_fill_round_rect(_nxp,_nyp,_nwp,_nhp,8,0x0E0E30);
            gfx_round_rect(_nxp,_nyp,_nwp,_nhp,8,0x4488FF);
            gfx_rect(_nxp+8,_nyp+32,_nwp-16,1,0x2A3A6A);
            gfx_print(_nxp+16,_nyp+10,0x8899CC,"Notifications");
            const char *_nt[3]={"USB Device Attached","OreoAI ready","System booted"};
            for(int _ni=0;_ni<3;_ni++){
                int _niy = _nyp+40+_ni*42;
                if(_ni==nc_sel)gfx_fill_round_rect(_nxp+4,_niy-2,_nwp-8,34,4,0x1A2A5A);
                uint32_t _nc2=(_ni==0)?0x3A6AFF:((_ni==1)?0xBB88FF:0x44CC44);
                gfx_fill_round_rect(_nxp+12,_niy+4,8,8,2,_nc2);
                gfx_print(_nxp+28,_niy+2,(_ni==nc_sel)?0xFFFFFF:0x8A9ACE,_nt[_ni]);
            }
            gfx_print(_nxp+10,_nyp+_nhp-18,0x3A4A6A,"[U/D] nav  [Space] dismiss  [Esc]");
            if(k==27){nc_active=0;goto redraw_desktop;}
            if(k==KEY_UP&&nc_sel>0){nc_sel--;continue;}
            if(k==KEY_DOWN&&nc_sel<2){nc_sel++;continue;}
            if(k==' '){continue;}
            if(!k){asm volatile("hlt");continue;}
            continue;
        }

        // ─── Wi-Fi panel (rete reale via proxy) ───
        if (wifi_active) {
            int _wxp = w/2 - 105, _wyp = 40, _wwp = 210, _whp = 26*6 + 30;
            if (_wxp + _wwp > w - 10) _wxp = w - 10 - _wwp;
            gfx_fill_round_rect(_wxp,_wyp,_wwp,_whp,8,0x0E0E30);
            gfx_round_rect(_wxp,_wyp,_wwp,_whp,8,0x4488FF);
            gfx_rect(_wxp+8,_wyp+32,_wwp-16,1,0x2A3A6A);
            gfx_print(_wxp+16,_wyp+10,0x8899CC,"Wi-Fi Networks");
            if (wifi_connected) {
                gfx_print(_wxp+16,_wyp+40,0x44FF44,"Connected");
            } else if (wifi_count == 0 && wifi_scan_done) {
                gfx_print(_wxp+16,_wyp+40,0xCC4444,"No networks / proxy offline");
            } else if (wifi_count == 0) {
                gfx_print(_wxp+16,_wyp+40,0x6A7A9E,"Scanning...");
            } else {
                int _vis = wifi_count < 6 ? wifi_count : 6;
                int _wtop = (wifi_sel >= _vis) ? wifi_sel - _vis + 1 : 0;
                for(int _wi=_wtop;_wi<wifi_count&&_wi<_wtop+_vis;_wi++){
                    int _wiy = _wyp+40+(_wi-_wtop)*26;
                    if(_wi==wifi_sel)gfx_fill_round_rect(_wxp+4,_wiy-2,_wwp-8,22,4,0x1A2A5A);
                    int _bs = wifi_bars(wifi_sig[_wi]);
                    for(int _b=0;_b<4;_b++){int _bh=(_b<_bs)?(3+_b*3):2;gfx_rect(_wxp+14+_b*8,_wiy+14-_bh,4,_bh,_wi==wifi_sel?0x4488FF:0x6A7A9E);}
                    if(wifi_sec[_wi]){gfx_rect(_wxp+46,_wiy+3,8,5,0x6A7A9E);gfx_rect(_wxp+44,_wiy+8,12,8,0x6A7A9E);}
                    gfx_print(_wxp+60,_wiy+3,(_wi==wifi_sel)?0xFFFFFF:0x8A9ACE,wifi_names[_wi]);
                    char _rs[6];int _ri2=0,_rn2=wifi_sig[_wi];if(_rn2<0){_rs[_ri2++]='-';_rn2=-_rn2;}
                    do{_rs[_ri2++]='0'+_rn2%10;_rn2/=10;}while(_rn2);_rs[_ri2]=0;
                    for(int _rk=0;_rk<_ri2/2;_rk++){char _rt=_rs[_rk];_rs[_rk]=_rs[_ri2-1-_rk];_rs[_ri2-1-_rk]=_rt;}
                    gfx_print(_wxp+_wwp-30,_wiy+3,0x4A5A7E,_rs);
                }
            }
            gfx_print(_wxp+10,_wyp+_whp-18,0x3A4A6A,"[U/D] select  [Enter] connect  [Esc]");
            if(k==27){wifi_active=0;goto redraw_desktop;}
            if(k==KEY_UP&&wifi_sel>0){wifi_sel--;continue;}
            if(k==KEY_DOWN&&wifi_sel<wifi_count-1){wifi_sel++;continue;}
            if(k=='\n'&&wifi_count>0){wifi_connected=1;continue;}
            if(!k){asm volatile("hlt");continue;}
            continue;
        }

        // ─── System Ctrl Shortcuts ───
        if (!cmd_active && !ai_active) {
            if (k_ctrl && k == ' ') {
                ai_active = 1; ai_pos = 0; ai_buf[0] = 0; ai_response = 0;
                continue;
            }
            if (k_ctrl && k == 'c') {
                if (win) { close_win(); }
                nc_active = 0; cc_active = !cc_active; cc_sel = 0; wifi_active = 0;
                if (!cc_active) goto redraw_desktop;
                continue;
            }
            if (k_ctrl && k == 'n') {
                if (win) { close_win(); }
                cc_active = 0; nc_active = !nc_active; nc_sel = 0; wifi_active = 0;
                if (!nc_active) goto redraw_desktop;
                continue;
            }
            // Ctrl+Shift+P → Command Palette
            if (k_ctrl && k == 'P') {
                cmd_active = 1; cmd_pos = 0; cmd_buf[0] = 0;
                if (win) { close_win(); }
                continue;
            }
        }

        // Refresh clock every ~50 keypresses
        _tick++; if ((_tick % 50) == 0) clock_refresh();

        // Animated wallpaper wave effect (every 5 ticks, bottom area only)
        if ((_tick % 5) == 0 && !win && !home_active && !oem_active && !settings_active) {
            volatile uint32_t *_awfb = gfx_get_fb_addr();
            int _awpitch = gfx_get_pitch() / 4;
            for (int _awy = 200; _awy < h - 180; _awy += 3) {
                int _wave = (int)(k_cosf((float)(_awy + _tick * 2) * 0.02f) * 8.0f);
                for (int _awx = 0; _awx < w; _awx += 4) {
                    int _srcx = _awx + _wave;
                    if (_srcx < 0) _srcx = 0;
                    if (_srcx >= WP_W) _srcx = WP_W - 1;
                    if (_awy >= 0 && _awy < WP_H) {
                        uint32_t _px = wallpaper_data[_awy * WP_W + _srcx];
                        int _br = (_px >> 16) & 0xFF;
                        int _bg2 = (_px >> 8) & 0xFF;
                        int _bb = _px & 0xFF;
                        int _shift = (int)(k_cosf((float)(_awy * 3 + _tick * 4) * 0.01f) * 12.0f);
                        _br += _shift; _bg2 += _shift / 2; _bb += _shift;
                        if (_br < 0) _br = 0; if (_br > 255) _br = 255;
                        if (_bg2 < 0) _bg2 = 0; if (_bg2 > 255) _bg2 = 255;
                        if (_bb < 0) _bb = 0; if (_bb > 255) _bb = 255;
                        _awfb[_awy * _awpitch + _awx] = (_br << 16) | (_bg2 << 8) | _bb;
                    }
                }
            }
        }

        // Notification toasts (top-right, auto-dismiss after 200 ticks)
        if (notify_count > 0) {
            if (_tick - notify_tick > 200) {
                for (int _ni = 0; _ni < notify_count - 1; _ni++)
                    for (int _nj = 0; _nj < 80; _nj++) notify_buf[_ni][_nj] = notify_buf[_ni+1][_nj];
                notify_count--;
                notify_tick = _tick;
            }
            if (notify_count > 0 && !win && !home_active && !oem_active && !settings_active) {
                for (int _ni = 0; _ni < notify_count; _ni++) {
                    int _ntx = w - 280, _nty = 30 + _ni * 36, _ntw = 260, _nth = 30;
                    gfx_fill_round_rect(_ntx+2, _nty+2, _ntw, _nth, 8, 0x000000);
                    gfx_fill_round_rect(_ntx, _nty, _ntw, _nth, 8, 0x0E1A3A);
                    gfx_round_rect(_ntx, _nty, _ntw, _nth, 8, 0x3A6AFF);
                    gfx_fill_circle_aa(_ntx+14, _nty+15, 4, 0x44CC44);
                    gfx_print(_ntx+24, _nty+8, 0xCCDDEE, notify_buf[_ni]);
                }
            }
        }

        // Push startup notifications
        if (_tick == 100) {
            for (int _sn = 0; _sn < 80; _sn++) notify_buf[0][_sn] = "System booted"[_sn] ? "System booted"[_sn] : 0;
            notify_count = 1; notify_tick = _tick;
        }

        // Poll USB for device changes
        if ((_tick % 100) == 0 && usb_poll() && !usb_popup && !win) {
            usb_popup = 1;
            int px = w-220, py = h-108, pw = 200, ph = 40;
            gfx_fill_round_rect(px+2,py+2,pw,ph,8,0x000000);
            gfx_fill_round_rect(px,py,pw,ph,8,0x0C0C2C);
            gfx_round_rect(px,py,pw,ph,8,0x3A6AFF);
            gfx_fill_round_rect(px+8,py+9,8,8,2,0x3A6AFF);
            gfx_rect(px+10,py+11,4,4,0x00E5FF);
            gfx_print(px+22,py+8,0xFFFFFF,"USB Device Attached");
            gfx_print(px+22,py+24,0x6A8ABE,usb_device_count() > 0 ? usb_device_name(0) : "Unknown device");
        }

        // Dismiss USB popup on any key
        if (usb_popup && k) {
            usb_popup = 0;
            int px = w-220, py = h-108, pw = 200, ph = 40;
            int _dmc = set_state & 1;
            for (int _y=py; _y<py+ph+4; _y++) {
                int _t=_y*255/h,_r,_g,_b;
                if(_dmc){_r=2+_t/60;_g=1+_t/40;_b=8+_t/30;if(_y<180){int _f=180-_y;_r=4+_f/40;_g=2+_f/60;_b=12+_f/15;}if(_r>8){_r=8;}if(_g>6){_g=6;}if(_b>20){_b=20;}}
                else{_r=8+_t/30;_g=6+_t/20;_b=24+_t/10;if(_y<180){int _f=180-_y;_r=18+_f/12;_g=10+_f/20;_b=50+_f/6;}if(_r>32){_r=32;}if(_g>28){_g=28;}if(_b>60){_b=60;}}
                gfx_rect(px,_y,pw+4,1,(_r<<16)|(_g<<8)|_b);
            }
            for(int _i=0;_i<60;_i++){
                int _sx=(_i*691+47)%w,_sy=(_i*983+19)%(h*3/5);
                if(_sx>=px&&_sx<px+pw+4&&_sy>=py&&_sy<py+ph+4){
                    int _sb=(_i*257+13)%6;if(_sb<2)continue;
                    uint32_t _sc=_sb>4?0xAAC0EE:(_sb>2?0x8899CC:0x6677AA);
                    gfx_fill_round_rect(_sx-1,_sy-1,3,3,1,_sc);
                }
            }
            continue;
        }

        // ─── WINDOW IS OPEN ───
        if (win) {
            if ((k == 27 || k == 'q' || (k_ctrl && k == 'w')) && win_type != 3 && win_type != 5) { close_win(); search_focus = 1; goto redraw_desktop; }

            // Terminal input
            if (win_type == 3) {
                if (k == 27 || k == 'q' || (k_ctrl && k == 'w')) { close_win(); search_focus = 1; goto redraw_desktop; }
                if (k == KEY_UP && term_scroll > 0) { term_scroll--; term_redraw(); continue; }
                if (k == KEY_DOWN && term_scroll < term_line_count-1) { term_scroll++; term_redraw(); continue; }
                if (k == '\n') {
                    term_buf[term_pos] = 0;
                    if (term_pos > 0) {
                        term_add("> ",0);
                        int _ti; for(_ti=0;term_buf[_ti];_ti++) term_lines[term_line_count-1][_ti+2]=term_buf[_ti];
                        term_lines[term_line_count-1][_ti+2]=0;
                        // Parse command
                        if (term_buf[0]=='h'&&term_buf[1]=='e'&&term_buf[2]=='l'&&term_buf[3]=='p'&&!term_buf[4]) {
                            term_add("Available commands:","");term_add("  help     - Show this","");
                            term_add("  echo     - Repeat text","");term_add("  clear    - Clear screen","");
                            term_add("  whoami   - Show user","");term_add("  ver      - OS version","");
                            term_add("  date     - Show date","");term_add("  neofetch - System info","");
                            term_add("  calc N+M - Addition","");
                            term_add("  natalize - Christmas mode","");
                            term_add("  ls       - List files","");
                            term_add("  cat FILE - View file","");
                            term_add("  pwd      - Print directory","");
                            term_add("  uname    - System name","");
                            term_add("  uptime   - Run time","");
                        } else if (term_buf[0]=='c'&&term_buf[1]=='l'&&term_buf[2]=='e'&&term_buf[3]=='a'&&term_buf[4]=='r'&&!term_buf[5]) {
                            term_line_count = 0; term_scroll = 0;
                        } else if (term_buf[0]=='w'&&term_buf[1]=='h'&&term_buf[2]=='o'&&term_buf[3]=='a'&&term_buf[4]=='m'&&term_buf[5]=='i'&&!term_buf[6]) {
                            term_add("user@viteza","");
                        } else if (term_buf[0]=='v'&&term_buf[1]=='e'&&term_buf[2]=='r'&&!term_buf[3]) {
                            term_add("Viteza Kernel v1.0 (x86_64)","");
                        } else if (term_buf[0]=='d'&&term_buf[1]=='a'&&term_buf[2]=='t'&&term_buf[3]=='e'&&!term_buf[4]) {
                            int _dy=0,_dm=0,_dd=0,_dw=0; rtc_read_full(&_dy,&_dm,&_dd,&_dw);
                            int _dhs2=0,_dms2=0,_dss2=0; rtc_read_time(&_dhs2,&_dms2,&_dss2);
                            const char *_dmnames[12]={"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
                            const char *_dwnames[7]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
                            const char *_dn = (_dw>=1&&_dw<=7) ? _dwnames[_dw-1] : _dwnames[0];
                            char _dbuf[48]; int _dbi=0,_dx;
                            while((_dn[_dbi])){_dbuf[_dbi]=_dn[_dbi];_dbi++;}
                            _dbuf[_dbi++]=' ';
                            const char *_mo=_dmnames[(_dm>=1&&_dm<=12)?_dm-1:0];
                            for(_dx=0;_mo[_dx];_dx++)_dbuf[_dbi++]=_mo[_dx];
                            _dbuf[_dbi++]=' ';
                            if(_dd<10)_dbuf[_dbi++]='0';
                            if(_dd>=100){_dbuf[_dbi++]='0'+_dd/100;_dd%=100;}
                            _dbuf[_dbi++]='0'+_dd/10;_dbuf[_dbi++]='0'+_dd%10;
                            _dbuf[_dbi++]=' ';
                            if(_dy>=1000){_dbuf[_dbi++]='0'+_dy/1000;_dy%=1000;}
                            _dbuf[_dbi++]='0'+_dy/100;_dy%=100;
                            _dbuf[_dbi++]='0'+_dy/10;_dbuf[_dbi++]='0'+_dy%10;
                            _dbuf[_dbi++]=' ';
                            _dbuf[_dbi++]='0'+_dhs2/10;_dbuf[_dbi++]='0'+_dhs2%10;_dbuf[_dbi++]=':';
                            _dbuf[_dbi++]='0'+_dms2/10;_dbuf[_dbi++]='0'+_dms2%10;_dbuf[_dbi++]=':';
                            _dbuf[_dbi++]='0'+_dss2/10;_dbuf[_dbi++]='0'+_dss2%10;
                            _dbuf[_dbi]=0;
                            term_add(_dbuf,0);
                        } else if (term_buf[0]=='n'&&term_buf[1]=='e'&&term_buf[2]=='o'&&term_buf[3]=='f'&&term_buf[4]=='e'&&term_buf[5]=='t'&&term_buf[6]=='c'&&term_buf[7]=='h'&&!term_buf[8]) {
                            term_add("OS: Viteza v1.0","");term_add("Kernel: x86_64 Long Mode","");
                            term_add("WM: OreoWM (built-in)","");term_add("RAM: 256 MB","");
                            term_add("Display: 1280x720x32","");term_add("Terminal: KairoShell v1.0","");
                        } else if (term_buf[0]=='e'&&term_buf[1]=='c'&&term_buf[2]=='h'&&term_buf[3]=='o'&&term_buf[4]==' ') {
                            char _ec[72];int _ei;
                            for(_ei=5;term_buf[_ei]&&_ei<76;_ei++)_ec[_ei-5]=term_buf[_ei];
                            _ec[_ei-5]=0;
                            term_add(_ec,0);
                        } else if (term_buf[0]=='c'&&term_buf[1]=='a'&&term_buf[2]=='l'&&term_buf[3]=='c'&&term_buf[4]==' ') {
                            int _a=0,_b=0,_op=0,_ci;
                            for(_ci=5;term_buf[_ci];_ci++){
                                if(term_buf[_ci]=='+'||term_buf[_ci]=='-'){_op=term_buf[_ci];break;}
                                if(term_buf[_ci]>='0'&&term_buf[_ci]<='9')_a=_a*10+(term_buf[_ci]-'0');
                            }
                            int _oi;for(_oi=_ci+1;term_buf[_oi];_oi++){if(term_buf[_oi]>='0'&&term_buf[_oi]<='9')_b=_b*10+(term_buf[_oi]-'0');}
                            int _r=_op=='+'?_a+_b:_a-_b;
                            char _rs[16];int _ri=0,_rn=_r;
                            if(_rn<0){_rs[_ri++]='-';_rn=-_rn;}
                            do{_rs[_ri++]='0'+_rn%10;_rn/=10;}while(_rn);
                            _rs[_ri]=0;
                            for(int _rk=0;_rk<_ri/2;_rk++){char _rt=_rs[_rk];_rs[_rk]=_rs[_ri-1-_rk];_rs[_ri-1-_rk]=_rt;}
                            term_add(_rs,0);
                        } else if (term_buf[0]=='n'&&term_buf[1]=='a'&&term_buf[2]=='t'&&term_buf[3]=='a'&&term_buf[4]=='l'&&term_buf[5]=='i'&&term_buf[6]=='z'&&term_buf[7]=='e'&&!term_buf[8]) {
                            _natal_mode = 1;
                            for(int _ni=0;_ni<64;_ni++){_natal_snow[_ni][0]=(_ni*709+53)%1280;_natal_snow[_ni][1]=(_ni*997+31)%720;}
                            term_add("🎄 NATALIZZATO! Buon Natale! 🎄",0);
                            term_add("Il tuo OS ora è tutto natalizio!","");
                            play_jingle_bells();
                        } else if (term_buf[0]=='u'&&term_buf[1]=='n'&&term_buf[2]=='n'&&term_buf[3]=='a'&&term_buf[4]=='t'&&term_buf[5]=='a'&&term_buf[6]=='l'&&!term_buf[7]||(term_buf[0]=='n'&&term_buf[1]=='o'&&term_buf[2]=='r'&&term_buf[3]=='m'&&term_buf[4]=='a'&&term_buf[5]=='l'&&term_buf[6]=='i'&&term_buf[7]=='z'&&term_buf[8]=='e'&&!term_buf[9])) {
                            _natal_mode = 0;
                            term_add("Natale disattivato. OS normale.","");
                        }
                        // ls command
                        else if (term_buf[0]=='l'&&term_buf[1]=='s'&&!term_buf[2]) {
                            const char *_ls_files[] = {"Documents/","Pictures/","Music/","Videos/","Projects/","README.txt","config.ini","notes.txt"};
                            for(int _li=0;_li<8;_li++) term_add(_ls_files[_li],0);
                        }
                        // cat command
                        else if (term_buf[0]=='c'&&term_buf[1]=='a'&&term_buf[2]=='t'&&term_buf[3]==' ') {
                            if(term_buf[4]=='R'&&term_buf[5]=='E') term_add("KairoOS v1.0 - Bare Metal OS",0);
                            else if(term_buf[4]=='c'&&term_buf[5]=='o') term_add("[config] dark_mode=0 vol=31",0);
                            else if(term_buf[4]=='n'&&term_buf[5]=='o') term_add("My TODO list: 1) learn OS dev",0);
                            else term_add("cat: file not found","");
                        }
                        // pwd command
                        else if (term_buf[0]=='p'&&term_buf[1]=='w'&&term_buf[2]=='d'&&!term_buf[3]) {
                            term_add("/home/user","");
                        }
                        // uname command
                        else if (term_buf[0]=='u'&&term_buf[1]=='n'&&term_buf[2]=='a'&&term_buf[3]=='m'&&term_buf[4]=='e'&&!term_buf[5]) {
                            term_add("KairoOS 1.0.0 x86_64","");
                        }
                        // uptime command
                        else if (term_buf[0]=='u'&&term_buf[1]=='p'&&term_buf[2]=='t'&&term_buf[3]=='i'&&term_buf[4]=='m'&&term_buf[5]=='e'&&!term_buf[6]) {
                            char _ub[40]; int _ut = _tick/50; int _um = _ut/60; int _uh = _um/60;
                            _ub[0]='0'+_uh%24/10; _ub[1]='0'+_uh%24%10; _ub[2]=':';
                            _ub[3]='0'+_um%60/10; _ub[4]='0'+_um%60%10; _ub[5]=0;
                            term_add("up ",0); term_add(_ub,0);
                        }
                        else if (term_pos > 0) {
                            term_add("Unknown command. Type 'help'.","");
                        }
                    }
                    term_pos = 0; term_buf[0] = 0;
                    term_redraw();
                    continue;
                }
                if (k == '\b' && term_pos > 0) { term_pos--; term_buf[term_pos] = 0; term_redraw(); continue; }
                if (term_pos < 127 && k >= ' ' && k <= '~') { term_buf[term_pos++] = k; term_buf[term_pos] = 0; term_redraw(); continue; }
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // OreoAI input
            if (win_type == 5) {
                // Durante la registrazione STT, Esc = annulla
                if (k == 27 && stt_active) {
                    while (serial_available()) serial_read();
                    serial_puts("STT|ABORT\n");
                    stt_active = 0; stt_train = 0;
                    chat_redraw();
                    continue;
                }
                if (k == 27 || k == 'q' || (k_ctrl && k == 'w')) { close_win(); search_focus = 1; goto redraw_desktop; }
                if (k == KEY_UP && chat_scroll > 0) { chat_scroll--; chat_redraw(); continue; }
                if (k == KEY_DOWN && chat_scroll < chat_line_count-1) { chat_scroll++; chat_redraw(); continue; }

                // Toggle training mode (Ctrl+T)
                if (k_ctrl && k == 't') {
                    stt_train = !stt_train;
                    chat_redraw();
                    continue;
                }

                // Avvia/ferma registrazione: bottone mic o Ctrl+K
                int _mbx = wx+ww-42, _mby = wy+44+wh-56-18;
                int _toggle = (k_ctrl && k == 'k') ||
                              (mouse_clicked() && mouse_get_x() >= _mbx && mouse_get_x() < _mbx+30 &&
                               mouse_get_y() >= _mby && mouse_get_y() < _mby+20);
                if (_toggle && !stt_active) {
                    while (serial_available()) serial_read();
                    if (stt_train) {
                        char _tph[80]; int _ti;
                        for (_ti=0; chat_buf[_ti] && _ti<79; _ti++) _tph[_ti]=chat_buf[_ti];
                        _tph[_ti]=0;
                        if (_ti == 0) { const char *_d = "ciao oreo";
                            for (_ti=0; _d[_ti]; _ti++) _tph[_ti]=_d[_ti]; _tph[_ti]=0; }
                        serial_puts("STT|TRAIN|"); serial_puts(_tph); serial_write('\n');
                    } else {
                        serial_puts("STT|START\n");
                    }
                    stt_active = 1; stt_ticks = 0;
                    chat_redraw();
                    continue;
                }

                // Stop registrazione STT: ricevi il PCM dal proxy, riconosci nel
                // kernel (DTW nativo + template di sessione); se fallisce, invia
                // l'audio al proxy per la dettatura Apple Speech
                if (stt_active && k == '\n') {
                    int _is_train = stt_train;
                    char _tph[80]; int _tlen;
                    for (_tlen=0; chat_buf[_tlen] && _tlen<79; _tlen++) _tph[_tlen]=chat_buf[_tlen];
                    _tph[_tlen]=0;
                    if (_tlen == 0) { const char *_d = "ciao oreo";
                        for (_tlen=0; _d[_tlen]; _tlen++) _tph[_tlen]=_d[_tlen]; _tph[_tlen]=0; }
                    while (serial_available()) serial_read();
                    serial_puts("STT|STOP\n");
                    int _got_pcm = 0, _n16 = 0;
                    for (int _tw = 0; _tw < 30000; _tw++) {
                        if (serial_available()) break;
                        for (volatile int _d = 0; _d < 300000; _d++);
                    }
                    if (serial_available()) {
                        char _rline[200]; int _ri = 0;
                        while (_ri < 199) {
                            if (!serial_read_timeout(&_rline[_ri], 5000)) break;
                            if (_rline[_ri] == '\n' || _rline[_ri] == '\r') break;
                            _ri++;
                        }
                        _rline[_ri] = 0;
                        serial_puts("STTDBG|hdr="); serial_puts(_rline); serial_write('\n');
                        if (_rline[0]=='S'&&_rline[1]=='T'&&_rline[2]=='T'&&_rline[3]=='|'&&
                            _rline[4]=='P'&&_rline[5]=='C'&&_rline[6]=='M'&&_rline[7]=='|') {
                            int _v = 0, _p = 8;
                            while (_rline[_p] >= '0' && _rline[_p] <= '9') { _v = _v*10 + (_rline[_p]-'0'); _p++; }
                            _n16 = _v;
                            if (_n16 > 80000) _n16 = 80000;
                            _got_pcm = 1;
                        }
                    }
                    if (_got_pcm && _n16 > 0) {
                        int _got16 = serial_read_bulk((char *)stt_tmp_mono, _n16 * 2, 40000000) / 2;
                        _n16 = _got16;
                        serial_puts("STTDBG|rx="); serial_dec(_n16); serial_write('\n');
                        long _sum = 0;
                        for (int _s = 0; _s < _n16; _s++) _sum += stt_tmp_mono[_s];
                        serial_puts("STTDBG|sum="); serial_dec((int)_sum); serial_write('\n');
                    }
                    stt_active = 0; stt_train = 0;
                    if (_got_pcm && _n16 > 4000) {
                        if (_is_train) {
                            int _sidx = stt_train_pcm16(stt_tmp_mono, _n16, (const char*)_tph);
                            serial_puts("STTDBG|train=");
                            serial_puts((const char*)_tph);
                            serial_puts("|sess="); serial_dec(_sidx); serial_write('\n');
                            if (_sidx >= 0) {
                                int _j; for (_j=0; _tph[_j] && _j<127; _j++) chat_buf[_j]=(char)_tph[_j];
                                chat_buf[_j]=0; chat_pos=_j;
                                chat_submit();
                            } else {
                                chat_redraw();
                            }
                        } else {
                            int _idx = stt_recognize_pcm16(stt_tmp_mono, _n16);
                            serial_puts("STTDBG|n="); serial_dec(_n16);
                            serial_puts("|idx="); serial_dec(_idx);
                            serial_puts("|dist="); serial_dec((int)(stt_last_dist * 1000.0));
                            serial_puts("|best="); serial_dec(stt_last_best); serial_write('\n');
                            if (_idx >= 0) {
                                const char *_ph = stt_phrase_name(_idx);
                                int _j; for (_j=0; _ph[_j] && _j<127; _j++) chat_buf[_j]=_ph[_j];
                                chat_buf[_j]=0; chat_pos=_j;
                                chat_submit();
                            } else {
                                // Fallback: invia audio al proxy per la dettatura Apple Speech
                                int _got = 0;
                                char _resp[160] = {0};
                                while (serial_available()) serial_read();
                                serial_puts("STT|AUDIO|16000|1|");
                                serial_dec(_n16); serial_write('\n');
                                for (int _i = 0; _i < _n16; _i++) {
                                    serial_write((char)(stt_tmp_mono[_i] & 0xFF));
                                    serial_write((char)((stt_tmp_mono[_i] >> 8) & 0xFF));
                                }
                                for (int _tw = 0; _tw < 30000; _tw++) {
                                    if (serial_available()) break;
                                    for (volatile int _d = 0; _d < 300000; _d++);
                                }
                                if (serial_available()) {
                                    char _rline[200];
                                    while (1) {
                                        int _ri = 0;
                                        while (_ri < 199) {
                                            if (!serial_read_timeout(&_rline[_ri], 5000)) break;
                                            if (_rline[_ri] == '\n' || _rline[_ri] == '\r') break;
                                            _ri++;
                                        }
                                        _rline[_ri] = 0;
                                        if (_rline[0]=='E'&&_rline[1]=='N'&&_rline[2]=='D'&&_rline[3]==0) break;
                                        if (_rline[0]=='R'&&_rline[1]=='E'&&_rline[2]=='S'&&_rline[3]=='P'&&_rline[4]=='|') {
                                            char *_rt = _rline + 5;
                                            int _j; for (_j=0; _rt[_j] && _j<159; _j++) _resp[_j]=_rt[_j];
                                            _resp[_j]=0;
                                            if (_resp[0]) _got = 1;
                                        }
                                    }
                                }
                                if (_got) {
                                    int _j; for (_j=0; _resp[_j] && _j<127; _j++) chat_buf[_j]=_resp[_j];
                                    chat_buf[_j]=0; chat_pos=_j;
                                    chat_submit();
                                } else {
                                    chat_redraw();
                                }
                            }
                        }
                    } else {
                        chat_redraw();
                    }
                    continue;
                }

                if (k == '\n') { chat_submit(); continue; }
                if (k == '\b' && chat_pos > 0) { chat_pos--; chat_buf[chat_pos] = 0; chat_redraw(); continue; }
                if (chat_pos < 127 && k >= ' ' && k <= '~') { chat_buf[chat_pos++] = k; chat_buf[chat_pos] = 0; chat_redraw(); continue; }

                // Animazione del registratore
                if (stt_active) { stt_ticks++; chat_redraw(); }
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // Calculator GUI (6)
            if (win_type == 6) {
                if (k == 27 || k == 'q') { close_win(); search_focus = 1; goto redraw_desktop; }
                if (k >= '0' && k <= '9') { calc_cur = calc_cur * 10 + (k - '0'); calc_redraw(); continue; }
                if (k == '+') {
                    if (calc_state && calc_op) { if (calc_op==1)calc_val=calc_val+calc_cur;if(calc_op==2)calc_val=calc_val-calc_cur;if(calc_op==3)calc_val=calc_val*calc_cur;if(calc_op==4&&calc_cur)calc_val=calc_val/calc_cur;calc_cur=0; } else { calc_val = calc_cur; calc_cur = 0; }
                    calc_op = 1; calc_state = 1; calc_redraw(); continue;
                }
                if (k == '-') {
                    if (calc_state && calc_op) { if (calc_op==1)calc_val=calc_val+calc_cur;if(calc_op==2)calc_val=calc_val-calc_cur;if(calc_op==3)calc_val=calc_val*calc_cur;if(calc_op==4&&calc_cur)calc_val=calc_val/calc_cur;calc_cur=0; } else { calc_val = calc_cur; calc_cur = 0; }
                    calc_op = 2; calc_state = 1; calc_redraw(); continue;
                }
                if (k == '*') {
                    if (calc_state && calc_op) { if (calc_op==1)calc_val=calc_val+calc_cur;if(calc_op==2)calc_val=calc_val-calc_cur;if(calc_op==3)calc_val=calc_val*calc_cur;if(calc_op==4&&calc_cur)calc_val=calc_val/calc_cur;calc_cur=0; } else { calc_val = calc_cur; calc_cur = 0; }
                    calc_op = 3; calc_state = 1; calc_redraw(); continue;
                }
                if (k == '/') {
                    if (calc_state && calc_op) { if (calc_op==1)calc_val=calc_val+calc_cur;if(calc_op==2)calc_val=calc_val-calc_cur;if(calc_op==3)calc_val=calc_val*calc_cur;if(calc_op==4&&calc_cur)calc_val=calc_val/calc_cur;calc_cur=0; } else { calc_val = calc_cur; calc_cur = 0; }
                    calc_op = 4; calc_state = 1; calc_redraw(); continue;
                }
                if (k == '\n' || k == '=') {
                    if (calc_state && calc_op) {
                        {if(calc_op==1)calc_cur=calc_val+calc_cur;if(calc_op==2)calc_cur=calc_val-calc_cur;if(calc_op==3)calc_cur=calc_val*calc_cur;if(calc_op==4&&calc_cur)calc_cur=calc_val/calc_cur;}
                        calc_op=0; calc_state=0;
                    }
                    calc_redraw(); continue;
                }
                if (k == 'c' || k == 'C') { calc_val=0;calc_cur=0;calc_op=0;calc_state=0; calc_redraw(); continue; }
                if (k == '\b') { calc_cur /= 10; calc_redraw(); continue; }
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // Notes app (7)
            if (win_type == 7) {
                if (k == 27 || k == 'q') { close_win(); note_widget_draw(); search_focus = 1; continue; }
                if (k == KEY_LEFT && note_pos > 0) { note_pos--; print_note_buf(); continue; }
                if (k == '\n') {
                    note_buf[note_pos] = 0;
                    if (note_pos > 0 && note_count < 10) {
                        int _ni;
                        for(_ni=0;note_buf[_ni]&&_ni<79;_ni++) notes[note_count][_ni]=note_buf[_ni];
                        notes[note_count][_ni]=0; note_count++;
                        gfx_print(wx+ww-80,wy+46,0x44FF44,"Saved!");
                        note_sel = note_count-1;
                        note_widget_draw();
                    }
                    note_pos = 0; note_buf[0] = 0;
                    // Redraw note list
                    gfx_fill_round_rect(wx+12,wy+44,ww-24,wh-56,6,0x08081C);
                    gfx_fill_round_rect(wx+16,wy+50,ww-32,20,4,0x12124A);gfx_print(wx+24,wy+53,0x4A9EFF,"My Notes");
                    for(int _ni=0;_ni<note_count&&_ni<8;_ni++){
                        char _ns[4];_ns[0]='0'+(_ni+1)%10;_ns[1]='.';_ns[2]=' ';_ns[3]=0;
                        gfx_print(wx+20,wy+78+_ni*16,0x8899CC,_ns);
                        char _nt[44];note_short(_ni,_nt);
                        gfx_print(wx+40,wy+78+_ni*16,0x6A8ABE,_nt);
                    }
                    gfx_rect(wx+12,wy+210,ww-24,1,0x2A3A6A);
                    gfx_print(wx+16,wy+218,0x4A6A8A,"> ");
                    print_note_buf();
                    continue;
                }
                if (k == '\b' && note_pos > 0) { note_pos--; note_buf[note_pos] = 0;
                    print_note_buf(); continue; }
                if (note_pos < 78 && k >= ' ' && k <= '~') { note_buf[note_pos++] = k; note_buf[note_pos] = 0;
                    print_note_buf(); continue; }
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // App Store (8)
            if (win_type == 8) {
                if (k == 27 || k == 'q') { close_win(); search_focus = 1; goto redraw_desktop; }
                if (k >= '0' && k <= '9') {
                    int _ai = k - '0';
                    if (_ai < APP_COUNT) { apps_installed[_ai] = !apps_installed[_ai];
                        gfx_fill_round_rect(wx+16,wy+50,ww-32,20,4,0x12124A);gfx_print(wx+24,wy+53,0x4A9EFF,"Available Apps");
                        for(int _asi=0;_asi<APP_COUNT&&_asi<10;_asi++){
                            int _asy=wy+78+_asi*20;
                            gfx_fill_round_rect(wx+16,_asy-2,ww-32,18,3,app_colors[_asi]);gfx_rect(wx+16,_asy-2,4,18,app_colors[_asi]);
                            gfx_print(wx+28,_asy+1,0xFFFFFF,app_names[_asi]);
                            gfx_print(wx+180,_asy+1,0x8080AA,app_cats[_asi]);
                            if(apps_installed[_asi]){gfx_print(wx+300,_asy+1,0x44FF44,"[Installed]");}
                            else{gfx_print(wx+300,_asy+1,0x808080,"[ ");char _ak[2];_ak[0]='0'+_asi%10;_ak[1]=0;gfx_print(wx+312,_asy+1,0xFFAA00,_ak);gfx_print(wx+324,_asy+1,0x808080," ]");}
                        }
                    }
                    continue;
                }
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // ─── Kairo Studio macros ───
            #define studio_init_data() do {\
                studio_panel=0;studio_file=0;int _r,_c;\
                const char *s0[]={"<?xml version=\"1.0\"?>","<catalog>","  <book id=\"bk101\">","    <title>Viteza OS</title>","    <author>Kairo Dev</author>","    <price currency=\"USD\">29.99</price>","  </book>","</catalog>"};\
                code_lines[0]=8;code_cx[0]=0;code_cy[0]=0;code_scroll[0]=0;\
                for(_r=0;_r<8;_r++){for(_c=0;s0[_r][_c]&&_c<59;_c++)code_buf[0][_r][_c]=s0[_r][_c];code_buf[0][_r][_c]=0;}\
                const char *s1[]={"class AppController {","  constructor(name, ver) {","    this.name = name;","    this.version = ver;","  }","  start() {","    return \"Running \" + this.name;","  }","}"};\
                code_lines[1]=9;code_cx[1]=0;code_cy[1]=0;code_scroll[1]=0;\
                for(_r=0;_r<9;_r++){for(_c=0;s1[_r][_c]&&_c<59;_c++)code_buf[1][_r][_c]=s1[_r][_c];code_buf[1][_r][_c]=0;}\
                const char *s2[]={"body {","  background: #0A0A1A;","  color: #88CCFF;","  font-family: monospace;","}","h1 {","  color: #FFAA44;","}"};\
                code_lines[2]=8;code_cx[2]=0;code_cy[2]=0;code_scroll[2]=0;\
                for(_r=0;_r<8;_r++){for(_c=0;s2[_r][_c]&&_c<59;_c++)code_buf[2][_r][_c]=s2[_r][_c];code_buf[2][_r][_c]=0;}\
            } while(0)
            #define studio_draw_all() do {\
                /* fill content bg */\
                gfx_fill_round_rect(wx+4,wy+40,ww-8,wh-48,4,0x000000);\
                int _ex1=wx+8,_ey1=wy+44,_ew1=112,_eh1=wh-62;\
                gfx_fill_round_rect(_ex1,_ey1,_ew1,_eh1,4,0x08081C);\
                gfx_round_rect(_ex1,_ey1,_ew1,_eh1,4,studio_panel==1?0x4488FF:0x1A2A4A);\
                gfx_print(_ex1+6,_ey1+3,0x6A8ABE,"Explorer");\
                gfx_rect(_ex1+4,_ey1+14,_ew1-8,1,0x1A2A5A);\
                for(int _fi=0;_fi<3;_fi++){\
                    int _fy=_ey1+20+_fi*18;\
                    if(_fi==studio_file){gfx_fill_round_rect(_ex1+2,_fy,_ew1-4,16,3,0x1A3A8A);}\
                    gfx_print(_ex1+8,_fy+2,_fi==studio_file?0xFFFFFF:0x8899CC,studio_fnames[_fi]);\
                }\
                int _ex2=wx+124,_ey2=wy+44,_ew2=ww-132,_eh2=wh-62;\
                int _out_h = studio_output_count > 0 ? 50 : 0;\
                int _ed_h = _eh2 - _out_h;\
                gfx_fill_round_rect(_ex2,_ey2,_ew2,_ed_h,4,0x0A0A1A);\
                gfx_round_rect(_ex2,_ey2,_ew2,_ed_h,4,studio_panel==0?0x4488FF:0x1A2A4A);\
                gfx_print(_ex2+4,_ey2+2,0x6666AA,studio_fnames[studio_file]);\
                {char _md[5];_md[0]='[';_md[1]="XJC"[studio_file];_md[2]=']';_md[3]=0;\
                gfx_print(_ex2+_ew2-24,_ey2+2,0x444466,_md);}\
                gfx_rect(_ex2+2,_ey2+13,_ew2-4,1,0x1A2A5A);\
                int _ln_x=_ex2+2,_ln_w=24,_code_x=_ln_x+_ln_w,_code_y=_ey2+16,_code_h=_ed_h-22;\
                gfx_fill_round_rect(_ln_x,_code_y,_ln_w-2,_code_h,2,0x060612);\
                int _fi2=studio_file,_vis=_code_h/8,_sc=code_scroll[_fi2];\
                for(int _li=0;_li<_vis&&(_sc+_li)<code_lines[_fi2];_li++){\
                    int _lx=_code_x,_ly=_code_y+_li*8;\
                    char _lb[4];int _n=_sc+_li+1;_lb[2]='0'+_n%10;_n/=10;_lb[1]='0'+_n%10;_n/=10;_lb[0]='0'+_n;_lb[3]=0;\
                    gfx_print(_ln_x+2,_ly,0x3A4A6A,_lb+(_lb[0]=='0'?1:0));\
                    if((_sc+_li)==code_cy[_fi2]){gfx_rect(_lx,_ly,_ew2-_ln_w-2,8,0x0E0E2E);}\
                    studio_print_hl(_fi2,code_buf[_fi2][_sc+_li],_lx,_ly);\
                }\
                {int _cy2=code_cy[_fi2],_cx=code_cx[_fi2];\
                if(_cy2>=_sc&&_cy2<_sc+_vis){\
                    gfx_rect(_code_x+_cx*8,_code_y+(_cy2-_sc)*8,6,8,0xFFFFFF);\
                }}\
                /* Output panel */\
                if (studio_output_count > 0) {\
                    int _oy = _ey2 + _ed_h + 2;\
                    gfx_fill_round_rect(_ex2,_oy,_ew2,_out_h-2,4,0x0C0C14);\
                    gfx_round_rect(_ex2,_oy,_ew2,_out_h-2,4,0x3A5A3A);\
                    gfx_print(_ex2+4,_oy+2,0x66AA66,"Output");\
                    gfx_rect(_ex2+2,_oy+12,_ew2-4,1,0x2A4A2A);\
                    for(int _oi=0;_oi<studio_output_count&&_oi<3;_oi++){\
                        gfx_print(_ex2+4,_oy+14+_oi*12,0x88CC88,studio_output[_oi]);\
                    }\
                }\
                gfx_fill_round_rect(wx+8,wy+wh-16,ww-16,12,2,0x0C0C28);\
                {char _sb[60];int _sp=0;\
                _sb[_sp++]='L';\
                {int _cn=code_cy[studio_file]+1;int _t=100;int _pr=0;\
                while(_t>1){if(_cn>=_t||_pr){_sb[_sp++]='0'+_cn/_t%10;_pr=1;}_t/=10;}\
                _sb[_sp++]='0'+_cn%10;}\
                _sb[_sp++]=',';_sb[_sp++]=' ';_sb[_sp++]='C';_sb[_sp++]='o';_sb[_sp++]='l';_sb[_sp++]=' ';\
                {int _cn=code_cx[studio_file]+1;int _t=100;int _pr=0;\
                while(_t>1){if(_cn>=_t||_pr){_sb[_sp++]='0'+_cn/_t%10;_pr=1;}_t/=10;}\
                _sb[_sp++]='0'+_cn%10;}\
                _sb[_sp++]=' ';_sb[_sp++]='[';_sb[_sp++]='T';_sb[_sp++]='a';_sb[_sp++]='b';_sb[_sp++]=']';\
                _sb[_sp++]=' ';_sb[_sp++]='[';_sb[_sp++]='C';_sb[_sp++]='t';_sb[_sp++]='r';_sb[_sp++]='l';_sb[_sp++]='+';_sb[_sp++]='R';_sb[_sp++]=']';\
                _sb[_sp++]=' ';_sb[_sp++]='[';_sb[_sp++]='E';_sb[_sp++]='s';_sb[_sp++]='c';_sb[_sp++]=']';\
                _sb[_sp++]=0;\
                gfx_print(wx+14,wy+wh-15,0x5A6A8A,_sb);}\
            } while(0)
            // ─── Kairo Studio (9) — Full IDE ───
            if (win_type == 9) {
                // Handle close
                if (k == 27 || k == 'q') { close_win(); search_focus = 1; goto redraw_desktop; }
                // Tab: switch panel focus
                if (k == '\t') { studio_panel = !studio_panel; studio_draw_all(); continue; }
                // File selection via Up/Down in file panel
                if (studio_panel == 1) {
                    if (k == KEY_UP && studio_file > 0) { studio_file--; studio_draw_all(); continue; }
                    if (k == KEY_DOWN && studio_file < 2) { studio_file++; studio_draw_all(); continue; }
                    if (k == '\n') { studio_panel = 0; studio_draw_all(); continue; }
                    if (!k) { asm volatile("hlt"); continue; }
                    continue;
                }
                // ─── Editor panel keyboard handling ───
                int _fi = studio_file;
                // Arrow keys
                if (k == KEY_UP && code_cy[_fi] > 0) { code_cy[_fi]--;
                    if (code_cy[_fi] < code_scroll[_fi]) code_scroll[_fi] = code_cy[_fi];
                    {int _ml;for(_ml=0;code_buf[_fi][code_cy[_fi]][_ml];_ml++);if(code_cx[_fi]>_ml)code_cx[_fi]=_ml;}
                    studio_draw_all(); continue;
                }
                if (k == KEY_DOWN && code_cy[_fi] < code_lines[_fi]-1) { code_cy[_fi]++;
                    if (code_cy[_fi] >= code_scroll[_fi] + (wh-62-38)/8) code_scroll[_fi] = code_cy[_fi] - (wh-62-38)/8 + 1;
                    {int _ml;for(_ml=0;code_buf[_fi][code_cy[_fi]][_ml];_ml++);if(code_cx[_fi]>_ml)code_cx[_fi]=_ml;}
                    studio_draw_all(); continue;
                }
                if (k == KEY_LEFT && code_cx[_fi] > 0) { code_cx[_fi]--; studio_draw_all(); continue; }
                if (k == KEY_RIGHT) {
                    int _ml; for(_ml=0;code_buf[_fi][code_cy[_fi]][_ml];_ml++);
                    if (code_cx[_fi] < _ml) { code_cx[_fi]++; studio_draw_all(); continue; }
                }
                if (k == KEY_HOME) { code_cx[_fi] = 0; studio_draw_all(); continue; }
                if (k == KEY_END) { int _ml; for(_ml=0;code_buf[_fi][code_cy[_fi]][_ml];_ml++); code_cx[_fi] = _ml; studio_draw_all(); continue; }
                if (k == KEY_PGUP) {
                    int _page = (wh-62-38)/8;
                    code_cy[_fi] -= _page; if (code_cy[_fi] < 0) code_cy[_fi] = 0;
                    code_scroll[_fi] = code_cy[_fi];
                    {int _ml;for(_ml=0;code_buf[_fi][code_cy[_fi]][_ml];_ml++);if(code_cx[_fi]>_ml)code_cx[_fi]=_ml;}
                    studio_draw_all(); continue;
                }
                if (k == KEY_PGDN) {
                    int _page = (wh-62-38)/8;
                    code_cy[_fi] += _page; if (code_cy[_fi] >= code_lines[_fi]) code_cy[_fi] = code_lines[_fi]-1;
                    if (code_cy[_fi] >= code_scroll[_fi] + _page) code_scroll[_fi] = code_cy[_fi] - _page + 1;
                    {int _ml;for(_ml=0;code_buf[_fi][code_cy[_fi]][_ml];_ml++);if(code_cx[_fi]>_ml)code_cx[_fi]=_ml;}
                    studio_draw_all(); continue;
                }
                // Ctrl+R → Run via AI proxy
                if (k_ctrl && k == 'r') {
                    studio_output_count = 0;
                    // Build full code string from buffer
                    char _send[512]; int _sp = 0;
                    const char *_label = studio_fnames[studio_file];
                    while (*_label && _sp < 500) _send[_sp++] = *_label++;
                    _send[_sp++] = '|';
                    for (int _ri = 0; _ri < code_lines[_fi] && _sp < 500; _ri++) {
                        for (int _ci = 0; code_buf[_fi][_ri][_ci] && _sp < 500; _ci++) {
                            _send[_sp++] = code_buf[_fi][_ri][_ci];
                        }
                        if (_ri < code_lines[_fi]-1 && _sp < 500) _send[_sp++] = '\n';
                    }
                    _send[_sp] = 0;
                    // Send to proxy via serial
                    while (serial_available()) serial_read();
                    serial_puts("RUN|");
                    serial_puts(_send);
                    serial_write('\n');
                    // Wait for response
                    for (int _tw = 0; _tw < 30000; _tw++) {
                        if (serial_available()) break;
                        for (volatile int _d = 0; _d < 300000; _d++);
                    }
                    if (serial_available()) {
                        char _rline[200];
                        while (1) {
                            int _ri = 0;
                            while (_ri < 199) {
                                if (!serial_read_timeout(&_rline[_ri], 5000)) break;
                                if (_rline[_ri] == '\n' || _rline[_ri] == '\r') break;
                                _ri++;
                            }
                            _rline[_ri] = 0;
                            if (_rline[0]=='E'&&_rline[1]=='N'&&_rline[2]=='D'&&_rline[3]==0) break;
                            if (_rline[0]=='R'&&_rline[1]=='E'&&_rline[2]=='S'&&_rline[3]=='P'&&_rline[4]=='|') {
                                char *_rt = _rline + 5;
                                if (_rt[0] && studio_output_count < 10) {
                                    int _oi; for(_oi=0;_rt[_oi]&&_oi<59;_oi++) studio_output[studio_output_count][_oi]=_rt[_oi];
                                    studio_output[studio_output_count][_oi]=0;
                                    studio_output_count++;
                                }
                            }
                        }
                    }
                    if (studio_output_count == 0 && studio_output_count < 10) {
                        char *_msg = "[No output or proxy not connected]";
                        int _oi; for(_oi=0;_msg[_oi]&&_oi<59;_oi++) studio_output[0][_oi]=_msg[_oi];
                        studio_output[0][_oi]=0;
                        studio_output_count = 1;
                    }
                    studio_draw_all(); continue;
                }
                // Enter: split line
                if (k == '\n') {
                    if (code_lines[_fi] < 30) {
                        char *_ln = code_buf[_fi][code_cy[_fi]];
                        int _pos = code_cx[_fi], _len; for(_len=0;_ln[_len];_len++);
                        // Shift lines down
                        for (int _i = code_lines[_fi]; _i > code_cy[_fi]; _i--)
                            {int _j;for(_j=0;_j<60;_j++)code_buf[_fi][_i][_j]=code_buf[_fi][_i-1][_j];}
                        // Copy rest of line to new line
                        for (int _j = 0; _j <= _len - _pos; _j++)
                            code_buf[_fi][code_cy[_fi]+1][_j] = _ln[_pos + _j];
                        _ln[_pos] = 0;
                        code_lines[_fi]++;
                        code_cy[_fi]++; code_cx[_fi] = 0;
                        if (code_cy[_fi] >= code_scroll[_fi] + (wh-62-38)/8)
                            code_scroll[_fi] = code_cy[_fi] - (wh-62-38)/8 + 1;
                    }
                    studio_draw_all(); continue;
                }
                // Backspace
                if (k == '\b') {
                    if (code_cx[_fi] > 0) {
                        char *_ln = code_buf[_fi][code_cy[_fi]];
                        for (int _i = code_cx[_fi]-1; _ln[_i]; _i++) _ln[_i] = _ln[_i+1];
                        code_cx[_fi]--;
                    } else if (code_cy[_fi] > 0) {
                        char *_pl = code_buf[_fi][code_cy[_fi]-1];
                        char *_cl = code_buf[_fi][code_cy[_fi]];
                        int _plen; for(_plen=0;_pl[_plen];_plen++);
                        int _clen; for(_clen=0;_cl[_clen];_clen++);
                        if (_plen + _clen < 59) {
                            for (int _j = 0; _j <= _clen; _j++) _pl[_plen + _j] = _cl[_j];
                        }
                        code_cx[_fi] = _plen;
                        // Shift lines up
                        for (int _i = code_cy[_fi]; _i < code_lines[_fi]-1; _i++)
                            {int _j;for(_j=0;_j<60;_j++)code_buf[_fi][_i][_j]=code_buf[_fi][_i+1][_j];}
                        code_lines[_fi]--;
                        code_cy[_fi]--;
                        if (code_cy[_fi] < code_scroll[_fi]) code_scroll[_fi] = code_cy[_fi];
                    }
                    studio_draw_all(); continue;
                }
                // Regular character input
                if (k >= ' ' && k <= '~') {
                    char *_ln = code_buf[_fi][code_cy[_fi]];
                    int _len; for(_len=0;_ln[_len];_len++);
                    if (_len < 59) {
                        for (int _i = _len; _i >= code_cx[_fi]; _i--) _ln[_i+1] = _ln[_i];
                        _ln[code_cx[_fi]] = k;
                        code_cx[_fi]++;
                    }
                    studio_draw_all(); continue;
                }
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // KairoVM (10)
            if (win_type == 10) {
                if (k == 27 || k == 'q') { close_win(); search_focus = 1; goto redraw_desktop; }

                // --- Library view ---
                if (vm_mode == 0) {
                    if (k == 'c' && vm_count < 4) {
                        vm_mode = 1; vm_creat_step = 0; vm_creat_os = 0; vm_creat_pos = 0;
                        vm_creat_name[0]=0; vm_creat_ram=2048; vm_creat_cores=2; vm_creat_disk=32;
                        gfx_clear(0x0A0A1A);
                        gfx_fill_round_rect(8,8,w-16,32,8,0xCC4444);gfx_print_scaled(20,12,0xFFFFFF,"Create Virtual Machine",1);
                        gfx_print(20,52,0xFFFF00,"Step 1: Select Operating System");
                        gfx_fill_round_rect(20,76,260,40,6,vm_creat_os==0?0x4444AA:0x12123A);gfx_print(36,88,vm_creat_os==0?0xFFFFFF:0x8899CC,"macOS 15 Sequoia");
                        gfx_fill_round_rect(20,124,260,40,6,vm_creat_os==1?0x4488FF:0x12123A);gfx_print(36,136,vm_creat_os==1?0xFFFFFF:0x8899CC,"Windows 11 Pro");
                        gfx_fill_round_rect(20,172,260,40,6,vm_creat_os==2?0xDD8844:0x12123A);gfx_print(36,184,vm_creat_os==2?0xFFFFFF:0x8899CC,"Ubuntu 24.04 LTS");
                        gfx_print(20,226,0x4A6A8A,"[Up/Down] choose  [Enter] next  [Esc] cancel");
                        continue;
                    }
                    if (k == KEY_UP && vm_sel > 0) { vm_sel--;
                        vm_redraw(); continue;
                    }
                    if (k == KEY_DOWN && vm_sel < vm_count-1) { vm_sel++;
                        vm_redraw(); continue;
                    }
                    if (k == '\n' && vm_count > 0) {
                        vm_running[vm_sel] = !vm_running[vm_sel];
                        if (vm_running[vm_sel]) {
                            vm_mode = 2; vm_cstate[vm_sel] = 0;
                            gfx_clear(0x000000);
                            gfx_print_scaled(40,10,0xFFFFFF,"KairoVM Console",1);
                            gfx_print(20,44,0x00AA00,"[KairoVM] Starting virtual machine...");
                        }
                        vm_redraw(); continue;
                    }
                    if (k == 'd' && vm_count > 0 && vm_sel < vm_count) {
                        for (int _di=vm_sel;_di<vm_count-1;_di++){
                            for(int _dj=0;_dj<32;_dj++)vm_name[_di][_dj]=vm_name[_di+1][_dj];
                            vm_os[_di]=vm_os[_di+1];vm_ram[_di]=vm_ram[_di+1];vm_cores[_di]=vm_cores[_di+1];
                            vm_disk[_di]=vm_disk[_di+1];vm_running[_di]=vm_running[_di+1];
                        }
                        vm_count--; if (vm_sel >= vm_count && vm_sel > 0) vm_sel--;
                        vm_redraw(); continue;
                    }
                    if (!k) { asm volatile("hlt"); continue; }
                    continue;
                }

                // --- Create Wizard ---
                if (vm_mode == 1) {
                    if (k == 27) { vm_mode = 0;
                        vm_redraw(); continue;
                    }
                    if (vm_creat_step == 0) {
                        if (k == KEY_UP && vm_creat_os > 0) { vm_creat_os--;
                            gfx_fill_round_rect(20,76,260,40,6,0x12123A);gfx_print(36,88,0x8899CC,"macOS 15 Sequoia");
                            gfx_fill_round_rect(20,124,260,40,6,0x12123A);gfx_print(36,136,0x8899CC,"Windows 11 Pro");
                            gfx_fill_round_rect(20,172,260,40,6,0x12123A);gfx_print(36,184,0x8899CC,"Ubuntu 24.04 LTS");
                            gfx_fill_round_rect(20,76+vm_creat_os*48,260,40,6,vm_os_color[vm_creat_os]);gfx_print(36,88+vm_creat_os*48,0xFFFFFF,vm_os_name[vm_creat_os]);
                            continue;
                        }
                        if (k == KEY_DOWN && vm_creat_os < 2) { vm_creat_os++;
                            gfx_fill_round_rect(20,76,260,40,6,0x12123A);gfx_print(36,88,0x8899CC,"macOS 15 Sequoia");
                            gfx_fill_round_rect(20,124,260,40,6,0x12123A);gfx_print(36,136,0x8899CC,"Windows 11 Pro");
                            gfx_fill_round_rect(20,172,260,40,6,0x12123A);gfx_print(36,184,0x8899CC,"Ubuntu 24.04 LTS");
                            gfx_fill_round_rect(20,76+vm_creat_os*48,260,40,6,vm_os_color[vm_creat_os]);gfx_print(36,88+vm_creat_os*48,0xFFFFFF,vm_os_name[vm_creat_os]);
                            continue;
                        }
                        if (k == '\n') {
                            vm_creat_step = 1;
                            gfx_clear(0x0A0A1A);
                            gfx_fill_round_rect(8,8,w-16,32,8,0xCC4444);gfx_print_scaled(20,12,0xFFFFFF,"Create Virtual Machine",1);
                            gfx_print(20,52,0xFFFF00,"Step 2: Name Your VM");
                            gfx_print(20,88,0x8A9ACE,"Enter a name for the virtual machine:");
                            gfx_fill_round_rect(20,116,320,24,4,0x000000);gfx_round_rect(20,116,320,24,4,0x3A6AFF);
                            gfx_print(28,120,0x00E5FF,"> ");
                            continue;
                        }
                        if (!k) { asm volatile("hlt"); continue; }
                        continue;
                    }
                    if (vm_creat_step == 1) {
                        if (k == '\n' && vm_creat_pos > 0) {
                            vm_creat_name[vm_creat_pos] = 0;
                            vm_creat_step = 2;
                            gfx_clear(0x0A0A1A);
                            gfx_fill_round_rect(8,8,w-16,32,8,0xCC4444);gfx_print_scaled(20,12,0xFFFFFF,"Create Virtual Machine",1);
                            gfx_print(20,52,0xFFFF00,"Step 3: Configure Hardware");
                            gfx_print(30,84,0x8899CC,"RAM:");gfx_print(110,84,0xFFFFFF,"2048");gfx_print(190,84,0x6A7A9E,"MB  [1]-  [2]+");
                            gfx_print(30,108,0x8899CC,"CPUs:");gfx_print(110,108,0xFFFFFF,"2");gfx_print(190,108,0x6A7A9E,"cores  [3]-  [4]+");
                            gfx_print(30,132,0x8899CC,"Disk:");gfx_print(110,132,0xFFFFFF,"32");gfx_print(190,132,0x6A7A9E,"GB  [5]-  [6]+");
                            gfx_print(20,170,0x4A9EFF,"[Enter] create  [Esc] cancel");
                            continue;
                        }
                        if (k == '\b' && vm_creat_pos > 0) { vm_creat_pos--;
                            gfx_rect(28,120,300,14,0x000000);gfx_print(28,120,0x00E5FF,"> ");
                            for(int _cp=0;_cp<vm_creat_pos;_cp++){char _cc[2];_cc[0]=vm_creat_name[_cp];_cc[1]=0;gfx_print(44+_cp*8,120,0xFFFFFF,_cc);}
                            continue;
                        }
                        if (vm_creat_pos < 30 && k >= ' ' && k <= '~') {
                            vm_creat_name[vm_creat_pos++] = k;
                            gfx_rect(28,120,300,14,0x000000);gfx_print(28,120,0x00E5FF,"> ");
                            for(int _cp=0;_cp<vm_creat_pos;_cp++){char _cc[2];_cc[0]=vm_creat_name[_cp];_cc[1]=0;gfx_print(44+_cp*8,120,0xFFFFFF,_cc);}
                            continue;
                        }
                        if (!k) { asm volatile("hlt"); continue; }
                        continue;
                    }
                    if (vm_creat_step == 2) {
                        if (k == '1' && vm_creat_ram > 512) { vm_creat_ram -= 512;
                            gfx_rect(100,84,200,14,0x0A0A1A);gfx_print(110,84,0xFFFFFF,"");char _r8[8];int _ri2=0,_rn2=vm_creat_ram;do{_r8[_ri2++]='0'+_rn2%10;_rn2/=10;}while(_rn2);_r8[_ri2]=0;for(int _rk2=0;_rk2<_ri2/2;_rk2++){char _rt2=_r8[_rk2];_r8[_rk2]=_r8[_ri2-1-_rk2];_r8[_ri2-1-_rk2]=_rt2;}gfx_print(110,84,0xFFFFFF,_r8);continue;
                        }
                        if (k == '2' && vm_creat_ram < 16384) { vm_creat_ram += 512;
                            gfx_rect(100,84,200,14,0x0A0A1A);gfx_print(110,84,0xFFFFFF,"");char _r8[8];int _ri2=0,_rn2=vm_creat_ram;do{_r8[_ri2++]='0'+_rn2%10;_rn2/=10;}while(_rn2);_r8[_ri2]=0;for(int _rk2=0;_rk2<_ri2/2;_rk2++){char _rt2=_r8[_rk2];_r8[_rk2]=_r8[_ri2-1-_rk2];_r8[_ri2-1-_rk2]=_rt2;}gfx_print(110,84,0xFFFFFF,_r8);continue;
                        }
                        if (k == '3' && vm_creat_cores > 1) { vm_creat_cores--;
                            gfx_rect(100,108,200,14,0x0A0A1A);char _c8[2];_c8[0]='0'+vm_creat_cores%10;_c8[1]=0;gfx_print(110,108,0xFFFFFF,_c8);continue;
                        }
                        if (k == '4' && vm_creat_cores < 8) { vm_creat_cores++;
                            gfx_rect(100,108,200,14,0x0A0A1A);char _c8[2];_c8[0]='0'+vm_creat_cores%10;_c8[1]=0;gfx_print(110,108,0xFFFFFF,_c8);continue;
                        }
                        if (k == '5' && vm_creat_disk > 8) { vm_creat_disk -= 8;
                            gfx_rect(100,132,200,14,0x0A0A1A);char _dk8[8];int _di3=0,_dn3=vm_creat_disk;do{_dk8[_di3++]='0'+_dn3%10;_dn3/=10;}while(_dn3);_dk8[_di3]=0;for(int _dk3=0;_dk3<_di3/2;_dk3++){char _dt3=_dk8[_dk3];_dk8[_dk3]=_dk8[_di3-1-_dk3];_dk8[_di3-1-_dk3]=_dt3;}gfx_print(110,132,0xFFFFFF,_dk8);continue;
                        }
                        if (k == '6' && vm_creat_disk < 512) { vm_creat_disk += 8;
                            gfx_rect(100,132,200,14,0x0A0A1A);char _dk8[8];int _di3=0,_dn3=vm_creat_disk;do{_dk8[_di3++]='0'+_dn3%10;_dn3/=10;}while(_dn3);_dk8[_di3]=0;for(int _dk3=0;_dk3<_di3/2;_dk3++){char _dt3=_dk8[_dk3];_dk8[_dk3]=_dk8[_di3-1-_dk3];_dk8[_di3-1-_dk3]=_dt3;}gfx_print(110,132,0xFFFFFF,_dk8);continue;
                        }
                        if (k == '\n') {
                            int _vi = vm_count;
                            for(int _nm=0;vm_creat_name[_nm]&&_nm<31;_nm++)vm_name[_vi][_nm]=vm_creat_name[_nm];
                            vm_name[_vi][31]=0; vm_os[_vi]=vm_creat_os; vm_ram[_vi]=vm_creat_ram;
                            vm_cores[_vi]=vm_creat_cores; vm_disk[_vi]=vm_creat_disk; vm_running[_vi]=0; vm_cstate[_vi]=0;
                            vm_count++;
                            vm_mode = 0; vm_sel = _vi;
                            vm_redraw(); continue;
                        }
                        if (!k) { asm volatile("hlt"); continue; }
                        continue;
                    }
                    if (!k) { asm volatile("hlt"); continue; }
                    continue;
                }

                // --- Console view ---
                if (vm_mode == 2) {
                    int _vsel = vm_sel;
                    if (k == 27 || k == 'q') {
                        vm_running[_vsel] = 0; vm_mode = 0;
                        vm_redraw(); continue;
                    }
                    if (k == '\n' && vm_cstate[_vsel] <= 8) {
                        int _st = vm_cstate[_vsel]++;
                        gfx_rect(20,44+_st*16,w-40,14,0x000000);
                        if (_st == 0) { gfx_print(20,44,0x00AA00,"Booting VM..."); }
                        else if (_st == 1) { gfx_print(20,44,0x00AA00,"[BIOS]  KairoVM UEFI v2.0"); }
                        else if (_st == 2) { gfx_print(20,60,0x00AA00,"[BIOS]  Detected RAM: ");char _ram_s[8];int _ri4=0,_rn4=vm_ram[_vsel];do{_ram_s[_ri4++]='0'+_rn4%10;_rn4/=10;}while(_rn4);_ram_s[_ri4]=0;for(int _rk4=0;_rk4<_ri4/2;_rk4++){char _rt4=_ram_s[_rk4];_ram_s[_rk4]=_ram_s[_ri4-1-_rk4];_ram_s[_ri4-1-_rk4]=_rt4;}gfx_print(172,60,0xFFFF00,_ram_s);gfx_print(200,60,0x00AA00," MB"); }
                        else if (_st == 3) { gfx_print(20,76,0x00AA00,"[BIOS]  Detected CPU: ");char _co_s[2];_co_s[0]='0'+vm_cores[_vsel]%10;_co_s[1]=0;gfx_print(148,76,0xFFFF00,_co_s);gfx_print(164,76,0x00AA00," cores x86_64"); }
                        else if (_st == 4) { gfx_print(20,92,0x00AA00,"[BIOS]  Boot device: KairoVM Virtual Disk (");char _ds_s[8];int _di5=0,_dn5=vm_disk[_vsel];do{_ds_s[_di5++]='0'+_dn5%10;_dn5/=10;}while(_dn5);_ds_s[_di5]=0;for(int _dk5=0;_dk5<_di5/2;_dk5++){char _dt5=_ds_s[_dk5];_ds_s[_dk5]=_ds_s[_di5-1-_dk5];_ds_s[_di5-1-_dk5]=_dt5;}gfx_print(350,92,0xFFFF00,_ds_s);gfx_print(376,92,0x00AA00," GB)"); }
                        else if (_st == 5) { gfx_print(20,108,0x00AA00,"[BOOT]  Loading bootloader..."); }
                        else if (_st == 6) { gfx_print(20,124,0x00AA00,"[BOOT]  Starting ");gfx_print(100,124,vm_os_color[vm_os[_vsel]],vm_os_name[vm_os[_vsel]]);gfx_print(300,124,0x00AA00," kernel..."); }
                        else if (_st == 7) { gfx_print(20,140,0x00AA00,"[KERNEL] System initialized. Login prompt ready."); }
                        else if (_st == 8) {
                            gfx_print(20,156,0x00FF00,"");gfx_print(20,156,vm_os_color[vm_os[_vsel]],vm_os_name[vm_os[_vsel]]);gfx_print(240,156,0x00FF00," running on KairoVM");
                            gfx_print(20,180,0xFFFFFF,"VM Console active. Press [Esc] to power off.");
                        }
                        gfx_print(20,44+vm_cstate[_vsel]*16,0x00AA00,"_");
                        continue;
                    }
                    if (!k) { asm volatile("hlt"); continue; }
                    continue;
                }

                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // ─── Camera App (win_type 11) ───
            if (win_type == 11) {
                if (k == 27 || k == 'q') { close_win(); search_focus = 1; goto redraw_desktop; }

                int _cfw = ww-40, _cfh = wh-80, _cfx = wx+20, _cfy = wy+44;

                // Draw camera viewport
                gfx_fill_round_rect(_cfx,_cfy,_cfw,_cfh,6,0x000000);
                gfx_round_rect(_cfx,_cfy,_cfw,_cfh,6,camera_is_present()?0x00CC44:0x666666);

                if (camera_is_present()) {
                    // Real camera detected — show status overlay
                    gfx_print(_cfx+12, _cfy+10, 0x00CC44, "Camera: Connected");
                    gfx_print(_cfx+12, _cfy+30, 0x8A9ACE, camera_get_name());
                    gfx_print(_cfx+12, _cfy+54, camera_is_streaming()?0x00CC44:0xCCAA00,
                        camera_is_streaming() ? "Streaming: Active" : "Streaming: Idle  [S]");
                    gfx_print(_cfx+12, _cfy+80, 0x6A7A9E, "USB Video Class detected.");
                    gfx_print(_cfx+12, _cfy+_cfh-40, 0x3A4A6A, "[Esc] close");
                    if (k == 's' && !camera_is_streaming()) { camera_start_stream(); continue; }
                    if (k == 's' && camera_is_streaming()) { camera_stop_stream(); continue; }
                } else {
                    // Rec Room — DormRoom 3D
                    static int _r3d_init_done = 0;
                    static camera_t r3d_cam;
                    if (!_r3d_init_done) {
                        r3d_init(w, h);
                        r3d_cam = (camera_t){{0, 1.5f, -2.5f}, 0, 0};
                        _r3d_init_done = 1;
                    }
                    r3d_set_viewport(_cfx, _cfy, _cfw, _cfh);
                    r3d_tick();
                    r3d_draw(&r3d_cam);
                    // Handle keyboard movement
                    float _spd = 0.08f;
                    float _rot = 0.04f;
                    float _fwd = 0, _right = 0, _up = 0;
                    if (k == 'w') _fwd = _spd;
                    if (k == 's') _fwd = -_spd;
                    if (k == 'a') _right = -_spd;
                    if (k == 'd') _right = _spd;
                    if (k == 'e') _up = _spd;
                    if (k == 'q') _up = -_spd;
                    if (k == KEY_LEFT) _rot = 0.05f;
                    if (k == KEY_RIGHT) _rot = -0.05f;
                    r3d_move_camera(&r3d_cam, _fwd, _right, _up);
                    if (k == KEY_LEFT || k == KEY_RIGHT) r3d_rotate_camera(&r3d_cam, _rot, 0);
                    if (k == KEY_UP) r3d_rotate_camera(&r3d_cam, 0, -0.04f);
                    if (k == KEY_DOWN) r3d_rotate_camera(&r3d_cam, 0, 0.04f);
                    // Info overlay
                    gfx_print(_cfx+12, _cfy+10, 0xFFFFFF88, "REC ROOM — DormRoom");
                    gfx_print(_cfx+12, _cfy+_cfh-36, 0xFFFFFF66, "WASD move | Arrows look | Q/E up/down");
                }

                gfx_print(wx+16, wy+wh-24, 0x3A4A6A, "[Esc] close");
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // Settings now handled by settings_active overlay (above)

            // Kairo Player (12) — audio/visual reali
            if (win_type == 12) {
                if (k == 'p') {
                    // Stato Playing
                    gfx_rect(wx+ww-130, wy+155, 80, 12, 0x141452);
                    gfx_print(wx+ww-130, wy+155, 0x44FF44, "Playing");
                    // Blocco riproduzione con audio + visuals reali
                    for (int _f = 0; _f < 60; _f++) {
                        // Sweep audio reale 200Hz → 800Hz in 4 secondi
                        uint32_t _freq = 200 + _f * 10;
                        speaker_tone(_freq);
                        // Render frame visuale
                        for (int _y = 0; _y < 100; _y++) {
                            for (int _x = 0; _x < 320; _x++) {
                                int _v = ((_x + _f * 3) ^ (_y + _f * 5)) & 0xFF;
                                int _r = (_v * 2 > 255) ? 255 : _v * 2;
                                int _g = _v ^ 0x80;
                                int _b = (255 - _v) > 128 ? 255 : (_v + 128);
                                gfx_putpixel(wx+30 + _x, wy+70 + _y, (_r << 16) | (_g << 8) | _b);
                            }
                        }
                        // ~66ms delay per frame → 4 secondi totali
                        for (volatile int _d = 0; _d < 12000000; _d++);
                        // Controllo uscita con Esc
                        char _ck = keyboard_last_char();
                        if (_ck == 27 || _ck == 'q') break;
                    }
                    speaker_off();
                    // Stato Stopped
                    gfx_rect(wx+ww-130, wy+155, 80, 12, 0x141452);
                    gfx_print(wx+ww-130, wy+155, 0xFF6644, "Stopped");
                    continue;
                }
                if (k == 's') {
                    speaker_off();
                    gfx_rect(wx+ww-130, wy+155, 80, 12, 0x141452);
                    gfx_print(wx+ww-130, wy+155, 0xFF6644, "Stopped");
                    continue;
                }
                if (k == ' ') {
                    // Test sweep breve
                    play_sweep(300, 900, 500);
                    gfx_rect(wx+ww-130, wy+155, 80, 12, 0x141452);
                    gfx_print(wx+ww-130, wy+155, 0x44FF44, "Sweep OK");
                    continue;
                }
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // True Video (win_type == 14)
            if (win_type == 14) {
                #define TV_COUNT 5
                const char *tv_creator[TV_COUNT] = {"@kairodev","@truevision","@pixelmaster","@soundwave","@retro_os"};
                const char *tv_title[TV_COUNT] = {"Plasma — first upload!","Wave scrolling — 60fps","Tunnel vision — demo","Light interference — 4K","Aurora borealis — live"};
                uint32_t tv_colors[TV_COUNT] = {0xFF4444,0x44FF44,0x4488FF,0xFFAA00,0xBB88FF};
                static int tv_sel = 0;

                if (k == 27) { tv_sel = 0; close_win(); continue; }
                if (k == KEY_UP && tv_sel > 0) { tv_sel--; }
                if (k == KEY_DOWN && tv_sel < TV_COUNT-1) { tv_sel++; }
                if (k == '\n' || k == ' ') {
                    int _tv = tv_sel;
                    int _notes[5][8] = {
                        {262,330,392,523,392,330,262,523},
                        {294,370,440,587,440,370,294,587},
                        {330,349,392,494,440,392,349,330},
                        {349,440,523,622,523,440,349,523},
                        {392,494,587,784,587,494,392,784}
                    };
                    for (int _f = 0; _f < 56; _f++) {
                        speaker_tone(_notes[_tv][_f / 7]);
                        for (int _y = 0; _y < 100; _y += 2) {
                            for (int _x = 0; _x < 320; _x += 2) {
                                int _dx = _x - 160, _dy = _y - 50;
                                int _v; uint32_t _col = 0;
                                switch (_tv) {
                                    case 0: _v = ((_dx*_dx+_dy*_dy)/30+_f*8)&0xFF; _col=(_v<<16)|((_v*3/4)<<8)|(_v>>1); break;
                                    case 1: _v = ((_x*5+_y*3+_f*12)&0xFF); _col=((_v>>2)<<16)|(_v<<8)|((_v>>1)<<8); break;
                                    case 2: _v = ((_dx*_dx^_dy*_dy)/40+_f*6)&0xFF; _col=((_v>>1)<<16)|((_v*3/4)<<8)|_v; break;
                                    case 3: _v = (((_x+_f*4)&0x18)^((_y+_f*2)&0x18))?0xF0:0x20; _col=(_v<<16)|(_v<<8)|(_v>>2); break;
                                    case 4: _v = (((_x>>1)+_f*5)^((_y>>1)+_f*3))&0x7F; _col=((_v>>1)<<16)|((_v*3/4)<<8)|(_v<<8); break;
                                }
                                gfx_putpixel(wx+30+_x,wy+70+_y,_col);
                                gfx_putpixel(wx+31+_x,wy+70+_y,_col);
                                gfx_putpixel(wx+30+_x,wy+71+_y,_col);
                                gfx_putpixel(wx+31+_x,wy+71+_y,_col);
                            }
                        }
                        gfx_print(wx+28, wy+56, tv_colors[_tv], tv_creator[_tv]);
                        gfx_print(wx+120, wy+56, 0xFFFFFF, tv_title[_tv]);
                        for (volatile int _d = 0; _d < 12000000; _d++);
                        char _ck = keyboard_last_char();
                        if (_ck == 27 || _ck == 'q') break;
                    }
                    speaker_off();
                }

                // Draw feed
                gfx_fill_round_rect(wx+20, wy+48, ww-40, 220, 4, 0x101030);
                gfx_round_rect(wx+20, wy+48, ww-40, 220, 4, 0x3A5A8A);
                gfx_print(wx+28, wy+56, 0xFF4444, "True Video — Feed");
                for (int _vi = 0; _vi < TV_COUNT; _vi++) {
                    int _vy = wy+80 + _vi * 36;
                    if (_vi == tv_sel) gfx_fill_round_rect(wx+24, _vy-2, ww-48, 32, 4, 0x2A1A4A);
                    gfx_round_rect(wx+24, _vy-2, ww-48, 32, 2, 0x3A5A8A);
                    gfx_print(wx+32, _vy+2, tv_colors[_vi], tv_creator[_vi]);
                    gfx_print(wx+32, _vy+16, 0xFFFFFF, tv_title[_vi]);
                }
                gfx_print(wx+20, wy+wh-18, 0x3A4A6A, "[Up/Down] navigate  [Enter/Space] play  [Esc] back");
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // ─── Launchpad (win_type 15) ───
            if (win_type == 15) {
                if (k == 27) { close_win(); search_focus = 1; goto redraw_desktop; }
                if (k == KEY_UP && lp_sel_y > 0) { lp_sel_y--; draw_launch_pad(w, h, lp_sel_x, lp_sel_y); continue; }
                if (k == KEY_DOWN && lp_sel_y < 2) { lp_sel_y++; draw_launch_pad(w, h, lp_sel_x, lp_sel_y); continue; }
                if (k == KEY_UP && lp_sel_y == 0) { lp_sel_y = 2; draw_launch_pad(w, h, lp_sel_x, lp_sel_y); continue; }
                if (k == KEY_DOWN && lp_sel_y == 2) { lp_sel_y = 0; draw_launch_pad(w, h, lp_sel_x, lp_sel_y); continue; }
                if (k == KEY_LEFT && lp_sel_x > 0) { lp_sel_x--; draw_launch_pad(w, h, lp_sel_x, lp_sel_y); continue; }
                if (k == KEY_LEFT && lp_sel_x == 0) { lp_sel_x = 3; draw_launch_pad(w, h, lp_sel_x, lp_sel_y); continue; }
                if (k == KEY_RIGHT && lp_sel_x < 3) { lp_sel_x++; draw_launch_pad(w, h, lp_sel_x, lp_sel_y); continue; }
                if (k == KEY_RIGHT && lp_sel_x == 3) { lp_sel_x = 0; draw_launch_pad(w, h, lp_sel_x, lp_sel_y); continue; }
                if (k == '\n' || k == ' ') {
                    int _li = lp_sel_y * 4 + lp_sel_x;
                    if (_li < 10) {
                        close_win(); search_focus = 1;
                        _launch_app(lpg_actions[_li]);
                        continue;
                    }
                    continue;
                }
                draw_launch_pad(w, h, lp_sel_x, lp_sel_y);
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // ─── Calendar (16) ───
            if (win_type == 16) {
                if (k == 27 || k == 'q') { close_win(); search_focus = 1; goto redraw_desktop; }
                if (k == KEY_LEFT) { cal_month--; if (cal_month < 1) { cal_month = 12; cal_year--; } continue; }
                if (k == KEY_RIGHT) { cal_month++; if (cal_month > 12) { cal_month = 1; cal_year++; } continue; }
                {
                    int _dmc = set_state & 1;
                    gfx_fill_round_rect(wx+12,wy+44,ww-24,wh-56,6,_dmc?0x03030E:0x08081C);
                    const char *_mnames[] = {"January","February","March","April","May","June","July","August","September","October","November","December"};
                    char _mt[32]; int _mp=0,_mn=cal_month-1;
                    for(_mp=0;_mnames[_mn][_mp];_mp++)_mt[_mp]=_mnames[_mn][_mp];
                    _mt[_mp++]=' ';_mt[_mp++]='0'+cal_year/1000%10;_mt[_mp++]='0'+cal_year/100%10;_mt[_mp++]='0'+cal_year/10%10;_mt[_mp++]='0'+cal_year%10;_mt[_mp]=0;
                    gfx_print(wx+24,wy+52,_dmc?0x3366CC:0x4488FF,_mt);
                    gfx_print(wx+ww-60,wy+52,_dmc?0x2A3A5A:0x4A6A8A,"< >");
                    const char *_dow[] = {"Mo","Tu","We","Th","Fr","Sa","Su"};
                    for(int _di=0;_di<7;_di++)gfx_print(wx+24+_di*48,wy+76,_dmc?0x3A4A6A:0x5A6A8A,_dow[_di]);
                    // Compute days in month (simplified)
                    int _dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
                    if((cal_year%4==0&&cal_year%100)||cal_year%400==0)_dim[1]=29;
                    int _fd = (cal_year*365 + cal_year/4 - cal_year/100 + cal_year/400) % 7;
                    for(int _m=0;_m<cal_month-1;_m++)_fd += _dim[_m];
                    _fd = (_fd+1)%7; // Monday-based
                    int _dc = 1;
                    for(int _r=0;_r<6&&_dc<=_dim[cal_month-1];_r++){
                        for(int _c=0;_c<7&&_dc<=_dim[cal_month-1];_c++){
                            if(_r==0&&_c<_fd){continue;}
                            char _ds[3];_ds[0]='0'+_dc/10%10;_ds[1]='0'+_dc%10;_ds[2]=0;
                            int _ix=wx+24+_c*48,_iy=wy+94+_r*20;
                            if(_ds[0]=='0'){_ds[0]=_ds[1];_ds[1]=0;}
                            int _cur_m=0,_cur_d=0; rtc_read_date(&_cur_m,&_cur_d);
                            if(cal_month==_cur_m&&_dc==_cur_d){
                                gfx_fill_round_rect(_ix-6,_iy-2,24,16,4,0x3A6AFF);
                                gfx_print(_ix,_iy,0xFFFFFF,_ds);
                            }else{
                                gfx_print(_ix,_iy,_dmc?0x4A5A7E:0x8899CC,_ds);
                            }
                            _dc++;
                        }
                    }
                    gfx_print(wx+16,wy+wh-20,_dmc?0x18182A:0x2A3A5A,"[L/R] month  [Esc]");
                }
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // ─── Pomodoro/Timer (17) ───
            if (win_type == 17) {
                if (k == 27 || k == 'q') { close_win(); search_focus = 1; goto redraw_desktop; }
                if (k == ' ') {
                    pom_running = !pom_running;
                    if (pom_running && pom_sec == 0) pom_sec = pom_total;
                }
                if (k == 'r') { pom_running = 0; pom_sec = 0; }
                if (k == '1') { pom_total = 1500; pom_sec = pom_total; } // 25min
                if (k == '2') { pom_total = 300; pom_sec = pom_total; }  // 5min
                if (k == '3') { pom_total = 600; pom_sec = pom_total; }  // 10min
                if (pom_running) {
                    pom_ticks++;
                    if (pom_ticks % 10 == 0 && pom_sec > 0) pom_sec--;
                    if (pom_sec == 0) pom_running = 0;
                }
                {
                    int _dmc = set_state & 1;
                    gfx_fill_round_rect(wx+16,wy+50,ww-32,wh-64,6,_dmc?0x03030E:0x08081C);
                    int _m = pom_sec / 60, _s = pom_sec % 60;
                    char _ts[9];_ts[0]='0'+_m/10%10;_ts[1]='0'+_m%10;_ts[2]=':';_ts[3]='0'+_s/10%10;_ts[4]='0'+_s%10;_ts[5]=0;
                    gfx_print_scaled(wx+ww/2-80,wy+70,_dmc?0x3366CC:0x4488FF,_ts,3);
                    // Progress bar
                    if (pom_total > 0) {
                        int _pct = (pom_total - pom_sec) * 100 / pom_total;
                        gfx_rect(wx+40,wy+130,ww-80,6,0x222244);
                        gfx_rect(wx+40,wy+130,(ww-80)*_pct/100,6,0x44AA66);
                    }
                    gfx_print_scaled(wx+30,wy+152,_dmc?0x4A5A7E:0x6A8ABE,pom_running?"● Running":"○ Paused",1);
                    gfx_print(wx+24,wy+180,_dmc?0x2A3A5A:0x4A6A8A,"[Space] start/pause  [R] reset  [1]25m [2]5m [3]10m");
                    gfx_print(wx+24,wy+196,_dmc?0x1A2A4A:0x3A4A6A,"[Esc] close");
                }
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // ─── Weather (18) real API via serial proxy ───
            if (win_type == 18) {
                if (k == 27 || k == 'q') { close_win(); search_focus = 1; goto redraw_desktop; }
                static const char *_wcities[] = {"San+Francisco","New+York","Tokyo","London","Dubai","Sydney"};
                static const char *_wlabels[] = {"San Francisco","New York","Tokyo","London","Dubai","Sydney"};
                // Cached weather data
                static int _wtemps[6] = {18,24,28,15,35,22};
                static int _wcond[6] = {0,1,0,2,0,1};
                if (k == KEY_LEFT || k == KEY_UP) { wthr_sel--; if (wthr_sel < 0) wthr_sel = 5; wthr_first=0; goto _fetch_wthr; }
                if (k == KEY_RIGHT || k == KEY_DOWN) { wthr_sel++; if (wthr_sel > 5) wthr_sel = 0; wthr_first=0; goto _fetch_wthr; }
                if (wthr_first) { wthr_first = 0; goto _fetch_wthr; }
                goto _draw_wthr;
            _fetch_wthr:
                {
                    while (serial_available()) serial_read();
                    serial_puts("WTHR|"); serial_puts(_wcities[wthr_sel]); serial_write('\n');
                    for (int _tw = 0; _tw < 10000; _tw++) {
                        if (serial_available()) break;
                        for (volatile int _d = 0; _d < 100000; _d++);
                    }
                    if (serial_available()) {
                        char _wbuf[64]; int _wi = 0;
                        while (_wi < 60 && serial_available()) {
                            char _c = serial_read();
                            if (_c == '\n' || _c == '\r') break;
                            if (_c == 'R' && _c+3 < 128) { /* skip RESP| */ }
                            _wbuf[_wi++] = _c;
                        }
                        _wbuf[_wi] = 0;
                        // Parse RESP|temp|cond
                        char *_wt = _wbuf, *_wp = 0;
                        for (int _pj = 0; _wbuf[_pj]; _pj++) if (_wbuf[_pj] == '|') { _wbuf[_pj] = 0; _wp = &_wbuf[_pj+1]; break; }
                        if (_wp) { 
                            int _tn = 0; for (int _ti = 0; _wp[_ti] >= '0' && _wp[_ti] <= '9'; _ti++) _tn = _tn*10 + _wp[_ti]-'0';
                            _wtemps[wthr_sel] = _tn ? _tn : _wtemps[wthr_sel];
                            // parse condition number after second |
                            char *_wp2 = _wp; while (*_wp2 && *_wp2 != '|') _wp2++; if (*_wp2 == '|') _wp2++;
                            if (*_wp2 >= '0' && *_wp2 <= '2') _wcond[wthr_sel] = *_wp2 - '0';
                        }
                    }
                    // Drain remaining serial
                    while (serial_available()) { char _dc = serial_read(); if (_dc == 'D') break; }
                }
            _draw_wthr:
                {
                    int _dmc = set_state & 1;
                    int _cx = wx+16, _cy = wy+50, _cw = ww-32, _ch = wh-64;
                    gfx_fill_round_rect(_cx,_cy,_cw,_ch,6,_dmc?0x03030E:0x08081C);
                    gfx_print(wx+24,wy+56,_dmc?0x3A4A6A:0x5A6A8A,"City:");
                    for(int _wi=0;_wi<6;_wi++){
                        int _wy = wy+76 + _wi*24;
                        if(_wi==wthr_sel)gfx_fill_round_rect(wx+20,_wy-2,160,22,4,0x1A3A8A);
                        gfx_print(wx+24,_wy+2,_wi==wthr_sel?0xFFFFFF:0x8A9ACE,_wlabels[_wi]);
                    }
                    gfx_fill_round_rect(wx+200,wy+60,180,140,8,_dmc?0x060612:0x0A0A28);
                    gfx_print_scaled(wx+210,wy+68,_dmc?0x3366CC:0x4488FF,_wlabels[wthr_sel],1);
                    char _wt[8];int _wn=_wtemps[wthr_sel];_wt[0]='0'+_wn/10%10;_wt[1]='0'+_wn%10;_wt[2]=0;
                    if(_wt[0]=='0'){_wt[0]=_wt[1];_wt[1]=0;}
                    gfx_print_scaled(wx+220,wy+100,_wcond[wthr_sel]==2?0x4488FF:(_wcond[wthr_sel]==1?0xAAAAAA:0xFFAA44),_wt,3);
                    gfx_print_scaled(wx+260,wy+115,_dmc?0x3A4A6A:0x4A6A8A,"°C",1);
                    const char *_wsym = _wcond[wthr_sel]==2?"☂":(_wcond[wthr_sel]==1?"☁":"☀");
                    gfx_print_scaled(wx+250,wy+140,_wcond[wthr_sel]==2?0x4488FF:(_wcond[wthr_sel]==1?0xAAAAAA:0xFFAA44),_wsym,2);
                    gfx_print(wx+212,wy+180,_dmc?0x2A3A5A:0x4A6A8A,"[U/D] city [Esc]");
                }
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // ─── Disk Usage / System Monitor (19) ───
            if (win_type == 19) {
                if (k == 27 || k == 'q') { close_win(); search_focus = 1; goto redraw_desktop; }
                {
                    int _dmc = set_state & 1;
                    int _cx = wx+16, _cy = wy+50, _cw = ww-32, _ch = wh-64;
                    gfx_fill_round_rect(_cx,_cy,_cw,_ch,6,_dmc?0x03030E:0x08081C);
                    gfx_print(wx+24,wy+56,_dmc?0x3366CC:0x4488FF,"System Monitor");
                    gfx_rect(wx+20,wy+72,_cw-8,1,_dmc?0x0E0E2A:0x1A2A5A);
                    // CPU
                    gfx_print(wx+24,wy+80,_dmc?0x4A5A7E:0x6A8ABE,"CPU:  ");
                    int _cpu_pct = ((_tick * 7) % 100);
                    gfx_rect(wx+80,wy+82,_cw-100,8,0x222244);
                    gfx_rect(wx+80,wy+82,(_cw-100)*_cpu_pct/100,8,_cpu_pct>70?0xFF6644:0x44AA66);
                    char _cpu_s[4];_cpu_s[0]='0'+_cpu_pct/10%10;_cpu_s[1]='0'+_cpu_pct%10;_cpu_s[2]='%';_cpu_s[3]=0;
                    gfx_print(wx+_cw-50,wy+80,_dmc?0x4A5A7E:0x6A8ABE,_cpu_s);
                    // RAM
                    gfx_print(wx+24,wy+100,_dmc?0x4A5A7E:0x6A8ABE,"RAM:  ");
                    int _ram_pct = ((_tick * 13 + 30) % 100);
                    gfx_rect(wx+80,wy+102,_cw-100,8,0x222244);
                    gfx_rect(wx+80,wy+102,(_cw-100)*_ram_pct/100,8,_ram_pct>70?0xFF6644:0x4488FF);
                    char _ram_s[4];_ram_s[0]='0'+_ram_pct/10%10;_ram_s[1]='0'+_ram_pct%10;_ram_s[2]='%';_ram_s[3]=0;
                    gfx_print(wx+_cw-50,wy+100,_dmc?0x4A5A7E:0x6A8ABE,_ram_s);
                    // Disk
                    gfx_print(wx+24,wy+120,_dmc?0x4A5A7E:0x6A8ABE,"DISK: ");
                    int _ds_pct = ((_tick * 17 + 50) % 100);
                    gfx_rect(wx+80,wy+122,_cw-100,8,0x222244);
                    gfx_rect(wx+80,wy+122,(_cw-100)*_ds_pct/100,8,_ds_pct>80?0xFF6644:0xDDAA44);
                    char _ds_s[4];_ds_s[0]='0'+_ds_pct/10%10;_ds_s[1]='0'+_ds_pct%10;_ds_s[2]='%';_ds_s[3]=0;
                    gfx_print(wx+_cw-50,wy+120,_dmc?0x4A5A7E:0x6A8ABE,_ds_s);
                    // Network
                    gfx_print(wx+24,wy+140,_dmc?0x4A5A7E:0x6A8ABE,"NET:  ");
                    gfx_print(wx+80,wy+140,_dmc?0x3A4A6A:0x5A6A8A,"Connected via serial proxy");
                    // Uptime
                    gfx_rect(wx+20,wy+162,_cw-8,1,_dmc?0x0E0E2A:0x1A2A5A);
                    gfx_print(wx+24,wy+170,_dmc?0x3A4A6A:0x5A6A8A,"Uptime: ~");
                    int _up = _tick / 50; // approx seconds
                    char _up_s[16];int _upi=0,_upd=_up/3600;if(_upd){if(_upd/10)_up_s[_upi++]='0'+_upd/10%10;_up_s[_upi++]='0'+_upd%10;_up_s[_upi++]='h';}
                    int _upm = (_up%3600)/60;_up_s[_upi++]='0'+_upm/10%10;_up_s[_upi++]='0'+_upm%10;_up_s[_upi++]='m';_up_s[_upi]=0;
                    gfx_print(wx+110,wy+170,_dmc?0x3A4A6A:0x5A6A8A,_up_s);
                    gfx_print(wx+24,wy+196,_dmc?0x1A2A4A:0x3A4A6A,"[Esc] close");
                }
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // ─── ASCII Art Gallery (20) ───
            if (win_type == 20) {
                if (k == 27 || k == 'q') { close_win(); search_focus = 1; goto redraw_desktop; }
                if (k == KEY_UP && art_sel > 0) { art_sel--; }
                if (k == KEY_DOWN && art_sel < art_count-1) { art_sel++; }
                static const char *_arts[] = {
                    "    /\\_/\\\n   ( o.o )\n    > ^ <",
                    "  .-\"\"\"\"-.\n /        \\\n|  O    O  |\n|    __    |\n \\  \\__/  /\n  '------'",
                    "   _____\n  /     |\n  |  @  |\n  \\_____/\n  _|___|_\n  |     |\n  |_____|",
                    "  __\n /  \\\n| () |\n \\__/\n  ||\n  ||\n  **",
                    "   * *\n *  |  *\n*---@---*\n *  |  *\n   * *",
                    "  __________\n /          \\\n|  ~  ~  ~   |\n|  ~  ~  ~   |\n \\__________/"
                };
                const char *_anames[] = {"Cat","Robot","TV Bot","Heart","Compass","Waves"};
                {
                    int _dmc = set_state & 1;
                    gfx_fill_round_rect(wx+12,wy+44,ww-24,wh-56,6,_dmc?0x03030E:0x08081C);
                    gfx_print(wx+24,wy+52,_dmc?0x3366CC:0x4488FF,"ASCII Art Gallery");
                    gfx_rect(wx+20,wy+68,ww-40,1,_dmc?0x0E0E2A:0x1A2A5A);
                    // Thumbnails
                    for(int _ai=0;_ai<art_count;_ai++){
                        int _ix = wx+24+_ai*60;
                        int _sel = (_ai==art_sel);
                        if(_sel)gfx_fill_round_rect(_ix-2,wy+74,56,26,4,0x1A3A8A);
                        gfx_fill_round_rect(_ix,wy+76,52,22,4,_sel?0x4488FF:0x1A1A4A);
                        gfx_print(_ix+26-(int)_anames[_ai][0]*4,wy+83,_sel?0xFFFFFF:0x8A9ACE,_anames[_ai]);
                    }
                    // Display selected art
                    gfx_rect(wx+20,wy+108,ww-40,1,_dmc?0x0E0E2A:0x1A2A5A);
                    int _ay = wy+114;
                    const char *_art = _arts[art_sel];
                    for(int _ai=0;_art[_ai];_ai++){
                        int _ax = wx+24; while(_art[_ai]&&_art[_ai]!='\n'){char _ac[2]={_art[_ai],0};gfx_print(_ax,_ay,_dmc?0x66BBFF:0x88CCFF,_ac);_ax+=8;_ai++;}_ay+=16;
                    }
                    gfx_print(wx+20,wy+wh-18,_dmc?0x1A2A4A:0x3A4A6A,"[U/D] browse  [Esc]");
                }
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // ─── Typing Test (21) ───
            if (win_type == 21) {
                if (k == 27 || k == 'q') { close_win(); search_focus = 1; goto redraw_desktop; }
                if (k == 'r') { type_pos = 0; type_err = 0; type_ok = 0; type_start = 0; type_done = 0; }
                if (!type_done && k >= ' ' && k <= '~' && k != 27 && k != 'r') {
                    if (!type_start) { type_start = 1; type_pos = 0; type_err = 0; type_ok = 0; }
                    if (k == type_text[type_pos]) { type_ok++; type_pos++; }
                    else { type_err++; type_pos++; }
                    if (type_text[type_pos] == 0) type_done = 1;
                }
                if (k == '\b' && type_pos > 0 && !type_done) { type_pos--; }
                {
                    int _dmc = set_state & 1;
                    gfx_fill_round_rect(wx+12,wy+44,ww-24,wh-56,6,_dmc?0x03030E:0x08081C);
                    gfx_print(wx+24,wy+52,_dmc?0x3366CC:0x4488FF,"Typing Test");
                    gfx_rect(wx+20,wy+68,ww-40,1,_dmc?0x0E0E2A:0x1A2A5A);
                    // Show the text with cursor
                    int _ty = wy+80, _tx = wx+24;
                    for(int _ti=0;type_text[_ti];_ti++){
                        uint32_t _tc;
                        if(_ti < type_pos) _tc = (_ti < type_pos - type_err) ? 0x44CC44 : 0xCC4444;
                        else if(_ti == type_pos) _tc = 0xFFFFFF;
                        else _tc = _dmc?0x3A4A6A:0x5A6A8A;
                        // Handle wrapping at 38 chars per line
                        if(_ti>0&&_ti%38==0){_ty+=16;_tx=wx+24;}
                        char _c[2]={type_text[_ti],0};
                        if(_ti==type_pos){gfx_fill_round_rect(_tx-1,_ty-1,8,10,1,0x4488FF);}
                        gfx_print(_tx,_ty,_tc,_c); _tx+=8;
                    }
                    // Stats
                    gfx_rect(wx+20,wy+190,ww-40,1,_dmc?0x0E0E2A:0x1A2A5A);
                    if(type_done){
                        int _wpm = type_ok * 60 / (type_ok+type_err);
                        char _st[32];int _sp=0;
                        int _n=type_ok;_st[_sp++]='O';_st[_sp++]='K';_st[_sp++]=':';do{_st[_sp++]='0'+_n%10;_n/=10;}while(_n);_st[_sp++]=' ';
                        _n=type_err;_st[_sp++]='E';_st[_sp++]='r';_st[_sp++]='r';_st[_sp++]=':';do{_st[_sp++]='0'+_n%10;_n/=10;}while(_n);_st[_sp++]=' ';
                        _n=_wpm;_st[_sp++]='W';_st[_sp++]='P';_st[_sp++]='M';_st[_sp++]=':';do{_st[_sp++]='0'+_n%10;_n/=10;}while(_n);_st[_sp]=0;
                        gfx_print(wx+24,wy+198,_dmc?0x4A5A7E:0x8899CC,_st);
                        gfx_print(wx+24,wy+218,0x44FF44,"Done! [R] restart");
                    }else if(type_start){
                        gfx_print(wx+24,wy+198,_dmc?0x4A5A7E:0x8899CC,"Typing...  [R] restart");
                        char _st2[32];int _sp2=0;
                        int _n=type_ok;do{_st2[_sp2++]='0'+_n%10;_n/=10;}while(_n);_st2[_sp2++]='/';
                        _n=type_ok+type_err;do{_st2[_sp2++]='0'+_n%10;_n/=10;}while(_n);_st2[_sp2]=0;
                        gfx_print(wx+170,wy+198,_dmc?0x3A4A6A:0x5A6A8A,_st2);
                    }else{
                        gfx_print(wx+24,wy+198,_dmc?0x3A4A6A:0x5A6A8A,"Start typing to begin! [R] reset");
                    }
                    gfx_print(wx+20,wy+wh-18,_dmc?0x1A2A4A:0x3A4A6A,"[Esc] close");
                }
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // ─── Clipboard (22) ───
            if (win_type == 22) {
                if (k == 27 || k == 'q') { close_win(); search_focus = 1; goto redraw_desktop; }
                if (k == '\n' && clip_save_pos > 0) {
                    clip_save[clip_save_pos] = 0;
                    if (clip_count < 10) {
                        int _ci; for(_ci=0;clip_save[_ci]&&_ci<79;_ci++) clip_buf[clip_count][_ci]=clip_save[_ci];
                        clip_buf[clip_count][_ci]=0; clip_count++;
                        clip_sel = clip_count-1;
                    }
                    clip_save_pos = 0; clip_save[0] = 0;
                }
                if (k == '\b' && clip_save_pos > 0) { clip_save_pos--; clip_save[clip_save_pos] = 0; }
                if (clip_save_pos < 63 && k >= ' ' && k <= '~') { clip_save[clip_save_pos++] = k; clip_save[clip_save_pos] = 0; }
                if (k == KEY_UP && clip_sel > 0) { clip_sel--; }
                if (k == KEY_DOWN && clip_sel < clip_count-1) { clip_sel++; }
                {
                    int _dmc = set_state & 1;
                    gfx_fill_round_rect(wx+12,wy+44,ww-24,wh-56,6,_dmc?0x03030E:0x08081C);
                    gfx_print(wx+24,wy+52,_dmc?0x3366CC:0x4488FF,"Clipboard");
                    gfx_rect(wx+20,wy+68,ww-40,1,_dmc?0x0E0E2A:0x1A2A5A);
                    // List
                    int _cy = wy+76;
                    for(int _ci=0;_ci<clip_count&&_ci<5;_ci++){
                        int _sel = (_ci==clip_sel);
                        if(_sel)gfx_fill_round_rect(wx+16,_cy-2,ww-32,20,4,0x1A3A8A);
                        char _cn[4];_cn[0]='0'+(_ci+1);_cn[1]='.';_cn[2]=' ';_cn[3]=0;
                        gfx_print(wx+24,_cy+2,_sel?0xFFFFFF:0x8899CC,_cn);
                        char _ct[44];int _cti;for(_cti=0;clip_buf[_ci][_cti]&&_cti<38;_cti++)_ct[_cti]=clip_buf[_ci][_cti];
                        if(clip_buf[_ci][_cti]){_ct[_cti++]='.';_ct[_cti++]='.';_ct[_cti++]='.';}_ct[_cti]=0;
                        gfx_print(wx+48,_cy+2,_sel?0xFFFFFF:0x8899CC,_ct);
                        _cy += 22;
                    }
                    if(clip_count==0)gfx_print(wx+40,wy+80,_dmc?0x3A4A6A:0x5A6A8A,"Empty - add text below");
                    // Input
                    gfx_rect(wx+20,wy+180,ww-40,1,_dmc?0x0E0E2A:0x1A2A5A);
                    gfx_print(wx+24,wy+190,_dmc?0x3A4A6A:0x5A6A8A,"New entry:");
                    gfx_fill_round_rect(wx+24,wy+208,ww-48,22,4,0x000000);
                    gfx_round_rect(wx+24,wy+208,ww-48,22,4,_dmc?0x2A3A5A:0x3A6AFF);
                    gfx_print(wx+30,wy+212,_dmc?0x6677AA:0x88AACC,"> ");
                    char _cb[54];int _cbi;for(_cbi=0;clip_save[_cbi]&&_cbi<50;_cbi++)_cb[_cbi]=clip_save[_cbi];_cb[_cbi]=0;
                    gfx_print(wx+50,wy+212,0xFFFFFF,_cb);
                    gfx_print(wx+24,wy+240,_dmc?0x1A2A4A:0x3A4A6A,"[Enter] save  [U/D] browse  [Esc]");
                }
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // ─── File Manager (23) ───
            if (win_type == 23) {
                if (k == 27 || k == 'q') { close_win(); search_focus = 1; goto redraw_desktop; }
                if (k == KEY_UP && fm_sel > 0) { fm_sel--; if (fm_sel < fm_scroll) fm_scroll = fm_sel; }
                if (k == KEY_DOWN && fm_sel < FM_COUNT-1) { fm_sel++; if (fm_sel > fm_scroll+5) fm_scroll = fm_sel-5; }
                if (k == '\n' && fm_is_dir[fm_sel]) {
                    fm_open = !fm_open; // toggle between root and subfolder
                }
                {
                    int _dmc = set_state & 1;
                    gfx_fill_round_rect(wx+12,wy+44,ww-24,wh-56,6,_dmc?0x03030E:0x08081C);
                    gfx_print(wx+24,wy+52,_dmc?0x3366CC:0x4488FF,"File Manager");
                    gfx_rect(wx+20,wy+68,ww-40,1,_dmc?0x0E0E2A:0x1A2A5A);
                    // Path
                    gfx_print(wx+24,wy+76,_dmc?0x3A4A6A:0x5A6A8A,fm_open?"/home/user/Documents/ >":"/ >");
                    // File list
                    for(int _fi=0;_fi<FM_COUNT;_fi++){
                        int _fy = wy+96+_fi*22;
                        if(_fi < fm_scroll || _fi > fm_scroll+5) continue;
                        int _sel = (_fi==fm_sel);
                        if(_sel)gfx_fill_round_rect(wx+16,_fy-2,ww-32,20,4,_dmc?0x12122A:0x1A3A8A);
                        // Icon
                        if(fm_is_dir[_fi]){
                            gfx_fill_round_rect(wx+22,_fy+2,12,10,2,fm_open?0xCCAA44:0x88AACC);
                            gfx_rect(wx+22,_fy+4,12,2,_sel?0xFFFFFF:(_dmc?0x4A5A7E:0x6A8ABE));
                        } else {
                            gfx_rect(wx+22,_fy+3,10,12,_sel?0xFFFFFF:(_dmc?0x4A5A7E:0x8899CC));
                        }
                        gfx_print(wx+42,_fy+2,_sel?0xFFFFFF:0x8A9ACE,fm_files[_fi]);
                        if(!fm_is_dir[_fi])gfx_print(wx+ww-60,_fy+2,_dmc?0x2A3A5A:0x4A6A8A,fm_sizes[_fi]);
                    }
                    // Scrollbar indicatore
                    {
                        int _fmh = 96+6*22;
                        int _mmax = FM_COUNT-6; if(_mmax<0)_mmax=0;
                        gfx_fill_round_rect(wx+ww-22, wy+96, 5, _fmh, 2, _dmc?0x0E0E2A:0x1A1A4A);
                        if(_mmax>0){
                            int _thumb = _fmh/(_mmax+1); if(_thumb<12)_thumb=12;
                            int _ty = wy+96 + fm_scroll*(_fmh-_thumb)/_mmax;
                            gfx_fill_round_rect(wx+ww-22, _ty, 5, _thumb, 2, 0x4A6ADF);
                        }
                    }
                    gfx_print(wx+20,wy+wh-18,_dmc?0x1A2A4A:0x3A4A6A,"[U/D] nav  [Enter] folder  [Esc]");
                }
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // ─── Tetris (24) ───
            if (win_type == 24) {
                if (k == 27 || k == 'q') { close_win(); search_focus = 1; goto redraw_desktop; }
                static const uint8_t _ts[7][2][4] = {
                    {{0x00,0x0F,0x00,0x00},{0x04,0x04,0x04,0x04}},
                    {{0x06,0x06,0x00,0x00},{0x06,0x06,0x00,0x00}},
                    {{0x04,0x0E,0x00,0x00},{0x04,0x06,0x04,0x00}},
                    {{0x06,0x0C,0x00,0x00},{0x04,0x06,0x02,0x00}},
                    {{0x0C,0x06,0x00,0x00},{0x04,0x0C,0x08,0x00}},
                    {{0x08,0x0E,0x00,0x00},{0x06,0x04,0x04,0x00}},
                    {{0x02,0x0E,0x00,0x00},{0x04,0x04,0x06,0x00}},
                };
                static const uint32_t _tc[8] = {0,0x44FFEE,0xEEEE44,0xBB44EE,0x44EE44,0xEE4444,0x4444EE,0xEE8844};
                int _lock = 0;
                if (!tet_gameover) {
                    if (k == KEY_LEFT) {
                        int _ok = 1;
                        for(int _i=0;_i<4&&_ok;_i++){uint8_t _rw=_ts[tet_piece][tet_rot][_i];for(int _j=0;_j<4&&_ok;_j++){if(_rw&(1<<(3-_j))){int _gx=tet_x+_j-1,_gy=tet_y+_i;if(_gx<0||_gx>=10||_gy>=18)_ok=0;else if(_gy>=0&&tet_grid[_gx][_gy])_ok=0;}}}
                        if(_ok) tet_x--;
                    }
                    if (k == KEY_RIGHT) {
                        int _ok = 1;
                        for(int _i=0;_i<4&&_ok;_i++){uint8_t _rw=_ts[tet_piece][tet_rot][_i];for(int _j=0;_j<4&&_ok;_j++){if(_rw&(1<<(3-_j))){int _gx=tet_x+_j+1,_gy=tet_y+_i;if(_gx<0||_gx>=10||_gy>=18)_ok=0;else if(_gy>=0&&tet_grid[_gx][_gy])_ok=0;}}}
                        if(_ok) tet_x++;
                    }
                    if (k == KEY_DOWN) {
                        int _ok = 1;
                        for(int _i=0;_i<4&&_ok;_i++){uint8_t _rw=_ts[tet_piece][tet_rot][_i];for(int _j=0;_j<4&&_ok;_j++){if(_rw&(1<<(3-_j))){int _gx=tet_x+_j,_gy=tet_y+_i+1;if(_gx<0||_gx>=10||_gy>=18)_ok=0;else if(_gy>=0&&tet_grid[_gx][_gy])_ok=0;}}}
                        if(_ok){tet_y++;tet_drop=0;}
                    }
                    if (k == KEY_UP) {
                        int _nr = (tet_rot+1)&1, _ok = 1;
                        for(int _i=0;_i<4&&_ok;_i++){uint8_t _rw=_ts[tet_piece][_nr][_i];for(int _j=0;_j<4&&_ok;_j++){if(_rw&(1<<(3-_j))){int _gx=tet_x+_j,_gy=tet_y+_i;if(_gx<0||_gx>=10||_gy>=18)_ok=0;else if(_gy>=0&&tet_grid[_gx][_gy])_ok=0;}}}
                        if(_ok) tet_rot = _nr;
                    }
                    if (k == ' ') {
                        while(1){
                            int _ok=1;
                            for(int _i=0;_i<4&&_ok;_i++){uint8_t _rw=_ts[tet_piece][tet_rot][_i];for(int _j=0;_j<4&&_ok;_j++){if(_rw&(1<<(3-_j))){int _gx=tet_x+_j,_gy=tet_y+_i+1;if(_gx<0||_gx>=10||_gy>=18)_ok=0;else if(_gy>=0&&tet_grid[_gx][_gy])_ok=0;}}}
                            if(!_ok)break;
                            tet_y++;
                        }
                        _lock=1;
                    }
                    tet_drop++;
                    if (tet_drop >= 50) {
                        tet_drop = 0;
                        int _ok=1;
                        for(int _i=0;_i<4&&_ok;_i++){uint8_t _rw=_ts[tet_piece][tet_rot][_i];for(int _j=0;_j<4&&_ok;_j++){if(_rw&(1<<(3-_j))){int _gx=tet_x+_j,_gy=tet_y+_i+1;if(_gx<0||_gx>=10||_gy>=18)_ok=0;else if(_gy>=0&&tet_grid[_gx][_gy])_ok=0;}}}
                        if(_ok) tet_y++;
                        else _lock=1;
                    }
                }
                if (_lock && !tet_gameover) {
                    for(int _i=0;_i<4;_i++){uint8_t _rw=_ts[tet_piece][tet_rot][_i];for(int _j=0;_j<4;_j++){if(_rw&(1<<(3-_j))){int _gx=tet_x+_j,_gy=tet_y+_i;if(_gx>=0&&_gx<10&&_gy>=0&&_gy<18)tet_grid[_gx][_gy]=tet_piece+1;}}}
                    int _cl=0;
                    for(int _y=17;_y>=0;_y--){int _fl=1;for(int _x=0;_x<10;_x++){if(!tet_grid[_x][_y]){_fl=0;break;}}if(_fl){_cl++;for(int _yy=_y;_yy>0;_yy--)for(int _x=0;_x<10;_x++)tet_grid[_x][_yy]=tet_grid[_x][_yy-1];for(int _x=0;_x<10;_x++)tet_grid[_x][0]=0;_y++;}}
                    if(_cl){tet_lines+=_cl;tet_score+=_cl*_cl*100;}
                    tet_piece=tet_next;tet_next=((_tick*7+_tick/3)%7+7)%7;tet_rot=0;tet_x=3;tet_y=0;
                    int _go=0;for(int _i=0;_i<4&&!_go;_i++){uint8_t _rw=_ts[tet_piece][tet_rot][_i];for(int _j=0;_j<4&&!_go;_j++){if(_rw&(1<<(3-_j))){int _gx=tet_x+_j,_gy=tet_y+_i;if(_gx<0||_gx>=10||_gy>=18)_go=1;else if(_gy>=0&&tet_grid[_gx][_gy])_go=1;}}}
                    if(_go)tet_gameover=1;
                }
                if (k == 'r' && tet_gameover) {
                    tet_score=0;tet_lines=0;tet_gameover=0;tet_drop=0;
                    for(int _ty=0;_ty<18;_ty++)for(int _tx=0;_tx<10;_tx++)tet_grid[_tx][_ty]=0;
                    tet_next=((_tick*7+_tick/3)%7+7)%7;tet_piece=((_tick*5+_tick/2)%7+7)%7;tet_rot=0;tet_x=3;tet_y=0;
                }
                {
                    int _dmc = set_state & 1;
                    gfx_fill_round_rect(wx+12,wy+44,ww-24,wh-56,6,_dmc?0x03030E:0x08081C);
                    gfx_print(wx+24,wy+52,_dmc?0x3366CC:0x4488FF,"Tetris");
                    gfx_rect(wx+20,wy+68,ww-40,1,_dmc?0x0E0E2A:0x1A2A5A);
                    int _bx=wx+24,_by=wy+78,_sz=10;
                    gfx_fill_round_rect(_bx-2,_by-2,10*_sz+4,18*_sz+4,4,0x000000);
                    for(int _y=0;_y<18;_y++){for(int _x=0;_x<10;_x++){int _v=tet_grid[_x][_y];if(_v){uint32_t _c=_tc[_v];gfx_rect(_bx+_x*_sz,_by+_y*_sz,_sz,_sz,_c);gfx_rect(_bx+_x*_sz+1,_by+_y*_sz+1,_sz-2,_sz-2,((_c&0xFEFEFE)>>1)+0x222222);}else{gfx_rect(_bx+_x*_sz,_by+_y*_sz,_sz,_sz,0x0A0A20);gfx_rect(_bx+_x*_sz+1,_by+_y*_sz+1,_sz-2,_sz-2,0x111133);}}}
                    if(!tet_gameover){for(int _i=0;_i<4;_i++){uint8_t _rw=_ts[tet_piece][tet_rot][_i];for(int _j=0;_j<4;_j++){if(_rw&(1<<(3-_j))){int _gx=tet_x+_j,_gy=tet_y+_i;if(_gx>=0&&_gx<10&&_gy>=0&&_gy<18){uint32_t _c=_tc[tet_piece+1];gfx_rect(_bx+_gx*_sz,_by+_gy*_sz,_sz,_sz,_c);gfx_rect(_bx+_gx*_sz+1,_by+_gy*_sz+1,_sz-2,_sz-2,((_c&0xFEFEFE)>>1)+0x222222);}}}}}
                    gfx_print(wx+132,wy+78,_dmc?0x4A5A7E:0x6A8ABE,"NEXT");
                    for(int _i=0;_i<4;_i++){uint8_t _rw=_ts[tet_next][0][_i];for(int _j=0;_j<4;_j++){if(_rw&(1<<(3-_j))){uint32_t _c=_tc[tet_next+1];gfx_rect(wx+142+_j*12,wy+94+_i*12,12,12,_c);gfx_rect(wx+142+_j*12+1,wy+94+_i*12+1,10,10,((_c&0xFEFEFE)>>1)+0x222222);}}}
                    gfx_print(wx+132,wy+150,_dmc?0x4A5A7E:0x6A8ABE,"SCORE");
                    char _ss[16];int _si=0,_sn=tet_score;do{_ss[_si++]='0'+_sn%10;_sn/=10;}while(_sn);_ss[_si]=0;for(int _sk=0;_sk<_si/2;_sk++){char _st=_ss[_sk];_ss[_sk]=_ss[_si-1-_sk];_ss[_si-1-_sk]=_st;}
                    gfx_print(wx+132,wy+166,_dmc?0x88BBFF:0xFFFFFF,_ss);
                    gfx_print(wx+132,wy+190,_dmc?0x4A5A7E:0x6A8ABE,"LINES");
                    char _ls[16];_si=0;_sn=tet_lines;do{_ls[_si++]='0'+_sn%10;_sn/=10;}while(_sn);_ls[_si]=0;for(int _sk=0;_sk<_si/2;_sk++){char _st=_ls[_sk];_ls[_sk]=_ls[_si-1-_sk];_ls[_si-1-_sk]=_st;}
                    gfx_print(wx+132,wy+206,_dmc?0x88BBFF:0xFFFFFF,_ls);
                    if(tet_gameover){gfx_fill_round_rect(wx+50,wy+120,ww-100,50,8,0x440000);gfx_round_rect(wx+50,wy+120,ww-100,50,8,0xFF4444);gfx_print_scaled(wx+80,wy+128,0xFF6666,"GAME OVER",2);gfx_print(wx+68,wy+152,0xFF8888,"[R] restart  [Esc] exit");}
                    gfx_print(wx+20,wy+wh-18,_dmc?0x1A2A4A:0x3A4A6A,"[Arrows] move/rot  [Space] drop  [R] restart");
                }
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // ─── Kairo Games (25) ───
            if (win_type == 25) {
                if (k == 27 || k == 'q') { close_win(); search_focus = 1; goto redraw_desktop; }
                int _dmc = set_state & 1;
                int _gx = wx+16, _gy = wy+50, _gw = ww-32;
                gfx_rect(_gx,_gy,_gw,wh-60,_dmc?0x03030E:0x08081C);
                gfx_print(_gx+8,_gy+4,0xCC44AA,"Kairo Games");_gy+=28;
                gfx_rect(_gx+4,_gy-4,_gw-8,1,0x2A2A5A);
                int _tw = (_gw-20)/2, _th = 80, _tgx = _gx+4;
                // Tile 1: Tetris
                int _t1x=_tgx,_t1y=_gy;
                gfx_fill_round_rect(_t1x,_t1y,_tw,_th,8,0x1A0A3A);
                gfx_round_rect(_t1x,_t1y,_tw,_th,8,0xBB44EE);
                gfx_fill_round_rect(_t1x+8,_t1y+8,16,16,2,0x44FFEE);
                gfx_fill_round_rect(_t1x+28,_t1y+8,16,16,2,0xEEEE44);
                gfx_fill_round_rect(_t1x+8,_t1y+28,36,16,2,0xBB44EE);
                gfx_print_scaled(_t1x+8,_t1y+52,0xFFFFFF,"TETRIS",1);
                gfx_print(_t1x+8,_t1y+68,0x6A6A9A,"[1]");
                // Tile 2: Snake
                int _t2x=_tgx+_tw+12,_t2y=_gy;
                gfx_fill_round_rect(_t2x,_t2y,_tw,_th,8,0x0A1A0A);
                gfx_round_rect(_t2x,_t2y,_tw,_th,8,0x44CC44);
                gfx_fill_round_rect(_t2x+12,_t2y+12,8,8,2,0x44CC44);
                gfx_fill_round_rect(_t2x+22,_t2y+12,8,8,2,0x44CC44);
                gfx_fill_round_rect(_t2x+32,_t2y+12,8,8,2,0x44CC44);
                gfx_fill_round_rect(_t2x+32,_t2y+22,8,8,2,0x44CC44);
                gfx_fill_round_rect(_t2x+32,_t2y+32,8,8,2,0x44CC44);
                gfx_fill_round_rect(_t2x+42,_t2y+32,8,8,2,0xFF4444);
                gfx_print_scaled(_t2x+10,_t2y+52,0xFFFFFF,"SNAKE",1);
                gfx_print(_t2x+10,_t2y+68,0x6A6A9A,"[2]");
                // Placeholder tile 3
                int _t3x=_tgx,_t3y=_gy+_th+12;
                gfx_fill_round_rect(_t3x,_t3y,_tw,_th,8,_dmc?0x0A0A1A:0x101030);
                gfx_round_rect(_t3x,_t3y,_tw,_th,8,_dmc?0x1A1A3A:0x3A3A5A);
                gfx_print_scaled(_t3x+20,_t3y+26,_dmc?0x2A2A4A:0x5A5A7A,"??",2);
                gfx_print(_t3x+12,_t3y+64,_dmc?0x1A1A3A:0x3A3A5A,"Coming Soon");
                // Placeholder tile 4
                int _t4x=_tgx+_tw+12,_t4y=_gy+_th+12;
                gfx_fill_round_rect(_t4x,_t4y,_tw,_th,8,_dmc?0x0A0A1A:0x101030);
                gfx_round_rect(_t4x,_t4y,_tw,_th,8,_dmc?0x1A1A3A:0x3A3A5A);
                gfx_print_scaled(_t4x+20,_t4y+26,_dmc?0x2A2A4A:0x5A5A7A,"??",2);
                gfx_print(_t4x+12,_t4y+64,_dmc?0x1A1A3A:0x3A3A5A,"Coming Soon");
                gfx_print(_gx+8,wy+wh-18,0x3A4A6A,"[1-4] launch  [Esc] back");
                if (k == '1') {
                    close_win(); search_focus = 1;
                    open_app(24); draw_mac_title("Tetris");
                    tet_score=0;tet_lines=0;tet_gameover=0;tet_drop=0;
                    for(int _ty=0;_ty<18;_ty++)for(int _tx=0;_tx<10;_tx++)tet_grid[_tx][_ty]=0;
                    tet_next=((_tick*7+_tick/3)%7+7)%7;tet_piece=((_tick*5+_tick/2)%7+7)%7;tet_rot=0;tet_x=3;tet_y=0;
                    continue;
                }
                if (k == '2') {
                    close_win(); search_focus = 1;
                    open_app(26); draw_mac_title("Snake");
                    snake_len=3;snake_dir=0;snake_score=0;snake_gameover=0;snake_drop=0;
                    snake_body[0][0]=5;snake_body[0][1]=9;
                    snake_body[1][0]=4;snake_body[1][1]=9;
                    snake_body[2][0]=3;snake_body[2][1]=9;
                    snake_food_x=8;snake_food_y=8;
                    continue;
                }
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // ─── Snake (26) ───
            if (win_type == 26) {
                if (k == 27 || k == 'q') { close_win(); search_focus = 1; goto redraw_desktop; }
                int _dmc = set_state & 1;
                if (!snake_gameover) {
                    if (k == KEY_UP && snake_dir != 2) snake_dir = 0;
                    if (k == KEY_DOWN && snake_dir != 0) snake_dir = 2;
                    if (k == KEY_LEFT && snake_dir != 1) snake_dir = 3;
                    if (k == KEY_RIGHT && snake_dir != 3) snake_dir = 1;
                    snake_drop++;
                    if (snake_drop >= 20) {
                        snake_drop = 0;
                        int _hx = snake_body[0][0], _hy = snake_body[0][1];
                        if (snake_dir == 0) _hy--;
                        if (snake_dir == 1) _hx++;
                        if (snake_dir == 2) _hy++;
                        if (snake_dir == 3) _hx--;
                        if (_hx < 0 || _hx >= 20 || _hy < 0 || _hy >= 20) {
                            snake_gameover = 1;
                        } else {
                            int _hit = 0;
                            for (int _si = 0; _si < snake_len && !_hit; _si++)
                                if (snake_body[_si][0] == _hx && snake_body[_si][1] == _hy) _hit = 1;
                            if (_hit) { snake_gameover = 1; }
                            else {
                                for (int _si = snake_len; _si > 0; _si--) {
                                    snake_body[_si][0] = snake_body[_si-1][0];
                                    snake_body[_si][1] = snake_body[_si-1][1];
                                }
                                snake_body[0][0] = _hx;
                                snake_body[0][1] = _hy;
                                if (_hx == snake_food_x && _hy == snake_food_y) {
                                    snake_len++;
                                    snake_score += 10;
                                    snake_body[snake_len][0] = snake_body[snake_len-1][0];
                                    snake_body[snake_len][1] = snake_body[snake_len-1][1];
                                    int _ok = 0;
                                    while (!_ok) {
                                        snake_food_x = (_tick*13 + snake_len*7) % 20;
                                        snake_food_y = (_tick*17 + snake_score*3) % 20;
                                        _ok = 1;
                                        for (int _si = 0; _si < snake_len; _si++)
                                            if (snake_body[_si][0] == snake_food_x && snake_body[_si][1] == snake_food_y) _ok = 0;
                                    }
                                }
                            }
                        }
                    }
                }
                if (k == 'r' && snake_gameover) {
                    snake_len=3;snake_dir=0;snake_score=0;snake_gameover=0;snake_drop=0;
                    snake_body[0][0]=5;snake_body[0][1]=9;
                    snake_body[1][0]=4;snake_body[1][1]=9;
                    snake_body[2][0]=3;snake_body[2][1]=9;
                    snake_food_x=8;snake_food_y=8;
                }
                {
                    gfx_fill_round_rect(wx+12,wy+44,ww-24,wh-56,6,_dmc?0x03030E:0x08081C);
                    gfx_print(wx+24,wy+52,_dmc?0x3366CC:0x4488FF,"Snake");
                    gfx_rect(wx+20,wy+68,ww-40,1,_dmc?0x0E0E2A:0x1A2A5A);
                    int _bs = (wh-90)/20; if (_bs > 14) _bs = 14;
                    int _bw = 20*_bs, _bh = 20*_bs;
                    int _bx = wx+(ww-_bw)/2, _by = wy+78;
                    gfx_fill_round_rect(_bx-2,_by-2,_bw+4,_bh+4,4,0x000000);
                    for (int _sy = 0; _sy < 20; _sy++) {
                        for (int _sx = 0; _sx < 20; _sx++) {
                            int _is_head = (snake_body[0][0] == _sx && snake_body[0][1] == _sy);
                            int _is_snake = _is_head;
                            for (int _si = 1; _si < snake_len && !_is_snake; _si++) {
                                if (snake_body[_si][0] == _sx && snake_body[_si][1] == _sy) _is_snake = 1;
                            }
                            if (_is_snake) {
                                uint32_t _sc = _is_head ? 0x66FF66 : 0x44CC44;
                                gfx_rect(_bx+_sx*_bs,_by+_sy*_bs,_bs,_bs,_sc);
                                gfx_rect(_bx+_sx*_bs+1,_by+_sy*_bs+1,_bs-2,_bs-2,((_sc&0xFEFEFE)>>1)+0x222222);
                            } else if (_sx == snake_food_x && _sy == snake_food_y) {
                                gfx_fill_round_rect(_bx+_sx*_bs+1,_by+_sy*_bs+1,_bs-2,_bs-2,2,0xFF4444);
                            } else {
                                gfx_rect(_bx+_sx*_bs,_by+_sy*_bs,_bs,_bs,0x0A0A20);
                                gfx_rect(_bx+_sx*_bs+1,_by+_sy*_bs+1,_bs-2,_bs-2,0x111133);
                            }
                        }
                    }
                    char _sb[16];int _si=0,_sn=snake_score;
                    do{_sb[_si++]='0'+_sn%10;_sn/=10;}while(_sn);_sb[_si]=0;
                    for(int _sk=0;_sk<_si/2;_sk++){char _st=_sb[_sk];_sb[_sk]=_sb[_si-1-_sk];_sb[_si-1-_sk]=_st;}
                    gfx_print(wx+ww-100,wy+78,_dmc?0x4A5A7E:0x6A8ABE,"SCORE");
                    gfx_print(wx+ww-100,wy+96,_dmc?0x88BBFF:0xFFFFFF,_sb);
                    if(snake_gameover){gfx_fill_round_rect(wx+50,wy+120,ww-100,50,8,0x440000);gfx_round_rect(wx+50,wy+120,ww-100,50,8,0xFF4444);gfx_print_scaled(wx+70,wy+128,0xFF6666,"GAME OVER",2);gfx_print(wx+68,wy+152,0xFF8888,"[R] restart  [Esc] exit");}
                    gfx_print(wx+20,wy+wh-18,_dmc?0x1A2A4A:0x3A4A6A,"[Arrows] move  [R] restart");
                }
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // ─── System Transfer (27) ───
            if (win_type == 27) {
                int st_close = 0;
                handle_system_transfer_key(w, h, k, &st_close);
                if (st_close) { close_win(); search_focus = 1; goto redraw_desktop; }
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // ─── Mic Test (29) — microphone level meter ───
            if (win_type == 29) {
                if (k == 27 || k == 'q') { ac97_stop_capture(); close_win(); search_focus = 1; goto redraw_desktop; }
                int _mbw = 300, _mbh = 20, _mbx = w/2 - _mbw/2, _mby = h/2 - 40;
                gfx_fill_round_rect(wx+16,wy+50,ww-32,wh-70,8,0x0E0E28);
                gfx_print_scaled(wx+ww/2-60,wy+60,ACCENT,"Mic Test",2);
                int _lev = ac97_capture_is_active() ? ac97_capture_level() : 0;
                // Animate a simulated meter when there is no real mic input (silence)
                if (_lev < 300) {
                    _lev = ((_tick * 37 + (_tick/3)*19) % 30000);
                    if (_lev > 25000) _lev = 30000 - (_lev-25000);
                    if (_lev < 500) _lev = 500;
                    gfx_print(wx+ww/2-80,_mby+50,0xFFAA44,
                        ac97_capture_is_active() ? "No mic input - demo meter" : "Demo mode (no capture)");
                }
                int _pct = _lev * 100 / 32768;
                if (_pct > 100) _pct = 100;
                gfx_rect(_mbx,_mby,_mbw,_mbh,0x1A1A3A);
                gfx_rect(_mbx,_mby,_mbw*_pct/100,_mbh,_pct>70?0xFF4444:_pct>30?0xFFAA44:0x44CC44);
                gfx_round_rect(_mbx,_mby,_mbw,_mbh,2,0x3A5A8A);
                char _lb[8]; int _li=0,_ln=_pct; do{_lb[_li++]='0'+_ln%10;_ln/=10;}while(_ln);_lb[_li]='%';_lb[_li+1]=0;
                for(int _lk=0;_lk<_li/2;_lk++){char _lt=_lb[_lk];_lb[_lk]=_lb[_li-1-_lk];_lb[_li-1-_lk]=_lt;}
                gfx_print(w/2-16,_mby+_mbh+8,_pct>70?0xFF6666:0x88BBCC,_lb);
                gfx_print(wx+16,wy+wh-24,0x3A4A6A,"[Esc] close");
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // ─── Viteza Wii (28) — fullscreen Wii-style OS ───
            if (win_type == 28) {
                if (k == 27) { _wii_boot = 0; close_win(); search_focus = 1; goto redraw_desktop; }
                // Draw fullscreen — covers the window frame entirely
                if (_wii_boot) {
                    // ─── Wii BIOS screen ───
                    gfx_clear(0x000000);
                    gfx_print_scaled(w/2-80, h/2-40, 0xFFFFFF, "VITEZA Wii", 4);
                    gfx_print_scaled(w/2-90, h/2+30, 0x6688AA, "System BIOS v1.0", 1);
                    // Loading bar
                    int _lbx = w/2-120, _lby = h/2+70, _lbw = 240, _lbh = 8;
                    gfx_rect(_lbx, _lby, _lbw, _lbh, 0x222222);
                    gfx_rect(_lbx, _lby, (_lbw*((_tick*3)%100))/100, _lbh, 0x4488FF);
                    gfx_print_scaled(w/2-90, h/2+90, 0x446688, "Press any key to continue", 1);
                    if (k) { _wii_boot = 0; goto redraw_desktop; }
                } else {
                    // ─── Wii Menu fullscreen (macOS-quality UI) ───
                    int _cols = 4, _rows = 3;
                    int _ctw = (w-80)/_cols - 20, _cth = (h-100)/_rows - 20;
                    int _mx2 = 40, _my2 = 60;
                    const char *_wn[] = {"Dino🦖","Music 🎵","Snake🐍","Pong⚡","Tetris","Maze 🗺️","Paint🎨","Camera3D","Terminal","Mic Test","ASCII Art","Player"};
                    const char *_wi[] = {"D","M","S","P","T","M","P","C","_","M","A","V"};
                    uint32_t _wcol[] = {0x66DD88,0x4488FF,0x44CCAA,0xFF6644,0x44DDFF,0x44AA66,0xFF8844,0x66DDFF,0x00CC44,0xBB88FF,0xFFAA44,0xDD44AA};
                    int _wa[] = {34,33,26,30,24,32,31,11,3,29,20,12};
                    // Fullscreen gradient sky (Wii white → light blue)
                    for (int _by = 0; _by < h; _by++) {
                        int _bt = _by*255/h;
                        int _br, _bg, _bb;
                        if (_by < h/2) {
                            _br = 235+_bt/30; _bg = 240+_bt/25; _bb = 248+_bt/20;
                        } else {
                            int _bh = _by-h/2; _bt = _bh*255/(h/2);
                            _br = 235+_bt/30- _bt/20; _bg = 240+_bt/25; _bb = 248+_bt/8;
                            if (_bb > 255) _bb = 255;
                        }
                        if (_br > 255) _br = 255; if (_bg > 255) _bg = 255;
                        gfx_rect(0, _by, w, 1, (_br<<16)|(_bg<<8)|_bb);
                    }
                    // Clouds (soft white ellipses)
                    gfx_fill_round_rect(w/4, h/3-10, w/5, h/12, 20, 0xFFFFFF44);
                    gfx_fill_round_rect(w*3/5, h/4+10, w/6, h/14, 18, 0xFFFFFF44);
                    gfx_fill_round_rect(w/3, h/2-20, w/7, h/16, 16, 0xFFFFFF33);
                    // Top bar (glass)
                    gfx_fill_round_rect(0, 0, w, 36, 8, 0xFFFFFFCC);
                    gfx_rect(0, 36, w, 1, 0x00000022);
                    gfx_print_scaled(14, 6, 0x003366, "Viteza Wii", 2);
                    int _whr = 12, _wmn = 0; rtc_read(&_whr, &_wmn);
                    char _wtbuf[8]; _wtbuf[0]='0'+_whr/10%10; _wtbuf[1]='0'+_whr%10; _wtbuf[2]=':'; _wtbuf[3]='0'+_wmn/10; _wtbuf[4]='0'+_wmn%10; _wtbuf[5]=0;
                    gfx_print_scaled(w-140, 8, 0x446688, _wtbuf, 1);
                    // Draw channel grid (macOS tiles with glass effect)
                    static int _whov = -1;
                    int _mx = mouse_get_x(), _my = mouse_get_y();
                    _whov = -1;
                    for (int _y = 0; _y < _rows; _y++) {
                        for (int _x = 0; _x < _cols; _x++) {
                            int _idx = _y*_cols+_x;
                            int _cx = _mx2+_x*(_ctw+20), _cy = _my2+_y*(_cth+20);
                            int _hov = (_mx >= _cx && _mx < _cx+_ctw && _my >= _cy && _my < _cy+_cth);
                            if (_hov) _whov = _idx;
                            // Shadow
                            gfx_fill_round_rect(_cx+4,_cy+6,_ctw,_cth,14,0x00000022);
                            // Tile body
                            uint32_t _bg = _wcol[_idx];
                            if (_hov) { _bg = ((_bg&0xFEFEFE)>>1)+0x555555; }
                            gfx_fill_round_rect(_cx,_cy,_ctw,_cth,14,_bg);
                            // Glass highlight
                            gfx_fill_round_rect(_cx+4,_cy+3,_ctw-8,_cth/3,8,0xFFFFFF33);
                            // Border glow on hover
                            if (_hov) gfx_round_rect(_cx-1,_cy-1,_ctw+2,_cth+2,15,0xFFFFFF);
                            else gfx_round_rect(_cx,_cy,_ctw,_cth,14,0x00000033);
                            // Icon letter
                            gfx_print_scaled(_cx+_ctw/2-12,_cy+_cth/2-30,0xFFFFFF,_wi[_idx],5);
                            // App name
                            int _wiy = _cy+_cth-28;
                            gfx_fill_round_rect(_cx+4,_wiy-2,_ctw-8,22,4,0x00000044);
                            gfx_print(_cx+_ctw/2-12,_wiy+2,_hov?0xFFFFFF:0xCCDDEE,_wn[_idx]);
                        }
                    }
                    gfx_print(w/2-90, h-16, 0xFFFFFF88, "[Esc] back  [Click] launch");
                    // Hand cursor override
                    gfx_fill_round_rect(_mx,_my-4,8,12,2,0xFFFFFF);
                    gfx_fill_round_rect(_mx+6,_my,6,10,2,0xFFFFFF);
                    // Handle click to launch channel
                    if (mouse_clicked() && _whov >= 0) {
                        int _act = _wa[_whov];
                        _wii_boot = 1;
                        close_win(); search_focus = 1;
                        _launch_app(_act);
                        continue;
                    }
                }
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // ─── Pong ⚡ (30) — classic arcade ───
            if (win_type == 30) {
                if (k == 27 || k == 'q') { close_win(); search_focus = 1; goto redraw_desktop; }
                int _pw = (ww-40)/2, _ph = wh-80;
                int _pfx = wx+20, _pfx2 = wx+ww-20-8;
                int _pby = wy+50, _pbh = _ph;
                // Init
                static int _pong_init = 0;
                if (!_pong_init) {
                    pong_py1 = _pby+_pbh/2-20; pong_py2 = _pby+_pbh/2-20;
                    pong_bx = wx+ww/2; pong_by = _pby+_pbh/2;
                    pong_bdx = 2; pong_bdy = 1; pong_s1 = 0; pong_s2 = 0;
                    _pong_init = 1;
                }
                // AI left paddle + player right paddle
                if (pong_bdx < 0 || (pong_bx < wx+ww/2)) {  // ball coming toward AI
                    if (pong_py1+20 < pong_by-4 && pong_py1+40 < _pby+_pbh) pong_py1 += 4;
                    if (pong_py1+20 > pong_by+4 && pong_py1 > _pby) pong_py1 -= 4;
                } else {  // ball going away, return to center
                    if (pong_py1+20 < _pby+_pbh/2-4) pong_py1 += 2;
                    if (pong_py1+20 > _pby+_pbh/2+4) pong_py1 -= 2;
                }
                if (k == KEY_UP && pong_py2 > _pby) pong_py2 -= 6;
                if (k == KEY_DOWN && pong_py2+40 < _pby+_pbh) pong_py2 += 6;
                // Ball movement (every 3 ticks)
                static int _pong_t = 0; _pong_t++;
                if (_pong_t % 3 == 0) {
                    pong_bx += pong_bdx; pong_by += pong_bdy;
                    // Top/bottom bounce
                    if (pong_by <= _pby || pong_by >= _pby+_pbh) pong_bdy = -pong_bdy;
                    // Left paddle
                    if (pong_bx <= _pfx+8 && pong_bx >= _pfx && pong_by >= pong_py1 && pong_by <= pong_py1+40) {
                        pong_bdx = -pong_bdx;
                        pong_bx = _pfx+9;
                        pong_bdy = (pong_by - (pong_py1+20)) / 6;
                        if (pong_bdy == 0) pong_bdy = 1;
                    }
                    // Right paddle
                    if (pong_bx >= _pfx2-4 && pong_bx <= _pfx2+8 && pong_by >= pong_py2 && pong_by <= pong_py2+40) {
                        pong_bdx = -pong_bdx;
                        pong_bx = _pfx2-5;
                        pong_bdy = (pong_by - (pong_py2+20)) / 6;
                        if (pong_bdy == 0) pong_bdy = 1;
                    }
                    // Score
                    if (pong_bx < _pfx-10) { pong_s2++; pong_bx = wx+ww/2; pong_by = _pby+_pbh/2; pong_bdx = -2; }
                    if (pong_bx > _pfx2+20) { pong_s1++; pong_bx = wx+ww/2; pong_by = _pby+_pbh/2; pong_bdx = 2; }
                }
                // Draw
                int _dmc = set_state & 1;
                gfx_fill_round_rect(wx+12,wy+44,ww-24,wh-56,6,_dmc?0x03030E:0x08081C);
                // Field
                gfx_rect(_pfx,_pby,_pfx2-_pfx+8,_pbh,0x000000);
                gfx_round_rect(_pfx-1,_pby-1,_pfx2-_pfx+10,_pbh+2,4,0x2A3A6A);
                // Center line
                for (int _l = 0; _l < _pbh; _l += 12) gfx_rect(wx+ww/2-1,_pby+_l,2,6,0x3A4A7A);
                // Paddles
                gfx_fill_round_rect(_pfx,pong_py1,8,40,4,0x44FF44);
                gfx_fill_round_rect(_pfx2,pong_py2,8,40,4,0xFF4444);
                // Ball
                gfx_fill_round_rect(pong_bx-3,pong_by-3,6,6,3,0xFFFFFF);
                // Score
                char _ps1[4], _ps2[4];
                _ps1[0]='0'+pong_s1; _ps1[1]=0; _ps2[0]='0'+pong_s2; _ps2[1]=0;
                gfx_print_scaled(wx+ww/2-40, wy+50, 0x44FF44, _ps1, 3);
                gfx_print_scaled(wx+ww/2+10, wy+50, 0xFF4444, _ps2, 3);
                // Win
                if (pong_s1 >= 5) { gfx_print_scaled(wx+ww/2-80, wy+160, 0x44FF44, "GREEN WINS!", 2); _pong_init = 0; }
                if (pong_s2 >= 5) { gfx_print_scaled(wx+ww/2-80, wy+160, 0xFF4444, "RED WINS!", 2); _pong_init = 0; }
                gfx_print(wx+16, wy+wh-24, 0x3A4A6A, "W/S  |  [Up]/[Down]  |  [Esc] exit");
                // Christmas theme
                if (_natal_mode) {
                    for (int _pn = 0; _pn < 8; _pn++) {
                        int _px = (_pn*177+53)%_pfx2;
                        gfx_putpixel(_px, _pby+(_pn*311)%_pbh, 0xFF444444);
                    }
                }
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // ─── Paint 🎨 (31) — draw with mouse ───
            if (win_type == 31) {
                if (k == 27 || k == 'q') { close_win(); search_focus = 1; goto redraw_desktop; }
                int _pcx = wx+12, _pcy = wy+48, _pcw = ww-24, _pch = wh-70;
                int _csize = _pcw / _PW;
                if (_csize * _PH > _pch) _csize = _pch / _PH;
                if (_csize < 3) _csize = 3;
                int _cw = _csize * _PW, _ch = _csize * _PH;
                int _cox = _pcx + (_pcw - _cw)/2, _coy = _pcy + (_pch - _ch)/2;
                // Init canvas
                static int _pinit = 0;
                if (!_pinit) {
                    for (int _pi = 0; _pi < _PW*_PH; _pi++) _paint_cv[_pi] = 0xFFFFFF;
                    _pinit = 1;
                }
                // Color palette
                uint32_t _pals[] = {0xFF0000,0x00FF00,0x0000FF,0xFFFF00,0xFF8800,0xFF00FF,0x00FFFF,0x000000,0x888888,0xFFFFFF};
                int _pal_y = wy+44, _pal_h = 10;
                for (int _pi = 0; _pi < 10; _pi++) {
                    gfx_rect(_pcx+_pi*28, _pal_y, 24, _pal_h, _pals[_pi]);
                    if (_paint_col == _pals[_pi]) gfx_round_rect(_pcx+_pi*28-1, _pal_y-1, 26, _pal_h+2, 2, 0x000000);
                }
                // Size selector
                if (k == '+' && _paint_size < 6) _paint_size++;
                if (k == '-' && _paint_size > 1) _paint_size--;
                char _psz[16]; _psz[0]='S';_psz[1]='z';_psz[2]=':';_psz[3]='0'+_paint_size;_psz[4]=0;
                gfx_print(_pcx+300, _pal_y-2, 0x88AACC, _psz);
                // Draw canvas
                for (int _py = 0; _py < _PH; _py++) {
                    for (int _px = 0; _px < _PW; _px++) {
                        uint32_t _pc = _paint_cv[_py*_PW+_px];
                        gfx_rect(_cox+_px*_csize, _coy+_py*_csize, _csize, _csize, _pc);
                    }
                }
                gfx_round_rect(_cox-1, _coy-1, _cw+2, _ch+2, 2, 0x3A5A8A);
                // Mouse drawing
                int _mmx = mouse_get_x(), _mmy = mouse_get_y();
                int _cix = (_mmx - _cox) / _csize, _ciy = (_mmy - _coy) / _csize;
                if (_cix >= 0 && _cix < _PW && _ciy >= 0 && _ciy < _PH) {
                    // Check color pick
                    if (_mmy >= _pal_y && _mmy < _pal_y+_pal_h && _mmx >= _pcx && _mmx < _pcx+280) {
                        int _picki = (_mmx - _pcx) / 28;
                        if (_picki >= 0 && _picki < 10 && mouse_clicked()) _paint_col = _pals[_picki];
                    } else if (mouse_clicked()) {
                        // Draw circle
                        for (int _dy = -_paint_size; _dy <= _paint_size; _dy++) {
                            for (int _dx = -_paint_size; _dx <= _paint_size; _dx++) {
                                if (_dx*_dx+_dy*_dy <= _paint_size*_paint_size) {
                                    int _px = _cix+_dx, _py = _ciy+_dy;
                                    if (_px >= 0 && _px < _PW && _py >= 0 && _py < _PH)
                                        _paint_cv[_py*_PW+_px] = _paint_col;
                                }
                            }
                        }
                    }
                    // Line from last point (smooth drawing)
                    if (mouse_clicked() && _paint_lastx >= 0) {
                        int _dx = _cix - _paint_lastx, _dy = _ciy - _paint_lasty;
                        int _step = _dx > _dy ? _dx : _dy; if (_step < 0) _step = -_step;
                        if (_step < 1) _step = 1;
                        for (int _li = 1; _li <= _step; _li++) {
                            int _lx = _paint_lastx + _dx*_li/_step;
                            int _ly = _paint_lasty + _dy*_li/_step;
                            for (int _dy2 = -_paint_size; _dy2 <= _paint_size; _dy2++) {
                                for (int _dx2 = -_paint_size; _dx2 <= _paint_size; _dx2++) {
                                    if (_dx2*_dx2+_dy2*_dy2 <= _paint_size*_paint_size) {
                                        int _px2 = _lx+_dx2, _py2 = _ly+_dy2;
                                        if (_px2 >= 0 && _px2 < _PW && _py2 >= 0 && _py2 < _PH)
                                            _paint_cv[_py2*_PW+_px2] = _paint_col;
                                    }
                                }
                            }
                        }
                    }
                    _paint_lastx = _cix; _paint_lasty = _ciy;
                } else {
                    _paint_lastx = -1;
                }
                // Clear button
                if (k == 'c') {
                    for (int _pi = 0; _pi < _PW*_PH; _pi++) _paint_cv[_pi] = 0xFFFFFF;
                }
                gfx_print(wx+16, wy+wh-22, 0x3A4A6A, "[C]lear  [+]/[-] size  [Esc] exit");
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // ─── Maze 🗺️ (32) — navigate the labyrinth ───
            if (win_type == 32) {
                if (k == 27 || k == 'q') { close_win(); search_focus = 1; goto redraw_desktop; }
                static int _maze_init = 0;
                if (!_maze_init) {
                    for (int _mi = 0; _mi < _MW*_MH; _mi++) _maze_map[_mi] = 1;
                    _maze_map[1*_MW+1]=0;_maze_map[1*_MW+2]=0;_maze_map[1*_MW+3]=0;_maze_map[1*_MW+5]=0;_maze_map[1*_MW+6]=0;_maze_map[1*_MW+7]=0;_maze_map[1*_MW+8]=0;_maze_map[1*_MW+9]=0;_maze_map[1*_MW+11]=0;_maze_map[1*_MW+12]=0;_maze_map[1*_MW+13]=0;_maze_map[1*_MW+14]=0;
                    _maze_map[2*_MW+1]=0;_maze_map[2*_MW+3]=0;_maze_map[2*_MW+5]=0;_maze_map[2*_MW+9]=0;_maze_map[2*_MW+11]=0;_maze_map[2*_MW+14]=0;
                    _maze_map[3*_MW+1]=0;_maze_map[3*_MW+3]=0;_maze_map[3*_MW+4]=0;_maze_map[3*_MW+5]=0;_maze_map[3*_MW+6]=0;_maze_map[3*_MW+7]=0;_maze_map[3*_MW+9]=0;_maze_map[3*_MW+10]=0;_maze_map[3*_MW+11]=0;_maze_map[3*_MW+12]=0;_maze_map[3*_MW+14]=0;
                    _maze_map[4*_MW+1]=0;_maze_map[4*_MW+3]=0;_maze_map[4*_MW+7]=0;_maze_map[4*_MW+11]=0;_maze_map[4*_MW+12]=0;_maze_map[4*_MW+13]=0;_maze_map[4*_MW+14]=0;
                    _maze_map[5*_MW+1]=0;_maze_map[5*_MW+2]=0;_maze_map[5*_MW+3]=0;_maze_map[5*_MW+4]=0;_maze_map[5*_MW+5]=0;_maze_map[5*_MW+7]=0;_maze_map[5*_MW+8]=0;_maze_map[5*_MW+9]=0;_maze_map[5*_MW+10]=0;_maze_map[5*_MW+12]=0;_maze_map[5*_MW+13]=0;_maze_map[5*_MW+14]=0;
                    _maze_map[7*_MW+1]=0;_maze_map[7*_MW+2]=0;_maze_map[7*_MW+3]=0;_maze_map[7*_MW+5]=0;_maze_map[7*_MW+6]=0;_maze_map[7*_MW+7]=0;_maze_map[7*_MW+8]=0;_maze_map[7*_MW+10]=0;_maze_map[7*_MW+11]=0;_maze_map[7*_MW+12]=0;_maze_map[7*_MW+13]=0;_maze_map[7*_MW+14]=0;
                    _maze_map[8*_MW+1]=0;_maze_map[8*_MW+3]=0;_maze_map[8*_MW+7]=0;_maze_map[8*_MW+14]=0;
                    _maze_map[9*_MW+1]=0;_maze_map[9*_MW+2]=0;_maze_map[9*_MW+3]=0;_maze_map[9*_MW+4]=0;_maze_map[9*_MW+5]=0;_maze_map[9*_MW+6]=0;_maze_map[9*_MW+7]=0;_maze_map[9*_MW+9]=0;_maze_map[9*_MW+10]=0;_maze_map[9*_MW+11]=0;_maze_map[9*_MW+12]=0;_maze_map[9*_MW+14]=0;
                    _maze_map[10*_MW+5]=0;_maze_map[10*_MW+7]=0;_maze_map[10*_MW+8]=0;_maze_map[10*_MW+9]=0;_maze_map[10*_MW+11]=0;_maze_map[10*_MW+12]=0;_maze_map[10*_MW+14]=0;
                    _maze_px = 1; _maze_py = 1; _maze_ex = 14; _maze_ey = 10; _maze_win = 0;
                    _maze_init = 1;
                }
                // Movement
                if (!_maze_win) {
                    int _nx = _maze_px, _ny = _maze_py;
                    if (k == KEY_UP) _ny--;
                    if (k == KEY_DOWN) _ny++;
                    if (k == KEY_LEFT) _nx--;
                    if (k == KEY_RIGHT) _nx++;
                    if (_nx >= 0 && _nx < _MW && _ny >= 0 && _ny < _MH && _maze_map[_ny*_MW+_nx] == 0) {
                        _maze_px = _nx; _maze_py = _ny;
                    }
                    if (_maze_px == _maze_ex && _maze_py == _maze_ey) _maze_win = 1;
                }
                // Draw
                int _mdm = set_state & 1;
                gfx_fill_round_rect(wx+12,wy+44,ww-24,wh-56,6,_mdm?0x03030E:0x08081C);
                int _mcw = (ww-60)/_MW, _mch = (wh-80)/_MH;
                if (_mcw > _mch) _mcw = _mch; if (_mcw < 10) _mcw = 10;
                int _mox = wx+(ww-_mcw*_MW)/2, _moy = wy+50+(wh-80-_mcw*_MH)/2;
                for (int _my = 0; _my < _MH; _my++) {
                    for (int _mx = 0; _mx < _MW; _mx++) {
                        int _mc = _maze_map[_my*_MW+_mx];
                        uint32_t _mcol = _mc ? 0x334488 : 0x111122;
                        if (_mx == _maze_px && _my == _maze_py) _mcol = 0x44FF44;
                        else if (_mx == _maze_ex && _my == _maze_ey) _mcol = 0xFFDD00;
                        gfx_fill_round_rect(_mox+_mx*_mcw+1, _moy+_my*_mcw+1, _mcw-2, _mcw-2, 3, _mcol);
                    }
                }
                if (_maze_win) {
                    gfx_print_scaled(wx+ww/2-70, wy+wh/2-20, 0xFFDD00, "YOU WIN!", 3);
                    gfx_print(wx+ww/2-50, wy+wh/2+20, 0x88AACC, "[R]estart  [Esc] exit");
                    if (k == 'r') { _maze_init = 0; }
                }
                gfx_print(wx+16, wy+wh-22, 0x3A4A6A, "[Arrow] move  [R]estart  [Esc] exit");
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // ─── Music Player 🎵 (33) — play melodies ───
            if (win_type == 33) {
                if (k == 27 || k == 'q') { close_win(); search_focus = 1; goto redraw_desktop; }
                int _mdm = set_state & 1;
                gfx_fill_round_rect(wx+12,wy+44,ww-24,wh-56,6,_mdm?0x03030E:0x08081C);
                gfx_print_scaled(wx+ww/2-80, wy+56, 0x4488FF, "Music Player", 2);
                gfx_rect(wx+20, wy+80, ww-40, 1, 0x2A3A6A);
                // Song buttons
                const char *_msongs[] = {"Jingle Bells","Startup","Sweep","Siren"};
                int _mbtns[] = {0,1,2,3};
                for (int _mi = 0; _mi < 4; _mi++) {
                    int _mby = wy+94 + _mi*34;
                    gfx_fill_round_rect(wx+30, _mby, ww-60, 28, 6, _mdm?0x0E0E2A:0x1A1A4A);
                    gfx_round_rect(wx+30, _mby, ww-60, 28, 6, 0x3A5A8A);
                    gfx_print_scaled(wx+50, _mby+4, 0x88BBFF, _msongs[_mi], 1);
                    // Click to play
                    int _mmx = mouse_get_x(), _mmy = mouse_get_y();
                    if (mouse_clicked() && _mmx >= wx+30 && _mmx < wx+ww-30 && _mmy >= _mby && _mmy < _mby+28) {
                        if (_mi == 0) play_jingle_bells();
                        else if (_mi == 1) { speaker_tone(523); for(volatile int _d=0;_d<4000000;_d++); speaker_off(); }
                        else if (_mi == 2) { play_sweep(200, 800, 500); }
                        else if (_mi == 3) {
                            for (int _si = 0; _si < 3; _si++) {
                                speaker_tone(800); for(volatile int _d=0;_d<2000000;_d++);
                                speaker_tone(600); for(volatile int _d=0;_d<2000000;_d++);
                            }
                            speaker_off();
                        }
                    }
                }
                // Status
                gfx_print(wx+16, wy+wh-22, 0x3A4A6A, "Click a song  |  [Esc] exit");
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // ─── Dino Runner 🦖 (34) — jump or die! ───
            // ─── KMP Chat 💬 (36) — instant messaging ───
            if (win_type == 36) {
                if (k == 27 || k == 'q') { close_win(); search_focus = 1; goto redraw_desktop; }
                int _kdm = set_state & 1;
                gfx_fill_round_rect(wx+12,wy+44,ww-24,wh-56,6,_kdm?0x03030E:0x08081C);
                gfx_print_scaled(wx+ww/2-70, wy+56, 0x4488FF, "Kairo Chat", 2);
                gfx_rect(wx+20, wy+80, ww-40, 1, 0x2A3A6A);

                // Chat message area
                int _cay = wy+88, _cah = wh-160;
                gfx_fill_round_rect(wx+16,_cay,ww-32,_cah,4,0x000008);
                gfx_round_rect(wx+16,_cay,ww-32,_cah,4,0x1A2A4A);

                // Status line
                int _cst_y = _cay+6;
                if (_kdm) {
                    gfx_fill_round_rect(wx+16,_cay,ww-32,_cah,4,0x000008);
                }
                if (kmp_connected) {
                    gfx_fill_round_rect(wx+20, _cay+2, 96, 16, 4, 0x0A2A0A);
                    gfx_fill_round_rect(wx+24, _cay+6, 8, 8, 4, 0x44CC44);
                    gfx_print(wx+36, _cay+4, 0x66DD66, "MyChat");
                    _cst_y = _cay+22;
                    // Show received message
                    static char _kmp_last[128];
                    static int _kmp_had = 0;
                    if (kmp_msg_ready) {
                        char _tmp[128];
                        if (kmp_poll(_tmp, 128) > 0) {
                            int _ti = 0;
                            while (_tmp[_ti] && _ti < 127) {
                                _kmp_last[_ti] = _tmp[_ti];
                                _ti++;
                            }
                            _kmp_last[_ti] = 0;
                            _kmp_had = 1;
                        }
                    }
                    if (_kmp_had) {
                        gfx_fill_round_rect(wx+18, _cst_y, ww-52, 52, 6, 0x0C0C2A);
                        gfx_round_rect(wx+18, _cst_y, ww-52, 52, 6, 0x2A2A5A);
                        gfx_print(wx+26, _cst_y+6, 0x7799CC, "peer@kairo");
                        gfx_print(wx+26, _cst_y+24, 0xC0D0FF, _kmp_last);
                    } else {
                        gfx_print(wx+22, _cst_y+8, 0x445577, "In attesa di messaggi...");
                    }
                } else {
                    gfx_fill_round_rect(wx+20, _cay+2, 96, 16, 4, 0x2A0A0A);
                    gfx_fill_round_rect(wx+24, _cay+6, 8, 8, 4, 0xCC4444);
                    gfx_print(wx+36, _cay+4, 0xDD6666, "Offline");
                }

                // Buttons
                int _kby = wy+wh-60;
                if (!kmp_connected) {
                    gfx_fill_round_rect(wx+30, _kby, 120, 28, 6, 0x1A3A1A);
                    gfx_round_rect(wx+30, _kby, 120, 28, 6, 0x3A6A3A);
                    gfx_print(wx+40, _kby+6, 0x44DD44, "Connect");
                    int _mmx = mouse_get_x(), _mmy = mouse_get_y();
                    if (mouse_clicked() && _mmx >= wx+30 && _mmx < wx+150 && _mmy >= _kby && _mmy < _kby+28) {
                        kmp_connect();
                    }
                } else {
                    gfx_fill_round_rect(wx+30, _kby, 120, 28, 6, 0x3A1A1A);
                    gfx_round_rect(wx+30, _kby, 120, 28, 6, 0x6A3A3A);
                    gfx_print(wx+36, _kby+6, 0xDD4444, "Disconnect");
                    int _mmx = mouse_get_x(), _mmy = mouse_get_y();
                    if (mouse_clicked() && _mmx >= wx+30 && _mmx < wx+150 && _mmy >= _kby && _mmy < _kby+28) {
                        kmp_disconnect();
                    }
                    // Send "Hello" button
                    gfx_fill_round_rect(wx+170, _kby, 120, 28, 6, 0x1A1A3A);
                    gfx_round_rect(wx+170, _kby, 120, 28, 6, 0x3A3A6A);
                    gfx_print(wx+186, _kby+6, 0x8888FF, "Say Hello");
                    if (mouse_clicked() && _mmx >= wx+170 && _mmx < wx+290 && _mmy >= _kby && _mmy < _kby+28) {
                        kmp_send_text("Hello from KairoOS!");
                    }
                }

                gfx_print(wx+16, wy+wh-22, 0x3A4A6A, "[Esc] exit");
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            if (win_type == 34) {
                if (k == 27 || k == 'q') { close_win(); search_focus = 1; goto redraw_desktop; }
                int _ddm = set_state & 1;
                gfx_fill_round_rect(wx+12,wy+44,ww-24,wh-56,6,_ddm?0x03030E:0x08081C);
                int _dgx = wx+20, _dgy = wy+80, _dgw = ww-40, _dgh = wh-120;
                // Ground
                int _gy = _dgy + _dgh - 20;
                gfx_rect(_dgx, _gy, _dgw, 2, 0x44AA44);
                for (int _gi = 0; _gi < _dgw; _gi += 8) {
                    gfx_rect(_dgx+_gi, _gy+2, 4, 2, 0x338833);
                }
                static int _dino_init = 0;
                if (!_dino_init) {
                    _dino_y = 0; _dino_v = 0; _dino_obs = 0;
                    _dino_ox = _dgw; _dino_score = 0; _dino_dead = 0; _dino_t = 0;
                    _dino_init = 1;
                }
                if (!_dino_dead) {
                    _dino_t++;
                    // Gravity
                    _dino_v += 1;
                    _dino_y += _dino_v;
                    if (_dino_y > 0) { _dino_y = 0; _dino_v = 0; }
                    // Jump
                    if ((k == ' ' || k == KEY_UP) && _dino_y == 0) { _dino_v = -10; }
                    // Obstacle
                    _dino_ox -= 3 + _dino_score/20;
                    if (_dino_ox < -20) {
                        _dino_ox = _dgw + (_dino_score*37%100);
                        _dino_score++;
                    }
                    // Collision
                    if (_dino_ox >= 30 && _dino_ox <= 50 && _dino_y >= -15) {
                        _dino_dead = 1;
                    }
                }
                // Draw dino
                int _dy = _gy - 20 + _dino_y;
                gfx_fill_round_rect(_dgx+30, _dy, 12, 16, 3, _dino_dead?0xFF4444:0x44DD44);
                gfx_putpixel(_dgx+32, _dy+4, 0x000000);
                gfx_fill_round_rect(_dgx+28, _dy+14, 16, 4, 2, _dino_dead?0xFF6666:0x66FF66);
                // Draw cactus
                if (_dino_ox >= 0) {
                    gfx_fill_round_rect(_dgx+_dino_ox, _gy-16, 8, 16, 2, 0x338833);
                    gfx_fill_round_rect(_dgx+_dino_ox-4, _gy-12, 16, 4, 2, 0x338833);
                }
                // Score
                char _ds[16]; int _di=0, _dn=_dino_score;
                do{_ds[_di++]='0'+_dn%10;_dn/=10;}while(_dn);_ds[_di]=0;
                for(int _dk=0;_dk<_di/2;_dk++){char _dt=_ds[_dk];_ds[_dk]=_ds[_di-1-_dk];_ds[_di-1-_dk]=_dt;}
                gfx_print_scaled(_dgx+_dgw-60, _dgy, 0x88AACC, _ds, 2);
                if (_dino_dead) {
                    gfx_print_scaled(_dgx+_dgw/2-70, _dgy+_dgh/2-20, 0xFF4444, "GAME OVER", 2);
                    gfx_print(_dgx+_dgw/2-60, _dgy+_dgh/2+10, 0x88AACC, "[Space] restart  [Esc] exit");
                    if (k == ' ') { _dino_init = 0; }
                }
                gfx_print(wx+16, wy+wh-22, 0x3A4A6A, "[Space/Up] jump  [Esc] exit");
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // ─── KairoWeb (35) — GUI browser ───
            if (win_type == 35) {
                if (k == 27 || k == 'q' || (k_ctrl && k == 'w')) {
                    if (br_focus && k == 27) { br_focus = 0; br_redraw(); continue; }
                    close_win(); search_focus = 1; goto redraw_desktop;
                }
                // mouse: click su back/reload/link/urlbar
                if (mouse_clicked()) {
                    int _mx = mouse_get_x(), _my = mouse_get_y();
                    if (_mx >= kw_rel_x0 && _mx <= kw_rel_x1 && _my >= kw_rel_y0 && _my <= kw_rel_y1) {
                        // reload: ricarica l'URL corrente
                        if (kw_url[0]) {
                            kw_load(kw_url);
                            kw_scroll = 0; br_focus = 0;
                            br_redraw(); continue;
                        }
                    } else if (_mx >= wx+14 && _mx < wx+32 && _my >= wy+42 && _my < wy+60) {
                        // back
                        if (kw_prev[0]) {
                            char _bv[160]; int _bi;
                            for (_bi=0;kw_url[_bi]&&_bi<159;_bi++) _bv[_bi]=kw_url[_bi];
                            _bv[_bi]=0;
                            kw_load(kw_prev);
                            for (_bi=0;_bv[_bi]&&_bi<159;_bi++) kw_prev[_bi]=_bv[_bi];
                            kw_prev[_bi]=0;
                            br_focus = 0; br_redraw(); continue;
                        }
                    } else if (_mx >= wx+38 && _mx < wx+ww-34 && _my >= wy+41 && _my < wy+61) {
                        if (!br_focus) {
                            char _cb[160]; int _ci=0;
                            for (_ci=0;kw_url[_ci]&&_ci<126;_ci++) br_url[_ci]=kw_url[_ci];
                            br_url[_ci]=0; br_pos=_ci;
                            br_focus = 1; br_redraw(); continue;
                        }
                    } else if (_my >= wy+52) {
                        // click su un link
                        for (int _lk = 0; _lk < kw_link_count; _lk++) {
                            if (_mx >= kw_lx0[_lk] && _mx <= kw_lx1[_lk] &&
                                _my >= kw_ly0[_lk] && _my <= kw_ly1[_lk]) {
                                if (kw_lhref[_lk][0]) {
                                    char _bi;
                                    for (_bi=0;kw_url[_bi]&&_bi<159;_bi++) kw_prev[_bi]=kw_url[_bi];
                                    kw_prev[_bi]=0;
                                    // resolve relative href (concat a kw_url base)
                                    char _full[200]; int _fi=0;
                                    if (kw_lhref[_lk][0]=='h'&&kw_lhref[_lk][1]=='t'&&kw_lhref[_lk][2]=='t'&&kw_lhref[_lk][3]=='p') {
                                        for (int _q=0;kw_lhref[_lk][_q]&&_q<199;_q++) _full[_fi++]=kw_lhref[_lk][_q];
                                    } else {
                                        // base scheme+host da kw_url
                                        int _b=0; while (_b<kw_len(kw_url) && kw_url[_b]!='/') _b++;
                                        if (_b==0) _b=kw_len(kw_url);
                                        for (int _q=0;_q<_b&&_q<150;_q++) _full[_fi++]=kw_url[_q];
                                        if (kw_lhref[_lk][0]!='/') _full[_fi++]='/';
                                        for (int _q=0;kw_lhref[_lk][_q]&&_q<50;_q++) _full[_fi++]=kw_lhref[_lk][_q];
                                    }
                                    _full[_fi]=0;
                                    kw_load(_full);
                                    for (int _q=0;_full[_q]&&_q<159;_q++) kw_url[_q]=_full[_q];
                                    kw_url[_fi<160?_fi:159]=0;
                                    kw_scroll = 0; br_focus = 0;
                                    br_redraw();
                                    continue;
                                }
                            }
                        }
                        if (br_focus) { br_focus = 0; br_redraw(); continue; }
                    }
                }
                if (k == KEY_UP) { if (kw_rline_count) { if (kw_scroll > 0) kw_scroll--; br_redraw(); } continue; }
                if (k == KEY_DOWN) { kw_scroll++; br_redraw(); continue; }
                if (k == KEY_PGUP) { kw_scroll -= 8; if (kw_scroll < 0) kw_scroll = 0; br_redraw(); continue; }
                if (k == KEY_PGDN) { kw_scroll += 8; br_redraw(); continue; }
                if (k == '\n') {
                    br_url[br_pos] = 0;
                    if (br_pos > 0) {
                        char _u[160]; int _ui;
                        for (_ui=0;_ui<br_pos&&_ui<159;_ui++) _u[_ui]=br_url[_ui];
                        _u[_ui]=0;
                        // https → prova senza s (no TLS)
                        if (_u[0]=='h'&&_u[1]=='t'&&_u[2]=='t'&&_u[3]=='p'&&_u[4]=='s'&&_u[5]==':'&&_u[6]=='/'&&_u[7]=='/') {
                            char _p[160]; int _pi=0;
                            for (int _q=8;_u[_q]&&_q<159;_q++) _p[_pi++]=_u[_q];
                            _p[_pi]=0;
                            char _done[168]; int _di=0;
                            const char *_px="http://"; for(const char*_s=_px;*_s;_s++)_done[_di++]=*_s;
                            for(int _q=0;_p[_q]&&_q<159;_q++)_done[_di++]=_p[_q];
                            _done[_di]=0;
                            for (_ui=0;_done[_ui]&&_ui<159;_ui++) _u[_ui]=_done[_ui];
                            _u[_ui]=0;
                        }
                        { int _bi; for(_bi=0;kw_url[_bi]&&_bi<159;_bi++) kw_prev[_bi]=kw_url[_bi]; kw_prev[_bi]=0; }
                        int _r = kw_load(_u);
                        (void)_r;
                        for (_ui=0;_u[_ui]&&_ui<159;_ui++) kw_url[_ui]=_u[_ui];
                        kw_url[_ui]=0;
                        kw_scroll = 0;
                        br_pos = 0; br_url[0]=0;
                        br_focus = 0;
                        br_redraw();
                    } else if (!br_focus) { br_focus = 1; br_redraw(); }
                    continue;
                }
                if (br_focus) {
                    if (k == '\b' && br_pos > 0) { br_pos--; br_url[br_pos] = 0; br_redraw(); continue; }
                    if (k == '\b' && br_pos == 0) { br_focus = 0; br_redraw(); continue; }
                    if (br_pos < 127 && k >= ' ' && k <= '~') { br_url[br_pos++] = k; br_url[br_pos] = 0; br_redraw(); continue; }
                }
                br_redraw();
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // ─── BootLoader Command Line (37) ───
            if (win_type == 37) {
                if (k == 27) { close_win(); goto redraw_desktop; }
                if (k == KEY_UP && bl_scroll > 0) { bl_scroll--; bl_redraw(); continue; }
                if (k == KEY_DOWN) { bl_scroll++; if (bl_scroll >= bl_count) bl_scroll = bl_count - 1; if (bl_scroll < 0) bl_scroll = 0; bl_redraw(); continue; }
                if (k == '\b') { if (bl_pos > 0) { bl_pos--; bl_buf[bl_pos] = 0; } bl_redraw(); continue; }
                if (k == '\n') {
                    bl_buf[bl_pos] = 0;
                    bl_add(bl_buf);
                    char _cmd[128]; int _ci;
                    for (_ci = 0; bl_lines[bl_count-1][_ci] && _ci < 127; _ci++) {
                        char _ch = bl_lines[bl_count-1][_ci];
                        if (_ch >= 'A' && _ch <= 'Z') _ch += 32;
                        _cmd[_ci] = _ch;
                    }
                    _cmd[_ci] = 0;
                    bl_pos = 0; bl_buf[0] = 0;
                    if (_ci == 0) {
                        bl_add(" ");
                        bl_add("Type 'help' for info.");
                    } else if (_ci == 4 && _cmd[0]=='h' && _cmd[1]=='e' && _cmd[2]=='l' && _cmd[3]=='p') {
                        bl_add(" ");
                        bl_add("/help");
                        bl_add("Command Line:");
                        bl_add("- Bloader open system --files");
                        bl_add("This command opens file's of the os, allowing you to edit it, but you may need experience and percausions at the same time");
                        bl_add("- Bloader erase os --kairoos");
                        bl_add("It erases KairoOS, but not its files");
                        bl_add("- BLoader repristinate os --Kairoos");
                        bl_add("work only on KairoCode BLoader (an IDE that lets you create OS, no simulations), it saves the Bootloader as an app and repiristinates KairoOS like normal");
                    } else if (bl_has(_cmd, "bloader") && bl_has(_cmd, "open") && bl_has(_cmd, "system") && bl_has(_cmd, "files")) {
                        bl_add(" ");
                        bl_add("- Bloader open system --files");
                        bl_add("This command opens file's of the os, allowing you to edit it, but you may need experience and percausions at the same time");
                    } else if (bl_has(_cmd, "bloader") && bl_has(_cmd, "erase") && bl_has(_cmd, "os") && bl_has(_cmd, "kairoos")) {
                        bl_add(" ");
                        bl_add("- Bloader erase os --kairoos");
                        bl_add("It erases KairoOS, but not its files");
                    } else if (bl_has(_cmd, "bloader") && bl_has(_cmd, "repristinate") && bl_has(_cmd, "os") && bl_has(_cmd, "kairoos")) {
                        bl_add(" ");
                        bl_add("- BLoader repristinate os --Kairoos");
                        bl_add("work only on KairoCode BLoader (an IDE that lets you create OS, no simulations), it saves the Bootloader as an app and repiristinates KairoOS like normal");
                    } else if (bl_has(_cmd, "bloader")) {
                        bl_add(" ");
                        bl_add("[Unknown BLoader command. Type 'help' for info]");
                    } else {
                        bl_add(" ");
                        bl_add("[Unknown command. Type 'help' for info]");
                    }
                    bl_redraw();
                    continue;
                }
                if (bl_pos < 126 && k >= ' ' && k <= '~') { bl_buf[bl_pos++] = k; bl_buf[bl_pos] = 0; bl_redraw(); continue; }
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // ─── Syslog Viewer (38) ───
            if (win_type == 38) {
                if (k == 27) { close_win(); goto redraw_desktop; }
                if (k == KEY_UP && syslog_scroll > 0) { syslog_scroll--; syslog_redraw(); continue; }
                if (k == KEY_DOWN) { syslog_scroll++; if(syslog_scroll>=syslog_count)syslog_scroll=syslog_count-1; syslog_redraw(); continue; }
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // ─── Phone / Dialer (39) ───
            if (win_type == 39) {
                if (k == 27) { if (voip_state != VOIP_IDLE) { voip_hangup(); ac97_stop_capture(); } close_win(); goto redraw_desktop; }

                // Navigation
                if (k == KEY_LEFT && phone_sel > 0) { phone_sel--; phone_redraw(set_state&1); continue; }
                if (k == KEY_RIGHT && phone_sel < 13) { phone_sel++; phone_redraw(set_state&1); continue; }
                if (k == KEY_UP) {
                    if (phone_sel >= 3 && phone_sel <= 11) phone_sel -= 3;
                    else if (phone_sel == 12 || phone_sel == 13) phone_sel = 9; // jump to row 3
                    else if (phone_sel < 3) phone_sel = 12; // wrap to call button
                    phone_redraw(set_state&1); continue;
                }
                if (k == KEY_DOWN) {
                    if (phone_sel <= 8) phone_sel += 3;
                    else if (phone_sel <= 11) phone_sel = 12; // jump to call button
                    else if (phone_sel == 12 || phone_sel == 13) phone_sel = 0; // wrap to top
                    phone_redraw(set_state&1); continue;
                }

                // Enter — press selected key
                if (k == '\n') {
                    if (phone_sel <= 11) {
                        // Dial pad digit
                        const char *pad_digits = "123456789*0#";
                        char d = pad_digits[phone_sel];
                        if (phone_pos < 18) {
                            phone_num[phone_pos++] = d;
                            phone_num[phone_pos] = 0;
                            ac97_play_dtmf(d, 150);
                        }
                        phone_redraw(set_state&1); continue;
                    } else if (phone_sel == 12 && voip_state == VOIP_IDLE) {
                        // Call button — start VoIP call
                        if (phone_pos > 0) {
                            voip_start_call();
                            phone_calling = 1;
                        }
                        phone_redraw(set_state&1); continue;
                    } else if (phone_sel == 12 && voip_state == VOIP_RINGING) {
                        // Accept incoming call
                        voip_state = VOIP_ACTIVE;
                        voip_seq = 0;
                        voip_jitter_count = 0;
                        voip_send_signal(SIG_ACCEPT, voip_peer_ip);
                        phone_calling = 1;
                        ac97_start_capture();
                        syslog_add("[PHONE] Call accepted!");
                        phone_redraw(set_state&1); continue;
                    } else if ((phone_sel == 13) && (voip_state == VOIP_ACTIVE || voip_state == VOIP_INVITING || phone_calling)) {
                        // Hangup
                        voip_hangup();
                        ac97_stop_capture();
                        phone_calling = 0;
                        syslog_add("[PHONE] Call ended");
                        phone_redraw(set_state&1); continue;
                    } else if (phone_sel == 13 && voip_state == VOIP_RINGING) {
                        // Reject incoming
                        voip_state = VOIP_IDLE;
                        syslog_add("[PHONE] Call rejected");
                        phone_redraw(set_state&1); continue;
                    }
                }

                // Backspace
                if (k == '\b' && phone_pos > 0 && !phone_calling) {
                    phone_pos--;
                    phone_num[phone_pos] = 0;
                    ac97_play_hover();
                    phone_redraw(set_state&1); continue;
                }

                // Direct number keys
                if (k >= '0' && k <= '9' && !phone_calling) {
                    if (phone_pos < 18) {
                        phone_num[phone_pos++] = k;
                        phone_num[phone_pos] = 0;
                        ac97_play_dtmf(k, 150);
                    }
                    phone_redraw(set_state&1); continue;
                }
                if (k == '*' && !phone_calling) {
                    if (phone_pos < 18) {
                        phone_num[phone_pos++] = '*';
                        phone_num[phone_pos] = 0;
                        ac97_play_dtmf('*', 150);
                    }
                    phone_redraw(set_state&1); continue;
                }
                if (k == '#' && !phone_calling) {
                    if (phone_pos < 18) {
                        phone_num[phone_pos++] = '#';
                        phone_num[phone_pos] = 0;
                        ac97_play_dtmf('#', 150);
                    }
                    phone_redraw(set_state&1); continue;
                }

                // Calling animation — ring pulse
                if (phone_calling && (_tick % 60 == 0)) {
                    phone_redraw(set_state&1); continue;
                }

                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            if (!k) { asm volatile("hlt"); continue; }
            continue; // other windows: just wait for Esc
        }

        // ─── NO WINDOW OPEN ───

        // Mouse click on dock icons
        if (mouse_clicked()) {
            int _dc_x = w/2 - 320, _dc_y = h - 70;
            int _di_sp = 56, _di_base_sz = 40;
            int _di_base = _dc_x + (640 - 10*_di_sp)/2;
            int _mx = mouse_get_x(), _my = mouse_get_y();
            int _di = (_mx - _di_base + _di_sp/2) / _di_sp;
            if (_di >= 0 && _di < 10) {
                int _msz = _di_base_sz * dock_sz[_di] / 1000;
                int _di_x = _di_base + _di*_di_sp + (_di_base_sz - _msz)/2;
                int _di_y = _dc_y + 8 + (_di_base_sz - _msz)/2;
                if (_mx >= _di_x - 4 && _mx <= _di_x + _msz + 4 && _my >= _di_y - 4 && _my <= _di_y + _msz + 4) {
                    int acts[] = {34, 33, 25, 3, 6, 32, 30, 31, 26, 24};
                    _launch_app(acts[_di]);
                    continue;
                }
            }
            // Click on search bar
            int sb_x = w/2-120, sb_y = 27, sb_w = 240, sb_h = 20;
            if (_mx >= sb_x && _mx < sb_x+sb_w && _my >= sb_y && _my < sb_y+sb_h) {
                search_focus = 1; continue;
            }
            // Click on desktop: unfocus search
            search_focus = 0;
        }

        // Keyboard cursor movement (arrow keys) — disabled while search is focused
        if ((k == 0x01 || k == 0x02 || k == 0x03 || k == 0x04 || k == ' ') && !search_focus) {
            mouse_move_by_key(k);
            goto redraw_desktop;
        }

        // ALT+B → BIOS Setup
        if (is_alt_pressed() && k == 'b' && !win) {
            gfx_clear(0x0000AA);
            gfx_rect(0, 0, w, 1, 0xFFFFFF);
            gfx_rect(0, h-14, w, 14, 0x000080);
            gfx_print_scaled(w/2-140, 10, 0xFFFFFF, "VITEZA BIOS SETUP v1.0", 2);
            gfx_rect(w/2-220, 36, 440, 1, 0xFFFFFF);
            gfx_print(40, 56, 0x8080FF, "Main    Advanced    Boot    Security    Save & Exit");
            gfx_rect(40, 70, 460, 1, 0x8080FF);

            gfx_print(40, 98, 0xFFFF00, "[1]");
            gfx_print(80, 98, 0xFFFFFF, "System Information");

            gfx_print(40, 124, 0xFFFF00, "[2]");
            gfx_print(80, 124, 0xFFFFFF, "Exit");

            gfx_fill_round_rect(w/2-160, h-110, 320, 50, 6, 0x000080);
            gfx_print(60, h-96, 0x00FF00, "Viteza v1.0  |  x86_64 Long Mode  |  OreoWM");
            gfx_print(60, h-80, 0x8080FF, "CPU: Intel/AMD  |  RAM: 256 MB  |  VBE: 1280x720x32");

            gfx_print(40, h-44, 0x8080FF, "Use number keys to select.  ESC to exit.");

            while (1) {
                char bk = keyboard_last_char();
                if (bk == 27 || bk == '2') goto redraw_desktop;
                asm volatile("hlt");
            }
        }

        // Launchpad (L key)
        if (k == 'l' && !win && !search_focus) {
            win=1;win_type=15;lp_sel_x=0;lp_sel_y=0;
            draw_launch_pad(w, h, lp_sel_x, lp_sel_y);
            continue;
        }

        // System Transfer (T key)
        if (k == 't' && !win && !search_focus) {
            st_mode = ST_MENU; st_sel = 0;
            win=1; win_type=27;
            draw_system_transfer(w, h, &(int){0});
            continue;
        }

        // Control Center (Ctrl+C)
        if (k_ctrl && k == 'c' && !win) {
            nc_active=0; cc_active=!cc_active; cc_sel=0;
            if(!cc_active){goto redraw_desktop;}
            continue;
        }

        // Notification Center (Ctrl+N)
        if (k_ctrl && k == 'n' && !win) {
            cc_active=0; nc_active=!nc_active; nc_sel=0;
            if(!nc_active){goto redraw_desktop;}
            continue;
        }

        // Wi-Fi panel (Ctrl+X)
        if (k_ctrl && k == 'x' && !win) {
            cc_active=0; nc_active=0;
            wifi_active=!wifi_active; wifi_sel=0;
            if(wifi_active){wifi_request_scan();}
            goto redraw_desktop;
        }

        // Camera app (C key)
        if (k == 'c' && !win && !search_focus) {
            open_app(11); draw_mac_title("Camera");
            gfx_print(wx+16, wy+44, 0x4A9EFF, "Camera — Hardware Status");
            continue;
        }

        // Kairo Player (V key)
        if (k == 'v' && !win && !search_focus) {
            open_app(12); draw_mac_title("Kairo Player");
            // Video screen area
            gfx_fill_round_rect(wx+20, wy+48, ww-40, 130, 4, 0x000000);
            gfx_round_rect(wx+20, wy+48, ww-40, 130, 4, 0x3A5A8A);
            // Kairo Visual Engine
            gfx_print(wx+28, wy+56, 0xFFFFFF, "Kairo Visual Engine");
            // Static pattern preview
            for (int _vy = 0; _vy < 100; _vy++) {
                for (int _vx = 0; _vx < 320; _vx++) {
                    int _vc = ((_vx * 5) ^ (_vy * 7)) & 0xFF;
                    uint32_t _col = (_vc << 16) | (_vc << 8) | _vc;
                    gfx_putpixel(wx+30 + _vx, wy+70 + _vy, _col);
                }
            }
            gfx_round_rect(wx+30, wy+70, 320, 100, 2, 0x4A6ADF);
            // Kairo Audio Engine badge
            gfx_print(wx+ww-130, wy+155, 0x00E5FF, "Kairo Audio");
            // Status
            gfx_print(wx+ww-130, wy+163, 0x3A5A8A, "Ready");
            // Playback controls
            gfx_print(wx+24, wy+196, 0x8A9ACE, "[P] Play  [S] Stop");
            // Powered by line
            gfx_print(wx+20, wy+wh-18, 0x3A4A6A, "Powered by Kairo Visual & Kairo Audio");
            continue;
        }

        // True Video app (T key)
        if (k == 't' && !win && !search_focus) {
            open_app(14); draw_mac_title("True Video");
            continue;
        }

        // New App Shortcuts (single letters when no window)
        if (k == 'e' && !win && !search_focus) { // Calendar
            open_app(16); draw_mac_title("Calendar");
            // Will redraw on next tick
            continue;
        }
        if (k == 'r' && !win && !search_focus) { // Pomodoro
            open_app(17); draw_mac_title("Pomodoro Timer");
            pom_sec = pom_total; pom_running = 0; pom_ticks = 0;
            continue;
        }
        if (k == 'w' && !win && !search_focus) { // Weather
            open_app(18); draw_mac_title("Weather"); wthr_first = 1;
            continue;
        }
        if (k == 'i' && !win && !search_focus) { // Disk Usage
            open_app(19); draw_mac_title("System Monitor");
            continue;
        }
        if (k == 'a' && !win && !search_focus) { // ASCII Art
            open_app(20); draw_mac_title("ASCII Art Gallery");
            art_sel = 0;
            continue;
        }
        if (k == 'y' && !win && !search_focus) { // Typing Test
            open_app(21); draw_mac_title("Typing Test");
            type_pos = 0; type_err = 0; type_ok = 0; type_start = 0; type_done = 0;
            continue;
        }
        if (k == 'o' && !win && !search_focus) { // Clipboard
            open_app(22); draw_mac_title("Clipboard");
            clip_save_pos = 0; clip_save[0] = 0;
            continue;
        }
        if (k == 'f' && !win && !search_focus) { // File Manager
            open_app(23); draw_mac_title("File Manager");
            fm_sel = 0; fm_scroll = 0;
            continue;
        }
        if (k == 'g' && !win && !search_focus) { // Tetris
            open_app(24); draw_mac_title("Tetris");
            continue;
        }
        if (k == 'k' && !win && !search_focus) { // Kairo Games
            open_app(25); draw_mac_title("Kairo Games");
            continue;
        }
        if (k == 'z' && !win && !search_focus) { // Viteza Wii
            open_app(28); _wii_boot = 1;
            continue;
        }
        if (k == 'p' && !win && !search_focus) { // Pong
            open_app(30); draw_mac_title("Pong ⚡");
            continue;
        }
        if (k == 'b' && !win && !search_focus) { // Paint
            open_app(31); draw_mac_title("Paint 🎨");
            continue;
        }
        if (k == 'x' && !win && !search_focus) { // Maze
            open_app(32); draw_mac_title("Maze 🗺️");
            continue;
        }
        if (k == 'm' && !win && !search_focus) { // Mic Test
            ac97_start_capture();
            open_app(29); draw_mac_title("Mic Test");
            continue;
        }
        if (k == 's' && !win && !search_focus) { // Music Player
            open_app(33); draw_mac_title("Music Player 🎵");
            continue;
        }
        if (k == 'd' && !win && !search_focus) { // Dino Runner
            open_app(34); draw_mac_title("Dino Runner 🦖");
            continue;
        }
        if (k == 'h' && !win && !search_focus) { // Chat
            open_app(36); draw_mac_title("Kairo Chat 💬");
            continue;
        }

        // Note widget navigation (when no window, search unfocused)
        if (!win && !search_focus) {
            if (k == ',' && note_count > 0) { note_sel--; if (note_sel < 0) note_sel = note_count-1; note_widget_draw(); continue; }
            if (k == '.' && note_count > 0) { note_sel++; if (note_sel >= note_count) note_sel = 0; note_widget_draw(); continue; }
        }

        // USB device simulation (press U) — works even when search is empty/focused
        if (k == 'u' && !win && !search_focus) {
            play_click();
            usb_popup = 1;
            int px = w-220, py = h-108, pw = 200, ph = 40;
            gfx_fill_round_rect(px+2,py+2,pw,ph,8,0x000000);
            gfx_fill_round_rect(px,py,pw,ph,8,0x0C0C2C);
            gfx_round_rect(px,py,pw,ph,8,0x3A6AFF);
            gfx_fill_round_rect(px+8,py+9,8,8,2,0x3A6AFF);
            gfx_rect(px+10,py+11,4,4,0x00E5FF);
            gfx_print(px+22,py+8,0xFFFFFF,"USB Device Attached");
            int _uc = usb_device_count();
            if (_uc > 0) {
                for (int _ui = 0; _ui < _uc && _ui < 2; _ui++) {
                    gfx_print(px+22, py+24+_ui*14, 0x6A8ABE, usb_device_name(_ui));
                }
            } else {
                gfx_print(px+22, py+24, 0x6A8ABE, "No USB devices connected");
            }
            continue;
        }

        // ─── SEARCH INPUT ───
        if (search_focus) {
            // Compute matches
            int match[MAX_SEARCH], match_count = 0;
            for (int mi = 0; mi < MAX_SEARCH; mi++) {
                if (search_pos == 0) { match[match_count++] = mi; continue; }
                const char *si = sitems[mi];
                int mm = 1;
                for (int sj = 0; search_buf[sj]; sj++) {
                    char c1 = search_buf[sj], c2 = si[sj];
                    if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
                    if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
                    if (c1 != c2) { mm = 0; break; }
                }
                if (mm) match[match_count++] = mi;
            }

            if (k == KEY_UP && match_count > 0) { search_sel--; if (search_sel < 0) search_sel = match_count-1; dropdown_draw(match,match_count,search_sel); continue; }
            if (k == KEY_DOWN && match_count > 0) { search_sel++; if (search_sel >= match_count) search_sel = 0; dropdown_draw(match,match_count,search_sel); continue; }

            if (k == '\n') {
                if (match_count > 0) {
                    int act = saction[match[search_sel]];
                    // Clear dropdown
                    int _dd_h=match_count*22+10;int _dd_x=sb_x,_dd_y=sb_y+sb_h+2;
                    for(int _y=_dd_y;_y<_dd_y+_dd_h;_y++){gfx_rect(_dd_x,_y,sb_w,1,0x0B0B30);}
                    // Draw corresponding flat window
                    win=1;win_type=act;
                    gfx_fill_round_rect(wx,wy,ww,wh,10,0x0E0E30);
                    gfx_round_rect(wx,wy,ww,wh,10,0x00E5FF);
                    if (act == 1) { draw_mac_title("This PC");
                        gfx_fill_round_rect(wx+16,wy+50,ww-32,20,4,0x12124A);gfx_print(wx+24,wy+53,0x4A9EFF,"System");
                        gfx_print(wx+24,wy+84,0x8A9ACE,"Kernel:  Viteza v1.0");
                        gfx_print(wx+24,wy+102,0x8A9ACE,"CPU:     x86_64 Long Mode");
                        gfx_print(wx+24,wy+120,0x8A9ACE,"RAM:     256 MB");
                        char _dd[16];int _dw=w,_dh=h;
                        _dd[0]='0'+_dw/100%10;_dd[1]='0'+_dw/10%10;_dd[2]='0'+_dw%10;
                        _dd[3]='x';_dd[4]='0'+_dh/100%10;_dd[5]='0'+_dh/10%10;_dd[6]='0'+_dh%10;_dd[7]=0;
                        gfx_print(wx+24,wy+138,0x8A9ACE,"Display: ");gfx_print(wx+96,wy+138,0x00E5FF,_dd);
                        gfx_print(wx+24,wy+162,0x6A7A9E,"No drives detected");
                        int _udc = usb_device_count();
                        if (_udc > 0) {
                            gfx_print(wx+24, wy+184, 0x4A9EFF, "USB Devices:");
                            for (int _uj = 0; _uj < _udc && _uj < 2; _uj++) {
                                gfx_print(wx+24, wy+204+_uj*16, 0x6A8ABE, usb_device_name(_uj));
                            }
                        } else {
                            gfx_print(wx+24, wy+184, 0x6A7A9E, "No USB devices");
                        }
                        gfx_print(wx+24,wy+240,0x3A4A6A,"[Esc] to close");
                    } else if (act == 2) { draw_mac_title("Network");
                        gfx_fill_round_rect(wx+16,wy+50,ww-32,20,4,0x12124A);gfx_print(wx+24,wy+53,0x4A9EFF,"Wi-Fi");
                        gfx_print(wx+24,wy+84,0x8A9ACE,"SSID:    HOME-5G");
                        gfx_print(wx+24,wy+102,0x8A9ACE,"Signal:  Excellent");
                        gfx_print(wx+24,wy+120,0x8A9ACE,"Sec:     WPA2-PSK");
                        gfx_print(wx+24,wy+144,0x6A7A9E,"IP: 0.0.0.0 (pending)");
                    } else if (act == 3) { draw_mac_title("Terminal");
                        term_line_count=0;term_scroll=0;
                        term_add("[Viteza Terminal v1.0]",0);
                        term_add("Type 'help' for commands.",0);
                        term_redraw();
                    } else if (act == 4) {
                        settings_active=1;set_cat=0;settings_redraw();
                    } else if (act == 5) { draw_mac_title("OreoAI Assistant");
                        chat_line_count=0;chat_scroll=0;chat_pos=0;
                        chat_add("[OreoAI v1.0 — Ask me anything!]");
                        chat_add("Try: hello, who are you, help");
                        chat_redraw();
                    } else if (act == 6) { draw_mac_title("Calculator");
                        calc_val=0;calc_cur=0;calc_op=0;calc_state=0;
                        calc_redraw();
                    } else if (act == 7) { draw_mac_title("Notes");
                        gfx_fill_round_rect(wx+16,wy+50,ww-32,20,4,0x12124A);gfx_print(wx+24,wy+53,0x4A9EFF,"My Notes");
                        for(int _ni=0;_ni<note_count&&_ni<8;_ni++){
                            char _ns[4];_ns[0]='0'+(_ni+1)%10;_ns[1]='.';_ns[2]=' ';_ns[3]=0;
                            gfx_print(wx+20,wy+78+_ni*16,0x8899CC,_ns);
                            char _nt[44];note_short(_ni,_nt);
                            gfx_print(wx+40,wy+78+_ni*16,0x6A8ABE,_nt);
                        }
                        gfx_rect(wx+12,wy+210,ww-24,1,0x2A3A6A);
                        gfx_print(wx+16,wy+218,0x4A6A8A,"> ");
                        print_note_buf();
                        gfx_print(wx+16,wy+248,0x3A4A6A,"[Enter] save  [Esc] back");
                    } else if (act == 8) { draw_mac_title("App Store");
                        gfx_fill_round_rect(wx+16,wy+50,ww-32,20,4,0x12124A);gfx_print(wx+24,wy+53,0x4A9EFF,"Available Apps");
                        for(int _asi=0;_asi<APP_COUNT&&_asi<10;_asi++){
                            int _asy=wy+78+_asi*20;
                            gfx_fill_round_rect(wx+16,_asy-2,ww-32,18,3,app_colors[_asi]);gfx_rect(wx+16,_asy-2,4,18,app_colors[_asi]);
                            gfx_print(wx+28,_asy+1,0xFFFFFF,app_names[_asi]);
                            gfx_print(wx+180,_asy+1,0x8080AA,app_cats[_asi]);
                            if(apps_installed[_asi]){gfx_print(wx+300,_asy+1,0x44FF44,"[Installed]");}
                            else{gfx_print(wx+300,_asy+1,0x808080,"[ ");char _ak[2];_ak[0]='0'+_asi%10;_ak[1]=0;gfx_print(wx+312,_asy+1,0xFFAA00,_ak);gfx_print(wx+324,_asy+1,0x808080," ]");}
                        }
                        gfx_print(wx+16,wy+240,0x3A4A6A,"[0-9] install/uninstall  [Esc] back");
                    } else if (act == 9) { draw_mac_title("Kairo Studio");
                        studio_init_data(); studio_draw_all();
                    } else if (act == 10) { draw_mac_title("KairoVM");
                        vm_mode = 0;
                        gfx_fill_round_rect(wx+16,wy+50,ww-32,20,4,0x12124A);gfx_print(wx+24,wy+53,0xFF6644,"KairoVM — Virtual Machine Manager");
                        if (vm_count == 0) {
                            gfx_print(wx+60,wy+100,0x6A7A9E,"No virtual machines yet.");
                            gfx_print(wx+60,wy+124,0x4A9EFF,"Press [c] to create one.");
                        } else {
                            gfx_print(wx+16,wy+76,0x8A8A9A,"Name                  OS                    RAM   CPUs  Disk  Status");
                            gfx_rect(wx+16,wy+92,ww-32,1,0x2A2A4A);
                            for (int _vi=0;_vi<vm_count&&_vi<4;_vi++){
                                int _vy=wy+96+_vi*40;
                                if(_vi==vm_sel){gfx_fill_round_rect(wx+14,_vy-2,ww-28,36,4,0x1A1A4A);}
                                char _vs[2];_vs[0]='0'+(_vi+1)%10;_vs[1]=0;
                                gfx_print(wx+20,_vy+2,0x8899CC,_vs);gfx_print(wx+40,_vy+2,0xFFFFFF,vm_name[_vi]);
                                gfx_print(wx+190,_vy+2,vm_os_color[vm_os[_vi]],vm_os_name[vm_os[_vi]]);
                                char _vr[8];int _ri=0,_rn=vm_ram[_vi];do{_vr[_ri++]='0'+_rn%10;_rn/=10;}while(_rn);_vr[_ri]=0;
                                for(int _rk=0;_rk<_ri/2;_rk++){char _rt=_vr[_rk];_vr[_rk]=_vr[_ri-1-_rk];_vr[_ri-1-_rk]=_rt;}
                                gfx_print(wx+310,_vy+2,0x88AACC,_vr);gfx_print(wx+336,_vy+2,0x6A7A9E,"MB");
                                char _vc[2];_vc[0]='0'+vm_cores[_vi]%10;_vc[1]=0;
                                gfx_print(wx+370,_vy+2,0x88AACC,_vc);
                                char _vd[8];int _di=0,_dn=vm_disk[_vi];do{_vd[_di++]='0'+_dn%10;_dn/=10;}while(_dn);_vd[_di]=0;
                                for(int _dk=0;_dk<_di/2;_dk++){char _dt=_vd[_dk];_vd[_dk]=_vd[_di-1-_dk];_vd[_di-1-_dk]=_dt;}
                                gfx_print(wx+400,_vy+2,0x88AACC,_vd);gfx_print(wx+420,_vy+2,0x6A7A9E,"GB");
                                if(vm_running[_vi]){gfx_print(wx+450,_vy+2,0x44FF44,"Running");}else{gfx_print(wx+450,_vy+2,0x808080,"Stopped");}
                            }
                            gfx_print(wx+16,wy+260,0x3A4A6A,"[Up/Down] select  [Enter] start/stop  [c] create  [d] delete");
                        }
                    } else if (act == 11) { draw_mac_title("Camera");
                        gfx_print(wx+16, wy+44, 0x4A9EFF, "Camera — Hardware Status");
                    } else if (act == 12) { draw_mac_title("Kairo Player");
                        gfx_fill_round_rect(wx+20, wy+48, ww-40, 130, 4, 0x000000);
                        gfx_round_rect(wx+20, wy+48, ww-40, 130, 4, 0x3A5A8A);
                        gfx_print(wx+28, wy+56, 0xFFFFFF, "Kairo Visual Engine");
                        for (int _vy = 0; _vy < 100; _vy++) {
                            for (int _vx = 0; _vx < 320; _vx++) {
                                int _vc = ((_vx * 5) ^ (_vy * 7)) & 0xFF;
                                uint32_t _col = (_vc << 16) | (_vc << 8) | _vc;
                                gfx_putpixel(wx+30 + _vx, wy+70 + _vy, _col);
                            }
                        }
                        gfx_round_rect(wx+30, wy+70, 320, 100, 2, 0x4A6ADF);
                        gfx_print(wx+ww-130, wy+155, 0x00E5FF, "Kairo Audio");
                        gfx_print(wx+ww-130, wy+163, 0x3A5A8A, "Ready");
                        gfx_print(wx+24, wy+196, 0x8A9ACE, "[P] Play  [S] Stop  [Space] Sweep");
                        gfx_print(wx+20, wy+wh-18, 0x3A4A6A, "Powered by Kairo Visual & Kairo Audio");
                    } else if (act == 14) { open_app(14); draw_mac_title("True Video"); }
                    else if (act == 16) { open_app(16); draw_mac_title("Calendar"); }
                    else if (act == 17) { open_app(17); draw_mac_title("Pomodoro Timer"); pom_sec=pom_total; pom_running=0; pom_ticks=0; }
                    else if (act == 18) { open_app(18); draw_mac_title("Weather"); wthr_first=1; }
                    else if (act == 19) { open_app(19); draw_mac_title("System Monitor"); }
                    else if (act == 20) { open_app(20); draw_mac_title("ASCII Art Gallery"); art_sel=0; }
                    else if (act == 21) { open_app(21); draw_mac_title("Typing Test"); type_pos=0; type_err=0; type_ok=0; type_start=0; type_done=0; }
                    else if (act == 22) { open_app(22); draw_mac_title("Clipboard"); clip_save_pos=0; clip_save[0]=0; }
                    else if (act == 23) { open_app(23); draw_mac_title("File Manager"); fm_sel=0; fm_scroll=0; }
                    else if (act == 24) { open_app(24); draw_mac_title("Tetris"); }
                    else if (act == 25) { open_app(25); draw_mac_title("Kairo Games"); }
                    else if (act == 26) { open_app(26); draw_mac_title("Snake");
                        snake_len=3;snake_dir=0;snake_score=0;snake_gameover=0;snake_drop=0;
                        snake_body[0][0]=5;snake_body[0][1]=9;
                        snake_body[1][0]=4;snake_body[1][1]=9;
                        snake_body[2][0]=3;snake_body[2][1]=9;
                        snake_food_x=8;snake_food_y=8;
                    }
                    // Reset search
                    search_focus = 0; search_buf[0] = 0; search_pos = 0; search_sel = 0;
                    gfx_fill_round_rect(sb_x,sb_y,sb_w,sb_h,14,0x0A0A28);
                    gfx_round_rect(sb_x,sb_y,sb_w,sb_h,14,0x2A4A7A);
                    gfx_print(sb_x+12,sb_y+6,0x3A5A8A,"Search...");
                }
                continue;
            }

            if (k == '\b' && search_pos > 0) {
                search_pos--; search_buf[search_pos] = 0; search_sel = 0;
                gfx_fill_round_rect(sb_x,sb_y,sb_w,sb_h,14,0x0A0A28);
                gfx_round_rect(sb_x,sb_y,sb_w,sb_h,14,0x00E5FF);
                gfx_print(sb_x+12,sb_y+6,TEXT,search_buf);
                // Redraw background behind dropdown
                int __dh=match_count*24+12;int __dx=sb_x,__dy=sb_y+sb_h+2;
                for(int __y=__dy-2;__y<__dy+__dh+4;__y++)gfx_rect(__dx-2,__y,sb_w+4,1,0x04040E);
                if (search_pos > 0) dropdown_draw(match,match_count,search_sel);
                continue;
            }
            if (k == 27) {
                // Clear dropdown
                int __dh=match_count*24+12;int __dx=sb_x,__dy=sb_y+sb_h+2;
                for(int __y=__dy-2;__y<__dy+__dh+4;__y++)gfx_rect(__dx-2,__y,sb_w+4,1,0x04040E);
                search_focus = 0; search_buf[0] = 0; search_pos = 0; search_sel = 0;
                gfx_fill_round_rect(sb_x,sb_y,sb_w,sb_h,14,0x0A0A28);
                gfx_round_rect(sb_x,sb_y,sb_w,sb_h,14,0x2A4A7A);
                gfx_print(sb_x+12,sb_y+6,0x3A5A8A,"Search...");
                continue;
            }
            if (search_pos < 63 && k >= ' ' && k <= '~') {
                search_buf[search_pos++] = k; search_buf[search_pos] = 0; search_sel = 0;
                gfx_fill_round_rect(sb_x,sb_y,sb_w,sb_h,14,0x0A0A28);
                gfx_round_rect(sb_x,sb_y,sb_w,sb_h,14,0x00E5FF);
                gfx_print(sb_x+12,sb_y+6,TEXT,search_buf);
                // Recompute matches for dropdown
                match_count = 0;
                for (int mi = 0; mi < MAX_SEARCH; mi++) {
                    const char *si = sitems[mi];
                    int mm = 1;
                    for (int sj = 0; search_buf[sj]; sj++) {
                        char c1 = search_buf[sj], c2 = si[sj];
                        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
                        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
                        if (c1 != c2) { mm = 0; break; }
                    }
                    if (mm) match[match_count++] = mi;
                }
                // Clear old dropdown and redraw
                int __dh=MAX_SEARCH*24+12;int __dx=sb_x,__dy=sb_y+sb_h+2;
                for(int __y=__dy-2;__y<__dy+__dh+4;__y++)gfx_rect(__dx-2,__y,sb_w+4,1,0x04040E);
                if (match_count > 0) dropdown_draw(match,match_count,search_sel);
                continue;
            }
        }
        // Christmas overlay effects (snow over everything + Santa)
        if (_natal_mode && win) {
            // Snowflakes OVER windows
            for (int _nf = 0; _nf < 32; _nf++) {
                int _sx = (_natal_snow[_nf][0]*3+_natal_snow[_nf*2+1][1]*7)%w;
                int _sy = (_natal_snow[_nf][1]*5+_nf*13)%h;
                gfx_putpixel(_sx, _sy, 0xFFFFFF44);
                gfx_putpixel(_sx, _sy+1, 0xFFFFFF44);
            }
            // Flying Santa
            _santa_x += 2;
            if (_santa_x > w + 80) _santa_x = -100;
            _santa_f++;
            // Sleigh body
            gfx_fill_round_rect(_santa_x, _santa_y, 40, 12, 4, 0xCC2222);
            gfx_fill_round_rect(_santa_x+2, _santa_y-3, 36, 6, 3, 0xDD4444);
            gfx_rect(_santa_x+30, _santa_y-2, 8, 4, 0xAA1111);
            // Santa in sleigh
            gfx_fill_round_rect(_santa_x+10, _santa_y-14, 12, 12, 6, 0xFF4444); // body
            gfx_fill_round_rect(_santa_x+12, _santa_y-20, 8, 8, 4, 0xFFDDCC); // head
            gfx_fill_round_rect(_santa_x+11, _santa_y-16, 10, 4, 2, 0xFFFFFF); // beard
            gfx_putpixel(_santa_x+14, _santa_y-18, 0x000000); // eye
            gfx_fill_round_rect(_santa_x+12, _santa_y-22, 8, 3, 2, 0xFF2222); // hat
            gfx_fill_round_rect(_santa_x+13, _santa_y-23, 6, 2, 1, 0xFFFFFF); // hat trim
            // Reindeer
            int _rdx = _santa_x - 10 + ((_santa_f/10)%2)*2;
            gfx_fill_round_rect(_rdx, _santa_y-6, 14, 8, 4, 0x886644);
            gfx_fill_round_rect(_rdx+10, _santa_y-14, 6, 10, 3, 0x886644); // head
            gfx_fill_round_rect(_rdx+10, _santa_y-18, 2, 6, 1, 0x664422); // antler
            gfx_fill_round_rect(_rdx+13, _santa_y-18, 2, 6, 1, 0x664422); // antler
            gfx_putpixel(_rdx+14, _santa_y-10, 0xFF4444); // nose (Rudolph!)
            // Sparkle trail
            for (int _sp = 0; _sp < 6; _sp++) {
                int _spx = _santa_x - _sp*6, _spy = _santa_y + 6 + (_sp%3)*2;
                gfx_putpixel(_spx, _spy, 0xFFDD44 - _sp*0x222200);
                gfx_putpixel(_spx, _spy+1, 0xFFDD44 - _sp*0x222200);
            }
        }
        asm volatile("hlt");
    }
}

// Simple string match helper (case-insensitive prefix)
int strmatch(const char *s, const char *t) {
    int i;
    for (i = 0; s[i] && t[i]; i++) {
        char c1 = s[i], c2 = t[i];
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return 0;
    }
    return s[i] == 0 || t[i] == 0;
}