#include "kernel.h"
#include "lib/io.h"
#include "lib/framebuffer.c"

#include "gdt/gdt.c"
#include "tss/tss.c"
#include "idt/idt.c"
#include "interrupts/isr.c"
#include "interrupts/irq.c"
#include "memory/paging.c"
#include "memory/heap.c"
#include "memory/mmu.c"
#include "drivers/graphics/gfx.c"
#include "drivers/mouse/mouse.c"
#include "drivers/keyboard/keyboard.c"
#include "drivers/usb/usb.c"
#include "drivers/camera/camera.c"
#include "drivers/bluetooth/bt.c"
#include "drivers/audio/ac97.c"
#include "scheduler/scheduler.c"
#include "scheduler/process.c"
#include "usermode/usermode.c"
#include "boot_screen.c"
#include "launch_pad.c"




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
    mmu_init();
    kheap_init();
}

void kernel_drivers(void) {
    keyboard_init();
    mouse_init();
    pci_scan();
    ac97_init();
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

#define BG      0x0A0A28
#define BORDER  0x00E5FF
#define ACCENT  0x4A9EFF
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

static int serial_read_timeout(char *c, int max_loops) {
    for (int _i = 0; _i < max_loops; _i++) {
        if (serial_available()) { *c = inb(SERIAL_PORT + 0); return 1; }
        for (volatile int _d = 0; _d < 500000; _d++);
    }
    return 0;
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

void kernel_main(void) {
    kernel_early();

    __asm__ volatile("sti");

    __asm__ volatile("movl 0x70000, %0" : "=r"(g_fb_addr) : : "memory");
    if (!g_fb_addr) g_fb_addr = 0xFD000000;

    int vbe_w = 1024, vbe_h = 768;
    __asm__ volatile("movzwl 0x70004, %0" : "=r"(vbe_w) : : "memory");
    __asm__ volatile("movzwl 0x70008, %0" : "=r"(vbe_h) : : "memory");
    if (vbe_w < 640 || vbe_h < 480) { vbe_w = 1024; vbe_h = 768; }

    kernel_memory(g_fb_addr);

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

    // Boot screen animation
    boot_screen(vbe_w, vbe_h);

    // Init serial port for social proxy
    serial_init();

    int w = vbe_w, h = vbe_h, win = 0, win_type = 0;
    int search_focus = 1, search_pos = 0, search_sel = 0;
    char search_buf[64] = {0};
    char term_buf[128] = {0}; int term_pos = 0, term_line_count = 0, term_scroll = 0;
    char term_lines[60][80]; for (int _t=0;_t<60;_t++) term_lines[_t][0]=0;
    char chat_buf[128] = {0}; int chat_pos = 0, chat_line_count = 0, chat_scroll = 0;
    char chat_lines[60][80]; for (int _c=0;_c<60;_c++) chat_lines[_c][0]=0;
    char notes[10][80]; int note_count = 0, note_sel = 0;
    char note_buf[80] = {0}; int note_pos = 0;
    int set_state = 0, set_cat = 0;
    int usb_popup = 0;
    int apps_installed[26] = {1,1,1,1,1,0,0,0,1,0,0,0,1,0,1,0,1,1,0,0,0,0,0,0,0,0};
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
const char *fm_files[8] = {"Documents/","Pictures/","Music/","Videos/","Projects/","README.txt","config.ini","notes.txt"};
int fm_is_dir[8] = {1,1,1,1,1,0,0,0};
// Command Palette
int cmd_active = 0, cmd_pos = 0; char cmd_buf[64] = {0};
// Control Center
int cc_active = 0, cc_sel = 0;
// Notification Center
int nc_active = 0, nc_sel = 0;
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
// Paint
#define _PW 80
#define _PH 60
uint32_t _paint_cv[_PW * _PH];
int _paint_col = 0xFF4444, _paint_lastx = -1, _paint_lasty = -1, _paint_size = 2;
// Maze
#define _MW 16
#define _MH 12
int _maze_map[_MW*_MH], _maze_px=1, _maze_py=1, _maze_ex=14, _maze_ey=10, _maze_win=0;
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

    // ─── First-boot Setup Wizard (like a new phone) ───
    #define draw_toggle(tx,ty,ton) do {\
        if(ton){gfx_fill_round_rect(tx,ty,40,20,10,0x006644);gfx_fill_round_rect(tx+20,ty+3,14,14,7,0x00FF88);}\
        else{gfx_fill_round_rect(tx,ty,40,20,10,0x2A2A2A);gfx_fill_round_rect(tx+6,ty+3,14,14,7,0x6A6A6A);}\
    } while(0)
    #define NET_MAX 12
    int setup_done = 0, setup_step = 0;
    // 0=Welcome, 1=Personalize, 2=Network/Wi-Fi, 3=Ready
    while (!setup_done) {
        gfx_clear(0x080818);
        gfx_fill_round_rect(0,0,w,h,12,0x0A0A22);
        gfx_round_rect(2,2,w-4,h-4,10,BORDER);
        // Step dots (bigger)
        for (int si=0;si<4;si++){
            int dx=w/2-54+si*36;
            gfx_fill_round_rect(dx,16,24,24,12,si==setup_step?0x4488FF:0x2A2A4A);
            char sn[2]={'1'+si,0}; gfx_print(dx+6,20,si==setup_step?0xFFFFFF:0x6A7A9E,sn);
        }
        if (setup_step==0){
            gfx_print_scaled(w/2-170,60,BORDER,"WELCOME TO VITEZA",3);
            gfx_print_scaled(w/2-120,140,0x8A9ACE,"Your new OS is ready.",1);
            gfx_print_scaled(w/2-105,172,0x6A7A9E,"Let's set things up.",1);
            gfx_fill_round_rect(w/2-100,230,200,32,8,0x1A3A8A);
            gfx_print_scaled(w/2-42,236,0xFFFFFF,"[Enter] Start",1);
            gfx_print_scaled(w/2-120,280,0x3A4A6A,"[Esc] skip to desktop",1);
            char k=keyboard_last_char();
            if(k=='\n')setup_step=1;
            if(k==27){setup_done=1;break;}
        }else if(setup_step==1){
            gfx_print_scaled(w/2-120,60,BORDER,"PERSONALIZE",3);
            gfx_print_scaled(w/2-140,120,0x6A7A9E,"Configure your preferences:",1);
            draw_toggle(w/2-160,170,set_state&1);
            gfx_print_scaled(w/2-105,173,0x8A9ACE,"Dark Mode     [1]",1);
            draw_toggle(w/2-160,220,set_state&2);
            gfx_print_scaled(w/2-105,223,0x8A9ACE,"Notifications [2]",1);
            draw_toggle(w/2-160,270,set_state&4);
            gfx_print_scaled(w/2-105,273,0x8A9ACE,"Developer Mode [3]",1);
            gfx_print_scaled(w/2-130,330,0x6A7A9E,"[1-3] toggle  [Enter] next",1);
            gfx_print_scaled(w/2-120,356,0x3A4A6A,"[Esc] skip to desktop",1);
            char k=keyboard_last_char();
            if(k=='1')set_state^=1;
            if(k=='2')set_state^=2;
            if(k=='3')set_state^=4;
            if(k=='\n')setup_step=2;
            if(k==27){setup_done=1;break;}
        }else if(setup_step==2){
            // Wi-Fi selection (integrated into wizard)
            gfx_print_scaled(w/2-130,60,BORDER,"WI-FI NETWORKS",3);
            gfx_rect(w/2-300,96,600,1,0x2A5EAF);
            int lx=80,ly=110,lw=864,lh=440;
            gfx_rect(lx,ly,lw,lh,PANEL_BG);
            gfx_round_rect(lx,ly,lw,lh,6,ACCENT);
            static const char *net_names[NET_MAX]={"HOME-5G","OFFICE-NET","GUEST-WIFI","ANIMATEOS-LAB","NEIGHBOR-2G","PUBLIC-HOTSPOT","SCHOOL-CAMPUS","FIBRA-OPTI","SMART-HOME","ROOFTOP-ROOF","CAFE-FREE","DORM-ROOM"};
            static int net_sig[NET_MAX]={4,3,2,4,1,3,2,4,2,1,2,3};
            static int net_sec[NET_MAX]={1,1,0,1,1,0,1,1,1,0,0,1};
            static int sel=0,scroll=0,connecting=0,connected=0;
            int max_vis=5;
            if(sel<scroll)scroll=sel;
            if(sel>=scroll+max_vis)scroll=sel-max_vis+1;
            for(int i=scroll;i<NET_MAX&&i<scroll+max_vis;i++){
                int idx=i-scroll,ey=ly+12+idx*76;
                if(i==sel&&!connecting)gfx_fill_round_rect(lx+8,ey-2,lw-16,68,6,0x15154A);
                int sx=lx+24;
                for(int b=0;b<4;b++){int bh=(b<net_sig[i])?(5+b*5):3;gfx_rect(sx+b*12,ey+28-bh,8,bh,i==sel?0x4488FF:0x6A7A9E);}
                if(net_sec[i]){gfx_rect(sx+52,ey+4,10,6,0x6A7A9E);gfx_rect(sx+49,ey+10,16,12,0x6A7A9E);gfx_rect(sx+55,ey+14,4,5,0x0A0A22);}
                int tx=sx+76;
                gfx_print_scaled(tx,ey,i==sel?0x4488FF:0x8A9ACE,net_names[i],1);
                gfx_print_scaled(tx,ey+24,0x4A5A7E,net_sec[i]?"WPA2-PSK":"Open",1);
                char _pct[4];_pct[0]='0'+net_sig[i];_pct[1]='/';_pct[2]='4';_pct[3]=0;
                gfx_print_scaled(lx+lw-80,ey+6,0x4A5A7E,_pct,1);
            }
            gfx_print_scaled(lx+20,ly+lh-24,0x3A4A6A,"[Up/Down] select  [Space] Connect  [Enter] skip",1);
            char k=keyboard_last_char();
            if(!connecting&&!connected){
                if(k==KEY_UP&&sel>0)sel--;
                if(k==KEY_DOWN&&sel<NET_MAX-1)sel++;
                if(k==' '){connecting=1;connected=0;}
            }
            if(connecting&&!connected){delay();connected=1;connecting=0;}
            if(k=='\n')setup_step=3;
            if(k==27){setup_done=1;break;}
        }else if(setup_step==3){
            gfx_print_scaled(w/2-100,60,BORDER,"ALL SET!",3);
            gfx_print_scaled(w/2-150,140,0x44FF44,"Your Viteza OS is ready.",1);
            if(set_state&1)gfx_print_scaled(w/2-140,200,0x6A7A9E,"Dark Mode: ON",1);
            if(set_state&2)gfx_print_scaled(w/2-140,230,0x6A7A9E,"Notifications: ON",1);
            if(set_state&4)gfx_print_scaled(w/2-140,260,0x6A7A9E,"Developer Mode: ON",1);
            gfx_print_scaled(w/2-130,320,0x8A9ACE,"Press [Enter] to start.",1);
            char k=keyboard_last_char();
            if(k=='\n')setup_done=1;
            if(k==27){setup_done=1;break;}
        }
        asm volatile("hlt");
    }
    keyboard_last_char();
    delay();

    // ─── Desktop (macOS style) ───
redraw_desktop:
    gfx_clear(0x080818);
    int dm = set_state & 1;
    // Rich gradient (deep space → nebula → horizon)
    for (int y = 0; y < h; y++) {
        int t = y * 255 / h;
        int r, g, b;
        if (_natal_mode) {
            r = 4 + t/30; g = 4 + t/25; b = 16 + t/8;
            if (y < 100) { int f = 100-y; r = 8+f/12; g = 10+f/15; b = 40+f/6; }
            if (y > h-80) { int f = y-(h-80); r += f/6; g += f/4; b += f/3; }
            if (r > 80) r = 80; if (g > 80) g = 80; if (b > 120) b = 120;
        } else if (dm) {
            r = 1 + t/80;  g = 1 + t/60;  b = 4 + t/40;
            if (y < 100) { int f = 100-y; r = 3+f/50; g = 2+f/40; b = 10+f/20; }
            if (r > 6) r = 6; if (g > 5) g = 5; if (b > 16) b = 16;
        } else {
            r = 4 + t/40;  g = 3 + t/25;  b = 16 + t/12;
            if (y < 150) { int f = 150-y; r = 16+f/16; g = 10+f/24; b = 44+f/8; }
            if (r > 32) r = 32; if (g > 28) g = 28; if (b > 64) b = 64;
            // Nebula accent near top
            if (y > 80 && y < 280) {
                int nt = y - 80;
                r += (nt < 100 ? nt : 200-nt) / 6;
                g += (nt < 100 ? nt : 200-nt) / 10;
            }
        }
        if (r > 60) r = 60; if (g > 55) g = 55; if (b > 80) b = 80;
        gfx_rect(0, y, w, 1, (r<<16)|(g<<8)|b);
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

    // ─── Top menu bar (macOS style) ───
    int mby = 0, mbh = 24;
    dm = set_state & 1;
    if (_natal_mode) {
        gfx_rect(0, mby, w, mbh, 0x0A0400);
        gfx_rect(0, mby+mbh-1, w, 1, 0x660000);
        gfx_fill_round_rect(7, 5, 12, 12, 6, 0x440000);
        gfx_fill_round_rect(7, 5, 12, 12, 6, 0xCC2222);
        gfx_fill_round_rect(8, 6, 10, 5, 4, 0xFF4444);
        gfx_putpixel(12, 10, 0xFFFF44);
        gfx_print(24, 5, 0xFF4444, "Viteza");
        gfx_print(w-200, 5, 0x44FF44, "🎄 BUON NATALE!");
    } else {
        gfx_rect(0, mby, w, mbh, dm ? 0x06060E : 0x0A0A1C);
        gfx_rect(0, mby+mbh-1, w, 1, dm ? 0x101020 : 0x1A1A3A);
        gfx_fill_round_rect(7, 5, 12, 12, 6, 0x000018);
        gfx_fill_round_rect(7, 5, 12, 12, 6, dm ? 0x2A4A8A : 0x3A6AFF);
        gfx_fill_round_rect(8, 6, 10, 5, 4, dm ? 0x4A7ACC : 0x6A9AFF);
        gfx_putpixel(12, 10, dm ? 0x88CCFF : 0xFFFFFF);
        gfx_print(24, 5, dm ? 0x6677AA : 0x8899CC, "Viteza");
    }
    // App name when window is open
    _app_name = "Finder";
    if(win && win_type > 0 && win_type < 33) {
        switch(win_type){ case 3:_app_name="Terminal";break; case 4:_app_name="Settings";break; case 5:_app_name="OreoAI";break; case 6:_app_name="Calculator";break; case 7:_app_name="Notes";break; case 8:_app_name="App Store";break; case 9:_app_name="Studio";break; case 10:_app_name="KairoVM";break; case 11:_app_name="Camera";break; case 12:_app_name="Player";break; case 14:_app_name="TrueVideo";break; case 16:_app_name="Calendar";break; case 17:_app_name="Pomodoro";break; case 18:_app_name="Weather";break; case 19:_app_name="Monitor";break; case 20:_app_name="Art";break; case 21:_app_name="Typing";break; case 22:_app_name="Clipboard";break; case 23:_app_name="Files";break; case 24:_app_name="Tetris";break; case 25:_app_name="Games";break; case 26:_app_name="Snake";break; case 28:_app_name="Wii";break; case 29:_app_name="Mic Test";break; case 30:_app_name="Pong";break; case 31:_app_name="Paint";break; case 32:_app_name="Maze";break;
        }
        gfx_rect(85, 6, 2, 12, dm ? 0x1A1A3A : 0x2A2A5A);
        gfx_print(92, 5, dm ? 0x8899CC : 0xAABBEE, _app_name);
    }
    int _hr = 12, _mn = 0; rtc_read(&_hr, &_mn);
    char _time[6]; _time[0]='0'+_hr/10; _time[1]='0'+_hr%10; _time[2]=':';
    _time[3]='0'+_mn/10; _time[4]='0'+_mn%10; _time[5]=0;
    // Right-side items in menu bar
    int mb_rx = w-10, _clk_x;
    uint32_t _mic = _natal_mode ? 0xFF4444 : 0x5566AA;
    uint32_t _mic2 = _natal_mode ? 0x44FF44 : 0x5566AA;
    // Battery
    gfx_round_rect(mb_rx-24, 6, 18, 10, 2, _mic);
    gfx_rect(mb_rx-6, 8, 3, 6, _mic);
    gfx_fill_round_rect(mb_rx-22, 8, 10, 6, 2, _natal_mode ? 0xFF4444 : 0x44AA66);
    mb_rx -= 34;
    // Clock
    _clk_x = mb_rx-50;
    gfx_print(_clk_x, 5, _natal_mode ? 0xFF6666 : TEXT, _time);
    mb_rx -= 56;
    // Christmas countdown
    if (_natal_mode) {
        int _mm, _dd; rtc_read_date(&_mm, &_dd);
        int _left = 25 - _dd;
        if (_mm == 12 && _left >= 0) {
            char _cd[20]; int _ci=0;
            if (_left == 0) { _cd[0]='N';_cd[1]='A';_cd[2]='T';_cd[3]='A';_cd[4]='L';_cd[5]='E';_cd[6]=0; }
            else if (_left < 10) { _cd[0]='0'+_left; _cd[1]='d'; _cd[2]=0; }
            else { _cd[0]='0'+_left/10; _cd[1]='0'+_left%10; _cd[2]='d'; _cd[3]=0; }
            mb_rx -= 40;
            gfx_print(mb_rx+2, 5, 0x44FF44, _cd);
        }
    }
    // Notification Center icon (bell)
    nc_icon_x = mb_rx - 16;
    gfx_round_rect(nc_icon_x, 5, 12, 12, 2, _mic2);
    gfx_fill_round_rect(nc_icon_x+4, 4, 4, 2, 1, _mic2);
    gfx_fill_round_rect(nc_icon_x+3, 9, 6, 2, 1, _mic2);
    gfx_rect(nc_icon_x+4, 10, 4, 2, _mic2);
    mb_rx -= 20;
    // Control Center icon (3 lines)
    cc_icon_x = mb_rx - 14;
    gfx_rect(cc_icon_x, 7, 10, 2, _mic2);
    gfx_rect(cc_icon_x, 11, 10, 2, _mic2);
    gfx_rect(cc_icon_x, 15, 10, 2, _mic2);
    mb_rx -= 18;
    // Wi-Fi icon
    wifi_icon_x = mb_rx - 16;
    gfx_fill_round_rect(mb_rx-16, 8, 10, 2, 1, _mic2);
    gfx_putpixel(mb_rx-11, 7, _mic2);
    gfx_putpixel(mb_rx-11, 15, _mic2);
    // Separator
    mb_rx -= 14;
    gfx_rect(mb_rx-10, 5, 1, 14, _natal_mode ? 0x440000 : 0x1A1A3A);

    // ─── Dock (macOS style) ───
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

    // ─── Dock Icons — macOS quality, no pixels ───
    int di_y = dc_y + 8, di_sz = 40, di_sp = 56;
    int di_base = dc_x + (dc_w - 10*di_sp)/2;
    int di_x, di_cy;

    #define di_sh(_i,_c) do {\
        di_x = di_base + (_i)*di_sp; di_cy = di_y;\
        gfx_fill_round_rect(di_x+1,di_cy+1,di_sz,di_sz,9,0x000000);\
        if (_natal_mode) {\
            uint32_t _gg = (_i%2==0)?0x44FF4444:0xFF444444;\
            for (int _ni = 0; _ni < 4; _ni++) {\
                uint32_t _gac = (_i%2==0)?(0x440000+_ni*0x004400):(0x000044+_ni*0x440000);\
                gfx_fill_round_rect(di_x-2-_ni,di_cy-2-_ni,di_sz+4+_ni*2,di_sz+4+_ni*2,12+_ni*2,_gac);\
            }\
        }\
        gfx_fill_round_rect(di_x,di_cy,di_sz,di_sz,10,_c);\
        gfx_round_rect(di_x,di_cy,di_sz,di_sz,10,_natal_mode?0xFF4444:0x4A6AAF);\
    } while(0)

    // Kairo Games (0) — game controller icon
    di_sh(0,0xCC44AA); {
        gfx_fill_round_rect(di_x+6,di_cy+12,28,16,6,0x221133);
        gfx_fill_round_rect(di_x+4,di_cy+16,32,8,4,0x661155);
        gfx_fill_round_rect(di_x+8,di_cy+14,8,6,3,0xFF66DD);
        gfx_fill_round_rect(di_x+24,di_cy+14,8,6,3,0xFF66DD);
        gfx_fill_round_rect(di_x+14,di_cy+18,4,4,2,0x44AAFF);
        gfx_fill_round_rect(di_x+22,di_cy+18,4,4,2,0xFFAA44);
        gfx_fill_round_rect(di_x+18,di_cy+8,4,6,2,0xAA3388);
        gfx_fill_round_rect(di_x+19,di_cy+28,2,4,1,0xAA3388);
    }

    // Terminal (1) — real terminal window with buttons
    di_sh(1,0x2A2A3A); {
        gfx_fill_round_rect(di_x+5,di_cy+7,30,28,4,0x0A0A0A);
        gfx_fill_round_rect(di_x+7,di_cy+9,6,6,3,0xFF5A5A);
        gfx_fill_round_rect(di_x+15,di_cy+9,6,6,3,0xDDAA33);
        gfx_fill_round_rect(di_x+23,di_cy+9,6,6,3,0x44CC44);
        gfx_rect(di_x+9,di_cy+19,18,2,0x00FF66);
        gfx_rect(di_x+9,di_cy+23,14,2,0x00FF66);
        gfx_rect(di_x+9,di_cy+27,10,2,0x00FF66);
    }

    // Settings (2) — silver gear
    di_sh(2,0x4A4A5A); {
        gfx_fill_round_rect(di_x+16,di_cy+5,8,6,2,0x8A8A9A);
        gfx_fill_round_rect(di_x+16,di_cy+29,8,6,2,0x8A8A9A);
        gfx_fill_round_rect(di_x+5,di_cy+16,6,8,2,0x8A8A9A);
        gfx_fill_round_rect(di_x+29,di_cy+16,6,8,2,0x8A8A9A);
        gfx_fill_round_rect(di_x+9,di_cy+9,22,22,11,0x9A9AAA);
        gfx_fill_round_rect(di_x+17,di_cy+17,6,6,3,0x4A4A5A);
        gfx_rect(di_x+13,di_cy+11,6,1,0xCCCCDD);
    }

    // OreoAI (3) — chat bubble with tail
    di_sh(3,0x5A3A8A); {
        gfx_fill_round_rect(di_x+5,di_cy+7,30,24,6,0xFFFFFF);
        gfx_fill_round_rect(di_x+5,di_cy+29,12,8,3,0xFFFFFF);
        gfx_rect(di_x+7,di_cy+27,8,4,0xFFFFFF);
        gfx_fill_round_rect(di_x+9,di_cy+11,20,4,2,0x7A5AAA);
        gfx_fill_round_rect(di_x+9,di_cy+17,16,4,2,0x7A5AAA);
        gfx_fill_round_rect(di_x+9,di_cy+23,12,4,2,0x7A5AAA);
    }

    // Calculator (4) — with display and buttons
    di_sh(4,0x3A3A4A); {
        gfx_fill_round_rect(di_x+8,di_cy+8,24,10,2,0x1A1A2A);
        gfx_print(di_x+11,di_cy+9,0x44CC66,"0");
        gfx_fill_round_rect(di_x+8,di_cy+20,7,7,2,0x5A5A6A);
        gfx_fill_round_rect(di_x+17,di_cy+20,7,7,2,0x5A5A6A);
        gfx_fill_round_rect(di_x+26,di_cy+20,7,7,2,0x5A5A6A);
        gfx_fill_round_rect(di_x+8,di_cy+29,7,7,2,0x5A5A6A);
        gfx_fill_round_rect(di_x+17,di_cy+29,7,7,2,0x5A5A6A);
        gfx_fill_round_rect(di_x+26,di_cy+29,7,7,2,0x4A8AFF);
        gfx_print(di_x+27,di_cy+30,0xFFFFFF,"=");
    }

    // Notes (5) — yellow notepad with lines
    di_sh(5,0xDDAA33); {
        gfx_fill_round_rect(di_x+7,di_cy+7,26,26,4,0xFFEE88);
        gfx_rect(di_x+11,di_cy+14,18,2,0xCCAA33);
        gfx_rect(di_x+11,di_cy+18,18,2,0xCCAA33);
        gfx_rect(di_x+11,di_cy+22,18,2,0xCCAA33);
        gfx_fill_round_rect(di_x+24,di_cy+7,8,8,2,0xFF6644);
    }

    // App Store (6) — blue A
    di_sh(6,0x4488FF); {
        gfx_fill_round_rect(di_x+8,di_cy+8,24,24,6,0x88BBFF);
        gfx_print_scaled(di_x+15,di_cy+13,0xFFFFFF,"A",2);
    }

    // Weather (7) — cloud
    di_sh(7,0x6A8ABE); {
        gfx_fill_round_rect(di_x+8,di_cy+14,24,12,6,0xFFFFFF);
        gfx_fill_round_rect(di_x+14,di_cy+8,12,14,7,0xFFFFFF);
        gfx_fill_round_rect(di_x+10,di_cy+16,20,6,3,0xDDEEFF);
    }

    // Tetris (8) — purple T block
    di_sh(8,0x3A1A5A); {
        gfx_fill_round_rect(di_x+10,di_cy+8,20,24,4,0x1A0A2A);
        gfx_fill_round_rect(di_x+14,di_cy+10,12,4,2,0xBB44EE);
        gfx_fill_round_rect(di_x+16,di_cy+14,8,12,2,0xBB44EE);
        gfx_fill_round_rect(di_x+10,di_cy+22,6,6,2,0xBB44EE);
        gfx_fill_round_rect(di_x+24,di_cy+22,6,6,2,0xBB44EE);
    }

    // Viteza Wii (9) — Wii-style white disc + cursor
    di_sh(9,0xFFFFFF); {
        gfx_fill_round_rect(di_x+4,di_cy+4,32,32,16,0xE6E6E6);
        gfx_print_scaled(di_x+11,di_cy+10,0x0055CC,"W",2);
    }
    
    // App indicator dots under active icons (macOS style: small, bright for active)
    int _active_di = -1;
    if(win){
        int _wt = win_type;
        if(_wt==25)_active_di=0; else if(_wt==3)_active_di=1;
        else if(_wt==4)_active_di=2; else if(_wt==5)_active_di=3;
        else if(_wt==6)_active_di=4; else if(_wt==7)_active_di=5;
        else if(_wt==8)_active_di=6; else if(_wt==18)_active_di=7;
        else if(_wt==24)_active_di=8; else if(_wt==28)_active_di=9;
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
    gfx_rect(40,116,190,1,0x1A2A5A);
    gfx_print(42,95,0x6A8ABE,"Notes");
    gfx_fill_round_rect(210,94,18,14,7,0xCC4444);
    gfx_print(216,94,0xFFFFFF,"0");
    gfx_print(42,126,0x3A4A6A,"No notes  [7]");

    // Desktop digital clock widget (right side)
    int dc_x2 = w - 170, dc_y2 = 90;
    gfx_fill_round_rect(dc_x2, dc_y2, 140, 60, 8, 0x080820);
    gfx_round_rect(dc_x2, dc_y2, 140, 60, 8, 0x2A3A6A);
    char _td[9]; _td[0]='0'+_hr/10; _td[1]='0'+_hr%10; _td[2]=':';
    _td[3]='0'+_mn/10; _td[4]='0'+_mn%10; _td[5]=':'; 
    rtc_read(&_hr, &_mn); // refresh time
    _td[5] = 0;
    gfx_print_scaled(dc_x2+16, dc_y2+8, 0x4488FF, _td, 2);
    gfx_print(dc_x2+12, dc_y2+44, 0x3A4A6A, "Viteza OS");

    // Search in top bar instead of dock
    int sb_x = w/2 - 120, sb_y = mbh+3, sb_w = 240, sb_h = 20;
    gfx_fill_round_rect(sb_x, sb_y, sb_w, sb_h, 10, 0x0A0A28);
    gfx_round_rect(sb_x, sb_y, sb_w, sb_h, 10, 0x2A3A6A);
    gfx_print(sb_x+8, sb_y+2, 0x3A5A8A, "Search...");

    // Keyboard shortcut bar (above dock)
    gfx_print(w/2-500, dc_y-18, 0x3A4A6A,
        "[1]  [2]  [3]Term  [4]Set  [5]Oreo  [6]Calc  [7]Notes  [8]Store  [9]Studio  [0]VM  [E]Cal  [R]Pomo  [W]Wthr  [A]Art  [Y]Type  [O]Clip  [F]File  [G]Tet  [K]Games  [L]Lpad  [C]Cam  [V]Player  [U]USB  [Esc]");

    // Search focused by default
    search_focus = 1; int _tick = 0;

    // Clock refresh helper (now in menu bar)
    #define clock_refresh() do {\
        rtc_read(&_hr,&_mn);\
        _time[0]='0'+_hr/10;_time[1]='0'+_hr%10;_time[2]=':';\
        _time[3]='0'+_mn/10;_time[4]='0'+_mn%10;_time[5]=0;\
        gfx_rect(_clk_x,5,55,12,0x0A0A1C);\
        gfx_print(_clk_x,5,TEXT,_time);\
        /* Wi-Fi signal animation */\
        if(wifi_icon_x){\
            int _wsig = (_tick/6) % 5;\
            gfx_rect(wifi_icon_x,6,12,10,0x0A0A1C);\
            for(int _wb=0;_wb<4;_wb++){\
                int _bh = _wb*3+2;\
                uint32_t _wc = (_wb <= _wsig-1) ? 0x66CCFF : 0x2A3A5A;\
                gfx_fill_round_rect(wifi_icon_x+1+_wb*3,15-_bh,2,_bh,1,_wc);\
            }\
        }\
        /* Refresh desktop digital clock widget */\
        if(!win){\
            char _wd[9];_wd[0]='0'+_hr/10;_wd[1]='0'+_hr%10;_wd[2]=':';\
            _wd[3]='0'+_mn/10;_wd[4]='0'+_mn%10;_wd[5]=0;\
            int _dmc = set_state & 1;\
            gfx_fill_round_rect(dc_x2,dc_y2,140,60,8,_dmc?0x040410:0x080820);\
            gfx_print_scaled(dc_x2+16,dc_y2+8,_dmc?0x3366CC:0x4488FF,_wd,2);\
            gfx_print(dc_x2+12,dc_y2+44,_dmc?0x1A2A4A:0x3A4A6A,"Viteza OS");\
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

    #define settings_redraw() do {\
        int _dmc = set_state & 1;\
        int _sx = wx+16, _sy = wy+54, _sw = ww-32, _sh = wh-60;\
        gfx_rect(_sx, _sy, _sw, _sh, _dmc ? 0x03030E : 0x06061A);\
        int _sbw = 110;\
        gfx_rect(_sx, _sy, _sbw, _sh, _dmc ? 0x050512 : 0x0A0A24);\
        const char *_cats[] = {"General","Display","Network","Sound","About","Parental"};\
        for(int _ci=0;_ci<6;_ci++){\
            int _cy = _sy + 8 + _ci*30;\
            if(_ci==set_cat){\
                gfx_rect(_sx+2,_cy,_sbw-4,24,0x1A3A8A);\
                gfx_print(_sx+12,_cy+6,0xFFFFFF,_cats[_ci]);\
            }else{\
                gfx_print(_sx+12,_cy+6,_dmc ? 0x4A5A7E : 0x6A7A9E,_cats[_ci]);\
            }\
        }\
        gfx_rect(_sx+_sbw,_sy,1,_sh,_dmc ? 0x0E0E2A : 0x1A1A4A);\
        int _cx = _sx+_sbw+16, _cy2 = _sy+12;\
        if(set_cat==0){\
            gfx_print(_cx,_cy2,0x4488FF,"General");_cy2+=22;\
            draw_toggle(_cx,_cy2,set_state&1);\
            gfx_print(_cx+32,_cy2-1,set_state&1?0x4488FF:0x8A9ACE,"Dark Mode");_cy2+=28;\
            draw_toggle(_cx,_cy2,set_state&2);\
            gfx_print(_cx+32,_cy2-1,0x8A9ACE,"Notifications");_cy2+=28;\
            draw_toggle(_cx,_cy2,set_state&4);\
            gfx_print(_cx+32,_cy2-1,0x8A9ACE,"Developer Mode");_cy2+=28;\
        }else if(set_cat==1){\
            gfx_print(_cx,_cy2,0x4488FF,"Display");_cy2+=22;\
            gfx_print(_cx,_cy2,_dmc?0x4A5A7E:0x6A7A9E,"1280x720");_cy2+=20;\
            gfx_print(_cx,_cy2,_dmc?0x2A3A5E:0x4A5A7E,"Brightness");_cy2+=16;\
            gfx_rect(_cx,_cy2,160,4,0x222244);\
            gfx_rect(_cx,_cy2,120,4,0x4488FF);\
            gfx_rect(_cx+118,_cy2-1,6,6,0x66BBFF);_cy2+=24;\
            gfx_print(_cx,_cy2,0x3A4A6A,"VBE Bochs Graphics");_cy2+=16;\
            gfx_print(_cx,_cy2,0x3A4A6A,"32-bit color");\
        }else if(set_cat==2){\
            gfx_print(_cx,_cy2,0x4488FF,"Network");_cy2+=22;\
            draw_toggle(_cx,_cy2,set_state&8);\
            gfx_print(_cx+32,_cy2-1,0x8A9ACE,"Wi-Fi");_cy2+=28;\
            gfx_print(_cx,_cy2,_dmc?0x2A3A5E:0x4A5A7E,"Status:");\
            gfx_print(_cx+56,_cy2,set_state&8?0x44FF44:0xFF6644,set_state&8?"Connected":"Disconnected");_cy2+=20;\
            gfx_print(_cx,_cy2,0x3A4A6A,"IP: 10.0.2.15");_cy2+=16;\
            gfx_print(_cx,_cy2,0x3A4A6A,"via serial proxy");\
        }else if(set_cat==3){\
            gfx_print(_cx,_cy2,0x4488FF,"Sound");_cy2+=22;\
            gfx_print(_cx,_cy2,_dmc?0x4A5A7E:0x6A7A9E,"Output: PC Speaker");_cy2+=20;\
            gfx_print(_cx,_cy2,_dmc?0x2A3A5E:0x4A5A7E,"Volume");_cy2+=16;\
            gfx_rect(_cx,_cy2,160,4,0x222244);\
            gfx_rect(_cx,_cy2,80,4,0x4488FF);\
            gfx_rect(_cx+78,_cy2-1,6,6,0x66BBFF);_cy2+=24;\
            gfx_print(_cx,_cy2,0x3A4A6A,"[Space] Test speaker");\
        }else if(set_cat==4){\
            gfx_print(_cx,_cy2,0x4488FF,"About Viteza OS");_cy2+=24;\
            gfx_print(_cx,_cy2,0x8A9ACE,"Version: 1.0");_cy2+=20;\
            gfx_print(_cx,_cy2,_dmc?0x4A5A7E:0x6A7A9E,"Kernel: x86_64 Long Mode");_cy2+=20;\
            gfx_print(_cx,_cy2,_dmc?0x4A5A7E:0x6A7A9E,"RAM: 256 MB");_cy2+=20;\
            gfx_print(_cx,_cy2,_dmc?0x4A5A7E:0x6A7A9E,"Display: VBE 1280x720");_cy2+=20;\
            gfx_print(_cx,_cy2,_dmc?0x4A5A7E:0x6A7A9E,"AI: OreoAI + Ollama");_cy2+=20;\
            gfx_print(_cx,_cy2,_dmc?0x2A3A5A:0x4A5A6A,"Built with love");_cy2+=20;\
        }else if(set_cat==5){\
            gfx_print(_cx,_cy2,0xFF6644,"Parental Control");_cy2+=22;\
            draw_toggle(_cx,_cy2,set_state&16);\
            gfx_print(_cx+32,_cy2-1,set_state&16?0xFF6644:0x8A9ACE,"App Lock");_cy2+=28;\
            draw_toggle(_cx,_cy2,set_state&32);\
            gfx_print(_cx+32,_cy2-1,set_state&32?0xFF6644:0x8A9ACE,"Website Filter");_cy2+=28;\
            draw_toggle(_cx,_cy2,set_state&64);\
            gfx_print(_cx+32,_cy2-1,set_state&64?0xFF6644:0x8A9ACE,"Time Limits");_cy2+=28;\
            draw_toggle(_cx,_cy2,set_state&128);\
            gfx_print(_cx+32,_cy2-1,set_state&128?0xFF6644:0x8A9ACE,"Content Rating");_cy2+=28;\
        }\
        gfx_print(_sx+8,_sy+_sh-14,_dmc?0x18182A:0x2A3A5A,"[U/D] nav  [1-5] toggle  [Esc]");\
    } while(0)

    #define MAX_SEARCH 24
    const char *sitems[MAX_SEARCH] = {"This PC","Network","Terminal","Settings","OreoAI","Calculator","Notes","App Store","Kairo Studio","KairoVM","Camera","Kairo Player","True Video","Calendar","Pomodoro","Weather","Disk Usage","ASCII Art","Typing Test","Clipboard","File Manager","Tetris","Kairo Games","Snake"};
    int saction[MAX_SEARCH] = {1,2,3,4,5,6,7,8,9,10,11,12,14,16,17,18,19,20,21,22,23,24,25,26};

    #define APP_COUNT 25
    const char *app_names[APP_COUNT] = {
        "Kairo Studio","Terminal","Calculator","Notes","OreoAI",
        "File Manager","Image Viewer","Text Editor","Settings",
        "System Monitor","Code Compiler","Web Browser","Kairo Player",
        "Music Player","True Video","Clock","KairoVM","Camera",
        "Calendar","Pomodoro","Weather","Disk Usage","ASCII Art","Typing Test","Tetris"
    };
    const char *app_cats[APP_COUNT] = {
        "Development","System","Utilities","Productivity","AI",
        "System","Multimedia","Productivity","System",
        "System","Development","Internet","Multimedia",
        "Multimedia","Multimedia","Utilities","Virtualization","Multimedia",
        "Productivity","Productivity","Utilities","System","Entertainment","Productivity","Game"
    };
    uint32_t app_colors[APP_COUNT] = {
        0x6A5ACD,0x00CC44,0x4A9EFF,0xFFAA00,0xBB88FF,
        0x4A7AFF,0xFF8844,0x88AACC,0x8A8A9A,
        0x44CCAA,0xFF6644,0x44AAFF,0xCC4466,
        0xFF66AA,0xFF4444,0x88AACC,0xCC4444,0x66DDFF,
        0xDD8844,0xFF5544,0x44CCDD,0x44FFAA,0xFF88CC,0x88CC44,0x44DDFF
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
        }else if(_act==4){open_app(4);draw_mac_title("System Settings");settings_redraw();\
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
        }\
    } while(0)

    int wx=w/2-200, wy=h/2-160, ww=400, wh=280;

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

    // macOS window chrome — realistic traffic light buttons with shine
    #define draw_mac_title(k_) do {\
        int _dmc = set_state & 1;\
        if (_natal_mode) {\
            gfx_fill_round_rect(wx+4,wy+4,ww-8,30,8,0x1A0404);\
            gfx_fill_round_rect(wx+4,wy+4,ww-8,20,8,0x2A0A0A);\
            gfx_rect(wx+8,wy+22,ww-16,12,0x1A0404);\
            gfx_rect(wx+4,wy+35,ww-8,1,0x660000);\
            gfx_print(wx+ww/2-28,wy+7,0xFF6644,k_);\
            gfx_fill_round_rect(wx+10,wy+8,12,12,6,0xCC3333);\
            gfx_fill_round_rect(wx+12,wy+9,4,3,2,0xFF6666);\
            gfx_fill_round_rect(wx+11,wy+9,2,2,1,0xFF9999);\
            gfx_fill_round_rect(wx+26,wy+8,12,12,6,0x44AA44);\
            gfx_fill_round_rect(wx+28,wy+9,4,3,2,0x66DD66);\
            gfx_fill_round_rect(wx+27,wy+9,2,2,1,0x88EE88);\
            gfx_fill_round_rect(wx+42,wy+8,12,12,6,0xDDAA22);\
            gfx_fill_round_rect(wx+44,wy+9,4,3,2,0xEECC44);\
            gfx_fill_round_rect(wx+43,wy+9,2,2,1,0xFFEE66);\
        } else {\
            gfx_fill_round_rect(wx+4,wy+4,ww-8,30,8,_dmc?0x141430:0x1E1E3E);\
            gfx_fill_round_rect(wx+4,wy+4,ww-8,20,8,_dmc?0x1A1A38:0x282852);\
            gfx_rect(wx+8,wy+22,ww-16,12,_dmc?0x141430:0x1E1E3E);\
            gfx_rect(wx+4,wy+35,ww-8,1,_dmc?0x24244A:0x3A3A6A);\
            gfx_print(wx+ww/2-28,wy+7,_dmc?0x667799:0x8899CC,k_);\
            gfx_fill_round_rect(wx+10,wy+8,12,12,6,0xDD4A4A);\
            gfx_fill_round_rect(wx+12,wy+9,4,3,2,0xFF7A7A);\
            gfx_fill_round_rect(wx+11,wy+9,2,2,1,0xFF9999);\
            gfx_fill_round_rect(wx+26,wy+8,12,12,6,0xCCAA33);\
            gfx_fill_round_rect(wx+28,wy+9,4,3,2,0xEECC66);\
            gfx_fill_round_rect(wx+27,wy+9,2,2,1,0xEEDD88);\
            gfx_fill_round_rect(wx+42,wy+8,12,12,6,0x44AA44);\
            gfx_fill_round_rect(wx+44,wy+9,4,3,2,0x66DD66);\
            gfx_fill_round_rect(wx+43,wy+9,2,2,1,0x88EE88);\
        }\
    } while(0)

    #define open_app(n) do {\
        win=1;win_type=n;\
        /* Set app name for menu bar */\
        switch(n){ case 3:_app_name="Terminal";break; case 4:_app_name="Settings";break; case 5:_app_name="OreoAI";break; case 6:_app_name="Calculator";break; case 7:_app_name="Notes";break; case 8:_app_name="Store";break; case 9:_app_name="Studio";break; case 10:_app_name="KairoVM";break; case 11:_app_name="Camera";break; case 12:_app_name="Player";break; case 14:_app_name="TrueVideo";break; case 16:_app_name="Calendar";break; case 17:_app_name="Pomodoro";break; case 18:_app_name="Weather";break; case 19:_app_name="Monitor";break; case 20:_app_name="Art";break; case 21:_app_name="Typing";break; case 22:_app_name="Clipboard";break; case 23:_app_name="Files";break; case 24:_app_name="Tetris";break; case 25:_app_name="Games";break; case 26:_app_name="Snake";break; case 28:_app_name="Wii";break; case 29:_app_name="Mic Test";break; case 30:_app_name="Pong";break; case 31:_app_name="Paint";break; case 32:_app_name="Maze";break;\
        }\
        /* Outer shadow (wide, soft) */\
        gfx_fill_round_rect(wx+10,wy+12,ww,wh,12,0x000008);\
        gfx_fill_round_rect(wx+8,wy+10,ww,wh,12,0x00000C);\
        gfx_fill_round_rect(wx+6,wy+8,ww,wh,12,0x000010);\
        /* Inner shadow (tight, dark) */\
        gfx_fill_round_rect(wx+4,wy+6,ww,wh,12,0x000018);\
        gfx_fill_round_rect(wx+2,wy+4,ww,wh,12,0x00001E);\
        /* Window body */\
        gfx_fill_round_rect(wx,wy,ww,wh,12,0x0E0E28);\
        gfx_round_rect(wx,wy,ww,wh,12,0x3A5ADF);\
        gfx_round_rect(wx+1,wy+1,ww-2,wh-2,11,0x2A3ABE);\
        gfx_fill_round_rect(wx+2,wy+2,ww-4,14,10,0x141430);\
        gfx_rect(wx+4,wy+14,ww-8,2,0x0E0E28);\
        /* Update menu bar app name */\
        int _dmc = set_state & 1;\
        gfx_rect(83,4,140,16,_dmc?0x06060E:0x0A0A1C);\
        gfx_rect(85,6,2,12,_dmc?0x1A1A3A:0x2A2A5A);\
        gfx_print(92,5,_dmc?0x8899CC:0xAABBEE,_app_name);\
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
        char _pb[80]; int _pi,_pn=0;\
        char _pfix[40]; int _pf=0;\
        {char _ws[]="user@viteza:~$ ";for(_pf=0;_ws[_pf];_pf++)_pfix[_pf]=_ws[_pf];_pfix[_pf]=0;}\
        for(_pi=0;_pfix[_pi];_pi++)_pb[_pn++]=_pfix[_pi];\
        for(_pi=0;_pi<term_pos&&_pi<44;_pi++)_pb[_pn++]=term_buf[_pi];\
        _pb[_pn]=0;\
        gfx_print(wx+16,wy+44+wh-56-16,_natal_mode?0xFF4444:0x00FF44,_pb);\
        int _ml = (wh-72)/16 - 1; if(_ml<1) _ml=1;\
        int _start = term_scroll - _ml + 1; if(_start<0) _start=0;\
        int _ty = wy+56;\
        for(int _i=_start; _i<=term_scroll && _i<term_line_count; _i++){\
            uint32_t _tc = _natal_mode?0xFF6644:0x00CC44;\
            if(term_lines[_i][0]=='['){_tc=_natal_mode?0x44FF44:0x00AA44;}if(term_lines[_i][0]=='#'){_tc=0xFF4444;}\
            if(term_lines[_i][0]=='%'){_tc=0xFFFF44;}\
            if(_natal_mode&&_i==term_line_count-1&&term_lines[_i][0]==0xE2){_tc=0x44FF44;}\
            gfx_print(wx+16,_ty,_tc,term_lines[_i]); _ty+=16;\
        }\
    } while(0)

    // Redraw chat window — bubble-style
    #define chat_redraw() do {\
        gfx_fill_round_rect(wx+8,wy+44,ww-16,wh-56,6,0x08081C);\
        gfx_round_rect(wx+8,wy+44,ww-16,wh-56,6,0x1A1A4E);\
        int _ml = (wh-72)/16 - 1; if(_ml<1) _ml=1;\
        int _start = chat_scroll - _ml + 1; if(_start<0) _start=0;\
        int _cy = wy+56;\
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
        gfx_print(wx+16,wy+44+wh-56-16,0xBB88FF,_cb);\
    } while(0)

#define calc_redraw() do {\
    int _cx=wx+16,_cy=wy+50,_cw=ww-32,_ch=wh-66;\
    gfx_fill_round_rect(_cx,_cy,_cw,_ch,6,0x06061A);\
    gfx_round_rect(_cx,_cy,_cw,_ch,6,0x2A4A7A);\
    int _ddy=_cy+8,_ddh=32;\
    gfx_fill_round_rect(_cx+8,_ddy,_cw-16,_ddh,4,0x0A0A1A);\
    gfx_round_rect(_cx+8,_ddy,_cw-16,_ddh,4,0x3A5A8A);\
    int _disp=(calc_cur!=0||!calc_state||!calc_op)?calc_cur:calc_val;\
    char _db[16];int _di=0,_dn=_disp<0?-_disp:_disp;\
    do{_db[_di++]='0'+_dn%10;_dn/=10;}while(_dn);\
    if(_disp<0)_db[_di++]='-';\
    _db[_di]=0;\
    for(int _rk=0;_rk<_di/2;_rk++){char _rt=_db[_rk];_db[_rk]=_db[_di-1-_rk];_db[_di-1-_rk]=_rt;}\
    gfx_print(_cx+_cw-16-_di*8,_ddy+10,0x44FF44,_db);\
    const char *_bl[16]={"7","8","9","/","4","5","6","*","1","2","3","-","C","0","=","+"};\
    int _bw=(_cw-32)/4,_bh=28,_by2=_ddy+_ddh+8;\
    for(int _bi=0;_bi<16;_bi++){\
        uint32_t _bcol=0x2A3A6A;\
        if(_bl[_bi][0]=='/'||_bl[_bi][0]=='*'||_bl[_bi][0]=='-')_bcol=0x3A6AAA;\
        if(_bl[_bi][0]=='+'||_bl[_bi][0]=='=')_bcol=0x4488FF;\
        if(_bl[_bi][0]=='C')_bcol=0xCC4444;\
        int _br=_bi/4,_bc=_bi%4;\
        int _bx=_cx+12+_bc*(_bw+4);\
        int _by3=_by2+_br*(_bh+4);\
        gfx_fill_round_rect(_bx+1,_by3+1,_bw,_bh,4,0x000000);\
        gfx_fill_round_rect(_bx,_by3,_bw,_bh,4,_bcol);\
        gfx_round_rect(_bx,_by3,_bw,_bh,4,0x4A6ADF);\
        gfx_print(_bx+_bw/2-4,_by3+8,0xFFFFFF,_bl[_bi]);\
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

    draw_cursor();
    // Snowflake cursor in Christmas mode
    if (_natal_mode) {
        int _cmx = mouse_get_x(), _cmy = mouse_get_y();
        gfx_putpixel(_cmx, _cmy-4, 0xFFFFFF);
        gfx_putpixel(_cmx, _cmy+4, 0xFFFFFF);
        gfx_putpixel(_cmx-4, _cmy, 0xFFFFFF);
        gfx_putpixel(_cmx+4, _cmy, 0xFFFFFF);
        gfx_putpixel(_cmx-3, _cmy-3, 0xFFFFFF);
        gfx_putpixel(_cmx+3, _cmy+3, 0xFFFFFF);
        gfx_putpixel(_cmx-3, _cmy+3, 0xFFFFFF);
        gfx_putpixel(_cmx+3, _cmy-3, 0xFFFFFF);
        gfx_putpixel(_cmx, _cmy, 0xFFFF44);
    }
    mouse_poll();

    // Debug: USB count + AC97 status
    gfx_print(8, 4, 0x00FF00, "U:");
    char _ud[8]; int _udi=7; _ud[_udi]=0; int _udn=usb_device_count(); do { _ud[--_udi]='0'+_udn%10; _udn/=10; } while(_udn); gfx_print(24, 4, 0x00FF00, _ud+_udi);
    extern int ac97_is_init(void);
    gfx_print(48, 4, ac97_is_init() ? 0x00FF00 : 0xFF4444, ac97_is_init() ? "A:OK" : "A:NO");

    char k;
    while (1) {
        mouse_poll();
        draw_cursor();
        if (_natal_mode) {
            int _cmx = mouse_get_x(), _cmy = mouse_get_y();
            gfx_putpixel(_cmx, _cmy-4, 0xFFFFFF);
            gfx_putpixel(_cmx, _cmy+4, 0xFFFFFF);
            gfx_putpixel(_cmx-4, _cmy, 0xFFFFFF);
            gfx_putpixel(_cmx+4, _cmy, 0xFFFFFF);
            gfx_putpixel(_cmx-3, _cmy-3, 0xFFFFFF);
            gfx_putpixel(_cmx+3, _cmy+3, 0xFFFFFF);
            gfx_putpixel(_cmx-3, _cmy+3, 0xFFFFFF);
            gfx_putpixel(_cmx+3, _cmy-3, 0xFFFFFF);
            gfx_putpixel(_cmx, _cmy, 0xFFFF44);
        }
        // Dock hover magnification + click
        static int _pdh = -2;
        int _mhx = mouse_get_x(), _mhy = mouse_get_y();
        int _ddcx = w/2 - 320, _ddcy = h - 70, _dhov = -1;
        int _dhbase = _ddcx + (640 - 9*56)/2;
        if (_mhy >= _ddcy - 5) {
            for (int _di = 0; _di < 9; _di++) {
                int _ix = _dhbase + _di*56;
                if (_mhx >= _ix && _mhx <= _ix + 40) { _dhov = _di; break; }
            }
        }
        if (_dhov >= 0) {
            int _hx = _dhbase + _dhov*56;
            uint32_t _mcols[] = {0xCC44AA,0x2A2A3A,0x4A4A5A,0x5A3A8A,0x3A3A4A,0xDDAA33,0x4488FF,0x6A8ABE,0x3A1A5A,0xFFFFFF};
            const char *_dinit[] = {"G","T","S","O","C","N","A","W","T","W"};
            const char *_dnm[] = {"Games","Terminal","Settings","OreoAI","Calculator","Notes","App Store","Weather","Tetris","Wii Menu"};
            int _msz = 54, _mx = _hx - 7, _my = _ddcy - 10;
            gfx_fill_round_rect(_mx+3, _my+3, _msz, _msz, 14, 0x000000);
            gfx_fill_round_rect(_mx, _my, _msz, _msz, 14, _mcols[_dhov]);
            gfx_round_rect(_mx, _my, _msz, _msz, 14, 0x88BBFF);
            gfx_print_scaled(_mx+12, _my+10, 0xFFFFFF, _dinit[_dhov], 3);
            const char *_dn = _dnm[_dhov]; int _dl = 0; while(_dn[_dl]) _dl++;
            int _dx = _hx + 20 - _dl*4;
            if (_dx < 4) _dx = 4; if (_dx + _dl*8 > w-4) _dx = w-4 - _dl*8;
            gfx_fill_round_rect(_dx-6, _my-24, _dl*8+12, 16, 4, 0x0A0A2A);
            gfx_round_rect(_dx-6, _my-24, _dl*8+12, 16, 4, 0x3A5A8A);
            gfx_print(_dx, _my-22, 0xFFFFFF, _dn);
        }
        // Capture click BEFORE hover redraw so we don't lose it
        if (mouse_clicked() && !win && !cc_active && !nc_active && _dhov >= 0) {
            const char *_dkmap = "k345678wgz";
            char _dc = _dkmap[_dhov];
            if (_dc) { k = _dc; goto _dock_go; }
        }
        if (_dhov != _pdh) { _pdh = _dhov; goto redraw_desktop; }
        k = keyboard_last_char();
_dock_go:

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
            gfx_clear(0x000008);
            // Draw some subtle grid lines for depth
            for (int _gi = 0; _gi < w; _gi += 40) gfx_rect(_gi, 0, 1, h, 0x080818);
            for (int _gj = 0; _gj < h; _gj += 40) gfx_rect(0, _gj, w, 1, 0x080818);
            // Move the DVD logo
            dv_x += dv_dx; dv_y += dv_dy;
            int dv_w = 160, dv_h = 40;
            if (dv_x <= 0 || dv_x + dv_w >= w) { dv_dx = -dv_dx; dv_c = (dv_c+1)%8; }
            if (dv_y <= 0 || dv_y + dv_h >= h) { dv_dy = -dv_dy; dv_c = (dv_c+1)%8; }
            // Clamp
            if (dv_x < 0) dv_x = 0; if (dv_x + dv_w > w) dv_x = w - dv_w;
            if (dv_y < 0) dv_y = 0; if (dv_y + dv_h > h) dv_y = h - dv_h;
            uint32_t dv_color = dv_colors[dv_c];
            // Draw logo with shadow
            gfx_fill_round_rect(dv_x+3, dv_y+3, dv_w, dv_h, 10, 0x000000);
            gfx_fill_round_rect(dv_x, dv_y, dv_w, dv_h, 10, dv_color);
            gfx_round_rect(dv_x, dv_y, dv_w, dv_h, 10, 0xFFFFFF);
            gfx_print_scaled(dv_x+20, dv_y+6, 0xFFFFFF, "DVD", 2);
            gfx_print(dv_x+10, dv_y+26, 0x000000, "KAIRO  OS");
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

        // ─── System Ctrl Shortcuts ───
        if (!cmd_active && !ai_active) {
            if (is_ctrl_pressed() && k == ' ') {
                ai_active = 1; ai_pos = 0; ai_buf[0] = 0; ai_response = 0;
                continue;
            }
            if (is_ctrl_pressed() && k == 'c') {
                if (win) { close_win(); }
                nc_active = 0; cc_active = !cc_active; cc_sel = 0;
                if (!cc_active) goto redraw_desktop;
                continue;
            }
            if (is_ctrl_pressed() && k == 'n') {
                if (win) { close_win(); }
                cc_active = 0; nc_active = !nc_active; nc_sel = 0;
                if (!nc_active) goto redraw_desktop;
                continue;
            }
            // Ctrl+Shift+P → Command Palette
            if (is_ctrl_pressed() && k == 'P') {
                cmd_active = 1; cmd_pos = 0; cmd_buf[0] = 0;
                if (win) { close_win(); }
                continue;
            }
        }

        // Refresh clock every ~50 keypresses
        _tick++; if ((_tick % 50) == 0) clock_refresh();

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
            if ((k == 27 || k == 'q' || (is_ctrl_pressed() && k == 'w')) && win_type != 3 && win_type != 5) { close_win(); search_focus = 1; goto redraw_desktop; }

            // Terminal input
            if (win_type == 3) {
                if (k == 27 || k == 'q' || (is_ctrl_pressed() && k == 'w')) { close_win(); search_focus = 1; goto redraw_desktop; }
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
                        } else if (term_buf[0]=='c'&&term_buf[1]=='l'&&term_buf[2]=='e'&&term_buf[3]=='a'&&term_buf[4]=='r'&&!term_buf[5]) {
                            term_line_count = 0; term_scroll = 0;
                        } else if (term_buf[0]=='w'&&term_buf[1]=='h'&&term_buf[2]=='o'&&term_buf[3]=='a'&&term_buf[4]=='m'&&term_buf[5]=='i'&&!term_buf[6]) {
                            term_add("user@viteza","");
                        } else if (term_buf[0]=='v'&&term_buf[1]=='e'&&term_buf[2]=='r'&&!term_buf[3]) {
                            term_add("Viteza Kernel v1.0 (x86_64)","");
                        } else if (term_buf[0]=='d'&&term_buf[1]=='a'&&term_buf[2]=='t'&&term_buf[3]=='e'&&!term_buf[4]) {
                            term_add("Sun Jun 28 2026","");
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
                        } else if (term_pos > 0) {
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
                if (k == 27 || k == 'q' || (is_ctrl_pressed() && k == 'w')) { close_win(); search_focus = 1; goto redraw_desktop; }
                if (k == KEY_UP && chat_scroll > 0) { chat_scroll--; chat_redraw(); continue; }
                if (k == KEY_DOWN && chat_scroll < chat_line_count-1) { chat_scroll++; chat_redraw(); continue; }
                if (k == '\n') {
                    chat_buf[chat_pos] = 0;
                    if (chat_pos > 0) {
                        chat_add(""); // blank to separate
                        char _tmp[80]; int _ti;
                        for(_ti=0;chat_buf[_ti];_ti++) _tmp[_ti]=chat_buf[_ti];
                        _tmp[_ti]=0;
                        chat_add(_tmp);
                        // Try AI via serial proxy
                        int _got = 0;
                        while (serial_available()) serial_read();
                        serial_puts("AI|");
                        serial_puts(_tmp);
                        serial_write('\n');
                        // Wait for response with timeout
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
                                    if (_rt[0]) { chat_add(_rt); _got = 1; }
                                }
                            }
                        }
                        if (!_got) {
                            chat_add("[AI proxy not connected. Run: python3 social_proxy.py]");
                        }
                    }
                    chat_pos = 0; chat_buf[0] = 0;
                    chat_redraw();
                    continue;
                }
                if (k == '\b' && chat_pos > 0) { chat_pos--; chat_buf[chat_pos] = 0; chat_redraw(); continue; }
                if (chat_pos < 127 && k >= ' ' && k <= '~') { chat_buf[chat_pos++] = k; chat_buf[chat_pos] = 0; chat_redraw(); continue; }
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
                if (k == '\n') {
                    note_buf[note_pos] = 0;
                    if (note_pos > 0 && note_count < 10) {
                        int _ni;
                        for(_ni=0;note_buf[_ni]&&_ni<79;_ni++) notes[note_count][_ni]=note_buf[_ni];
                        notes[note_count][_ni]=0; note_count++;
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
                if (note_pos < 63 && k >= ' ' && k <= '~') { note_buf[note_pos++] = k; note_buf[note_pos] = 0;
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
                if (is_ctrl_pressed() && k == 'r') {
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
                    // Simulated webcam — 3D city skyline
                    #define _Wsin(_a) _ws[((_a)%256+256)%256]
                    static const int _ws[256] = {0,6,12,18,25,31,37,43,49,56,62,68,74,80,86,92,97,103,109,115,120,126,131,136,142,147,152,157,162,167,171,176,181,185,189,193,197,201,205,209,212,216,219,222,225,228,231,234,236,238,241,243,244,246,248,249,251,252,253,254,254,255,255,255,256,255,255,255,254,254,253,252,251,249,248,246,244,243,241,238,236,234,231,228,225,222,219,216,212,209,205,201,197,193,189,185,181,176,171,167,162,157,152,147,142,136,131,126,120,115,109,103,97,92,86,80,74,68,62,56,49,43,37,31,25,18,12,6,0,-6,-12,-18,-25,-31,-37,-43,-49,-56,-62,-68,-74,-80,-86,-92,-97,-103,-109,-115,-120,-126,-131,-136,-142,-147,-152,-157,-162,-167,-171,-176,-181,-185,-189,-193,-197,-201,-205,-209,-212,-216,-219,-222,-225,-228,-231,-234,-236,-238,-241,-243,-244,-246,-248,-249,-251,-252,-253,-254,-254,-255,-255,-255,-256,-255,-255,-255,-254,-254,-253,-252,-251,-249,-248,-246,-244,-243,-241,-238,-236,-234,-231,-228,-225,-222,-219,-216,-212,-209,-205,-201,-197,-193,-189,-185,-181,-176,-171,-167,-162,-157,-152,-147,-142,-136,-131,-126,-120,-115,-109,-103,-97,-92,-86,-80,-74,-68,-62,-56,-49,-43,-37,-31,-25,-18,-12,-6};
                    int _ca = _tick, _cx0 = _cfx+_cfw/2, _cy0 = _cfy+_cfh/2;
                    int _sa = _Wsin(_ca), _caa = _Wsin(_ca+64);
                    gfx_fill_round_rect(_cfx,_cfy,_cfw,_cfh,6,0x080818);
                    // Ground plane (dark grid)
                    for (int _gz = 50; _gz < 500; _gz += 30) {
                        int _zx = (-_gz*_sa)>>8, _zz = (_gz*_caa+400)>>8;
                        int _d2 = 500, _s2 = _d2*256/(_zz+_d2);
                        int _gpx = _cx0 + (_zx*_s2>>8);
                        int _gpy = _cy0 + 80 + (200*_s2>>8)/2;
                        if (_gpx >= _cfx && _gpx < _cfx+_cfw && _gpy >= _cfy && _gpy < _cfy+_cfh)
                            gfx_rect(_gpx-1,_gpy,3,1,0x1A2A4A);
                    }
                    // Buildings: pos_x, pos_z, width, depth, height, color
                    int _nb = 12, _bd[72];
                    _bd[0]=-120;_bd[1]=80;_bd[2]=40;_bd[3]=40;_bd[4]=120;_bd[5]=0x4488FF;
                    _bd[6]=-60;_bd[7]=60;_bd[8]=30;_bd[9]=30;_bd[10]=200;_bd[11]=0xFF6644;
                    _bd[12]=0;_bd[13]=70;_bd[14]=50;_bd[15]=50;_bd[16]=90;_bd[17]=0x44DD88;
                    _bd[18]=70;_bd[19]=90;_bd[20]=35;_bd[21]=35;_bd[22]=160;_bd[23]=0xCC44AA;
                    _bd[24]=-80;_bd[25]=180;_bd[26]=45;_bd[27]=45;_bd[28]=80;_bd[29]=0x66AAFF;
                    _bd[30]=-20;_bd[31]=160;_bd[32]=25;_bd[33]=25;_bd[34]=250;_bd[35]=0xFFAA44;
                    _bd[36]=40;_bd[37]=140;_bd[38]=40;_bd[39]=40;_bd[40]=110;_bd[41]=0x44FFAA;
                    _bd[42]=100;_bd[43]=170;_bd[44]=30;_bd[45]=30;_bd[46]=140;_bd[47]=0xDD66BB;
                    _bd[48]=-100;_bd[49]=280;_bd[50]=50;_bd[51]=50;_bd[52]=60;_bd[53]=0x4488CC;
                    _bd[54]=30;_bd[55]=260;_bd[56]=35;_bd[57]=35;_bd[58]=180;_bd[59]=0xFF8844;
                    _bd[60]=90;_bd[61]=300;_bd[62]=45;_bd[63]=45;_bd[64]=100;_bd[65]=0x44DDFF;
                    _bd[66]=150;_bd[67]=250;_bd[68]=40;_bd[69]=40;_bd[70]=130;_bd[71]=0x8844CC;
                    for (int _bi = 0; _bi < _nb; _bi++) {
                        int _bi6 = _bi*6;
                        int _bx = _bd[_bi6], _bz = _bd[_bi6+1], _bw = _bd[_bi6+2], _bdp = _bd[_bi6+3], _bh = _bd[_bi6+4];
                        uint32_t _bc = _bd[_bi6+5];
                        // 8 vertices of building box
                        int _bv[8][3];
                        _bv[0][0]=_bx-_bw/2;_bv[0][1]=0;_bv[0][2]=_bz-_bdp/2;
                        _bv[1][0]=_bx+_bw/2;_bv[1][1]=0;_bv[1][2]=_bz-_bdp/2;
                        _bv[2][0]=_bx+_bw/2;_bv[2][1]=_bh;_bv[2][2]=_bz-_bdp/2;
                        _bv[3][0]=_bx-_bw/2;_bv[3][1]=_bh;_bv[3][2]=_bz-_bdp/2;
                        _bv[4][0]=_bx-_bw/2;_bv[4][1]=0;_bv[4][2]=_bz+_bdp/2;
                        _bv[5][0]=_bx+_bw/2;_bv[5][1]=0;_bv[5][2]=_bz+_bdp/2;
                        _bv[6][0]=_bx+_bw/2;_bv[6][1]=_bh;_bv[6][2]=_bz+_bdp/2;
                        _bv[7][0]=_bx-_bw/2;_bv[7][1]=_bh;_bv[7][2]=_bz+_bdp/2;
                        int _bpx[8], _bpy[8];
                        for (int _vi = 0; _vi < 8; _vi++) {
                            int _x = _bv[_vi][0], _y = _bv[_vi][1], _z = _bv[_vi][2];
                            int _x1 = (_x*_caa - _z*_sa)>>8;
                            int _z1 = (_x*_sa + _z*_caa)>>8;
                            int _d3 = 500, _s3 = _d3*256/(_z1+_d3);
                            _bpx[_vi] = _cx0 + (_x1*_s3>>8);
                            _bpy[_vi] = _cy0 + (100*_s3>>8)/2 - (_y*_s3>>8) + 100;
                        }
                        // Draw 4 vertical edges + 8 horizontal edges
                        int _be[24] = {0,1,1,2,2,3,3,0,4,5,5,6,6,7,7,4,0,4,1,5,2,6,3,7};
                        for (int _ei = 0; _ei < 12; _ei++) {
                            int _a = _be[_ei*2], _b = _be[_ei*2+1];
                            int _x1 = _bpx[_a], _y1 = _bpy[_a], _x2 = _bpx[_b], _y2 = _bpy[_b];
                            // Skip if behind camera
                            if (_x1 < _cfx-10 && _x2 < _cfx-10) continue;
                            if (_x1 > _cfx+_cfw+10 && _x2 > _cfx+_cfw+10) continue;
                            uint32_t _ec = _bc;
                            if (_ei >= 8) _ec = ((_bc&0xFEFEFE)>>1)+0x222222; // top edges darker
                            int _dx = _x2-_x1, _dy = _y2-_y1;
                            int _adx = _dx<0?-_dx:_dx, _ady = _dy<0?-_dy:_dy;
                            int _err = 0, _lx = _x1, _ly = _y1;
                            if (_adx > _ady) {
                                int _sy = _dy < 0 ? -1 : 1;
                                _err = -_adx;
                                for (int _li = 0; _li <= _adx; _li++) {
                                    if (_lx >= _cfx && _lx < _cfx+_cfw && _ly >= _cfy && _ly < _cfy+_cfh) gfx_putpixel(_lx,_ly,_ec);
                                    _err += _ady*2;
                                    if (_err > 0) { _ly += _sy; _err -= _adx*2; }
                                    _lx += (_dx<0 ? -1 : 1);
                                }
                            } else {
                                int _sx = _dx < 0 ? -1 : 1;
                                _err = -_ady;
                                for (int _li = 0; _li <= _ady; _li++) {
                                    if (_lx >= _cfx && _lx < _cfx+_cfw && _ly >= _cfy && _ly < _cfy+_cfh) gfx_putpixel(_lx,_ly,_ec);
                                    _err += _adx*2;
                                    if (_err > 0) { _lx += _sx; _err -= _ady*2; }
                                    _ly += (_dy<0 ? -1 : 1);
                                }
                            }
                        }
                    }
                    // Scanline overlay + watermark
                    int _ct = _tick;
                    for (int _y = _ct%6; _y < _cfh; _y += 6) {
                        gfx_rect(_cfx, _cfy+_y, _cfw, 1, 0x00000033);
                    }
                    // Neon glow border
                    for (int _gn = 0; _gn < 3; _gn++) {
                        uint32_t _gc = _gn==0?0xFF00AA:_gn==1?0x00DDFF:0x44FF44;
                        gfx_rect(_cfx,_cfy+_cfh-1-_gn,_cfw,1,_gc);
                    }
                    gfx_print_scaled(_cfx+_cfw/2-80, _cfy+_cfh/2-12, 0xFFFFFF66, "CITY CAM", 2);
                    gfx_print(_cfx+8, _cfy+_cfh-20, 0xFF00AA88, "VITEZA CITY 3D");
                }

                gfx_print(wx+16, wy+wh-24, 0x3A4A6A, "[Esc] close");
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // Settings interactive toggles
            if (win_type == 4) {
                if (k == KEY_UP && set_cat > 0) { set_cat--; settings_redraw(); continue; }
                if (k == KEY_DOWN && set_cat < 5) { set_cat++; settings_redraw(); continue; }
                if (k == '1') { set_state ^= 1; settings_redraw(); continue; }
                if (k == '2') { set_state ^= 2; settings_redraw(); continue; }
                if (k == '3') { set_state ^= 4; settings_redraw(); continue; }
                if (k == '4') { set_state ^= 8; settings_redraw(); continue; }
                if (k == '5') { set_state ^= 16; settings_redraw(); continue; }
                if (k == '6') { set_state ^= 32; settings_redraw(); continue; }
                if (k == '7') { set_state ^= 64; settings_redraw(); continue; }
                if (k == '8') { set_state ^= 128; settings_redraw(); continue; }
                if (k == ' ') { play_sweep(400,800,300); continue; }
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

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
                            gfx_print(_ix,_iy,_dmc?0x4A5A7E:0x8899CC,_ds);
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
                if (k == KEY_DOWN && fm_sel < 7) { fm_sel++; if (fm_sel > fm_scroll+5) fm_scroll = fm_sel-5; }
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
                    for(int _fi=0;_fi<8;_fi++){
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
                        if(!fm_is_dir[_fi])gfx_print(wx+ww-60,_fy+2,_dmc?0x2A3A5A:0x4A6A8A,"1.2 KB");
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
                gfx_print_scaled(wx+ww/2-60,wy+60,0x4488FF,"Mic Test",2);
                int _lev = ac97_capture_is_active() ? ac97_capture_level() : 0;
                // Simulate level when no real capture (demo mode)
                if (!ac97_capture_is_active()) {
                    _lev = ((_tick * 37 + (_tick/3)*19) % 30000);
                    if (_lev > 25000) _lev = 30000 - (_lev-25000);
                    if (_lev < 500) _lev = 500;
                    gfx_print(wx+ww/2-80,_mby+50,0xFFAA44,"Demo mode (no capture)");
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
                    const char *_wn[] = {"Snake","Pong⚡","Tetris","Maze 🗺️","Paint🎨","Camera3D","Terminal","Calc","Mic Test","Pomodoro","ASCII Art","Player"};
                    const char *_wi[] = {"S","P","T","M","P","C","_","+","M","T","A","V"};
                    uint32_t _wcol[] = {0x44CCAA,0xFF6644,0x44DDFF,0x44AA66,0xFF8844,0x66DDFF,0x00CC44,0x4A9EFF,0xBB88FF,0xFF8800,0xFFAA44,0xDD44AA};
                    int _wa[] = {26,30,24,32,31,11,3,6,29,17,20,12};
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

            if (!k) { asm volatile("hlt"); continue; }
            continue; // other windows: just wait for Esc
        }

        // ─── NO WINDOW OPEN ───

        // Mouse click on dock icons
        if (mouse_clicked()) {
            int _dc_x = w/2 - 320, _dc_y = h - 64;
            int _di_y = _dc_y + 8, _di_sp = 56, _di_sz = 40;
            int _di_base = _dc_x + (640 - 9*_di_sp)/2;
            int _mx = mouse_get_x(), _my = mouse_get_y();
            if (_my >= _di_y && _my < _di_y + _di_sz) {
                int _di = (_mx - _di_base) / _di_sp;
                if (_di >= 0 && _di < 10 && _mx >= _di_base + _di*_di_sp && _mx < _di_base + _di*_di_sp + _di_sz) {
                    int acts[] = {25, 3, 6, 32, 30, 31, 26, 24, 29, 28};
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

        // Keyboard cursor movement (arrow keys)
        if (k == 0x01 || k == 0x02 || k == 0x03 || k == 0x04 || k == ' ') {
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

        // App shortcuts (1-6) — improved content
        if (k == '1' && !win) { open_app(1); draw_mac_title("This PC");
            gfx_fill_round_rect(wx+16,wy+50,ww-32,20,4,0x12124A);gfx_print(wx+24,wy+53,0x4A9EFF,"System Information");
            gfx_print(wx+24,wy+84,0x8A9ACE,"Kernel:    Viteza v1.0");
            gfx_print(wx+24,wy+102,0x8A9ACE,"CPU:       x86_64 Long Mode");
            gfx_print(wx+24,wy+120,0x8A9ACE,"RAM:       256 MB");
            char _dstr[32];_dstr[0]=0;
            _dstr[0]='0'+w/100%10;_dstr[1]='0'+w/10%10;_dstr[2]='0'+w%10;
            _dstr[3]='x';_dstr[4]='0'+h/100%10;_dstr[5]='0'+h/10%10;_dstr[6]='0'+h%10;_dstr[7]=0;
            gfx_print(wx+24,wy+138,0x8A9ACE,"Display:   ");gfx_print(wx+100,wy+138,0x00E5FF,_dstr);
            gfx_fill_round_rect(wx+16,wy+162,ww-32,20,4,0x12124A);gfx_print(wx+24,wy+165,0x4A9EFF,"Storage");
            gfx_print(wx+24,wy+192,0x6A7A9E,"No drives detected");
            gfx_fill_round_rect(wx+16,wy+210,ww-32,20,4,0x12124A);gfx_print(wx+24,wy+213,0x4A9EFF,"USB Devices");
            int _udi = usb_device_count();
            if (_udi > 0) {
                for (int _uj = 0; _uj < _udi && _uj < 3; _uj++) {
                    gfx_print(wx+24, wy+236+_uj*16, 0x6A8ABE, usb_device_name(_uj));
                }
            } else {
                gfx_print(wx+24, wy+236, 0x6A7A9E, "No USB devices connected");
            }
            gfx_print(wx+24,wy+280,0x3A4A6A,"[Esc] to close"); continue;
        }
        if (k == '2' && !win) { open_app(2); draw_mac_title("Network");
            gfx_fill_round_rect(wx+16,wy+50,ww-32,20,4,0x12124A);gfx_print(wx+24,wy+53,0x4A9EFF,"Wi-Fi Status");
            gfx_print(wx+24,wy+84,0x8A9ACE,"SSID:     HOME-5G");
            gfx_print(wx+24,wy+102,0x8A9ACE,"Signal:   Excellent");gfx_fill_round_rect(wx+130,wy+101,40,6,3,0x0A3A0A);gfx_fill_round_rect(wx+130,wy+101,36,6,3,0x00CC44);
            gfx_print(wx+24,wy+120,0x8A9ACE,"Security: WPA2-PSK");
            gfx_print(wx+130,wy+119,0x00CC44,"● Protected");
            gfx_fill_round_rect(wx+16,wy+144,ww-32,20,4,0x12124A);gfx_print(wx+24,wy+147,0x4A9EFF,"IP Configuration");
            gfx_print(wx+24,wy+174,0x6A7A9E,"DHCP: Disabled (static kernel)");
            gfx_print(wx+24,wy+192,0x6A7A9E,"IP:   0.0.0.0 (pending)");
            gfx_print(wx+24,wy+220,0x3A4A6A,"[Esc] to close"); continue;
        }
        if (k == '3' && !win) { open_app(3); draw_mac_title("Terminal");
            term_line_count=0;term_scroll=0;
            term_add("[Viteza Terminal v1.0]",0);
            term_add("Type 'help' for commands.",0);
            term_redraw(); continue;
        }
        if (k == '4' && !win) { open_app(4); draw_mac_title("System Settings");
            settings_redraw(); continue;
        }
        if (k == '5' && !win) { open_app(5); draw_mac_title("OreoAI Assistant");
            chat_line_count=0;chat_scroll=0;chat_pos=0;
            chat_add("[OreoAI v1.0 — Ask me anything!]");
            chat_add("Try: hello, who are you, help");
            chat_redraw(); continue;
        }
        if (k == '6' && !win) { open_app(6); draw_mac_title("Calculator");
            calc_val=0;calc_cur=0;calc_op=0;calc_state=0;
            calc_redraw(); continue;
        }
        if (k == '7' && !win) { open_app(7); draw_mac_title("Notes");
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
            continue;
        }
        if (k == '8' && !win) { open_app(8); draw_mac_title("App Store");
            gfx_fill_round_rect(wx+16,wy+50,ww-32,20,4,0x12124A);gfx_print(wx+24,wy+53,0x4A9EFF,"Available Apps");
            int _asi,_asy;
            for(_asi=0;_asi<APP_COUNT&&_asi<10;_asi++){
                _asy=wy+78+_asi*20;
                gfx_fill_round_rect(wx+16,_asy-2,ww-32,18,3,app_colors[_asi]);gfx_rect(wx+16,_asy-2,4,18,app_colors[_asi]);
                gfx_print(wx+28,_asy+1,0xFFFFFF,app_names[_asi]);
                gfx_print(wx+180,_asy+1,0x8080AA,app_cats[_asi]);
                if(apps_installed[_asi]){gfx_print(wx+300,_asy+1,0x44FF44,"[Installed]");}
                else{gfx_print(wx+300,_asy+1,0x808080,"[ press ");char _ak[2];_ak[0]='0'+_asi%10;_ak[1]=0;gfx_print(wx+364,_asy+1,0xFFAA00,_ak);gfx_print(wx+376,_asy+1,0x808080," ]");}
            }
            gfx_print(wx+16,wy+240,0x3A4A6A,"[0-9] install/uninstall  [Esc] back");
            continue;
        }
        if (k == '9' && !win) { open_app(9); draw_mac_title("Kairo Studio");
            studio_init_data(); studio_draw_all(); continue;
        }
        if (k == '0' && !win) { open_app(10); draw_mac_title("KairoVM");
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
            continue;
        }

        // Launchpad (L key)
        if (k == 'l' && !win && (!search_focus || search_pos == 0)) {
            win=1;win_type=15;lp_sel_x=0;lp_sel_y=0;
            draw_launch_pad(w, h, lp_sel_x, lp_sel_y);
            continue;
        }

        // System Transfer (T key)
        if (k == 't' && !win && (!search_focus || search_pos == 0)) {
            st_mode = ST_MENU; st_sel = 0;
            win=1; win_type=27;
            draw_system_transfer(w, h, &(int){0});
            continue;
        }

        // Control Center (Ctrl+C)
        if (is_ctrl_pressed() && k == 'c' && !win) {
            nc_active=0; cc_active=!cc_active; cc_sel=0;
            if(!cc_active){goto redraw_desktop;}
            continue;
        }

        // Notification Center (Ctrl+N)
        if (is_ctrl_pressed() && k == 'n' && !win) {
            cc_active=0; nc_active=!nc_active; nc_sel=0;
            if(!nc_active){goto redraw_desktop;}
            continue;
        }

        // Camera app (C key)
        if (k == 'c' && !win && (!search_focus || search_pos == 0)) {
            open_app(11); draw_mac_title("Camera");
            gfx_print(wx+16, wy+44, 0x4A9EFF, "Camera — Hardware Status");
            continue;
        }

        // Kairo Player (V key)
        if (k == 'v' && !win && (!search_focus || search_pos == 0)) {
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
        if (k == 't' && !win && (!search_focus || search_pos == 0)) {
            open_app(14);
            continue;
        }

        // New App Shortcuts (single letters when no window)
        if (k == 'e' && !win && (!search_focus || search_pos == 0)) { // Calendar
            open_app(16); draw_mac_title("Calendar");
            // Will redraw on next tick
            continue;
        }
        if (k == 'r' && !win && (!search_focus || search_pos == 0)) { // Pomodoro
            open_app(17); draw_mac_title("Pomodoro Timer");
            pom_sec = pom_total; pom_running = 0; pom_ticks = 0;
            continue;
        }
        if (k == 'w' && !win && (!search_focus || search_pos == 0)) { // Weather
            open_app(18); draw_mac_title("Weather"); wthr_first = 1;
            continue;
        }
        if (k == 'i' && !win && (!search_focus || search_pos == 0)) { // Disk Usage
            open_app(19); draw_mac_title("System Monitor");
            continue;
        }
        if (k == 'a' && !win && (!search_focus || search_pos == 0)) { // ASCII Art
            open_app(20); draw_mac_title("ASCII Art Gallery");
            art_sel = 0;
            continue;
        }
        if (k == 'y' && !win && (!search_focus || search_pos == 0)) { // Typing Test
            open_app(21); draw_mac_title("Typing Test");
            type_pos = 0; type_err = 0; type_ok = 0; type_start = 0; type_done = 0;
            continue;
        }
        if (k == 'o' && !win && (!search_focus || search_pos == 0)) { // Clipboard
            open_app(22); draw_mac_title("Clipboard");
            clip_save_pos = 0; clip_save[0] = 0;
            continue;
        }
        if (k == 'f' && !win && (!search_focus || search_pos == 0)) { // File Manager
            open_app(23); draw_mac_title("File Manager");
            fm_sel = 0; fm_scroll = 0;
            continue;
        }
        if (k == 'g' && !win && (!search_focus || search_pos == 0)) { // Tetris
            open_app(24); draw_mac_title("Tetris");
            continue;
        }
        if (k == 'k' && !win && (!search_focus || search_pos == 0)) { // Kairo Games
            open_app(25); draw_mac_title("Kairo Games");
            continue;
        }
        if (k == 'z' && !win && (!search_focus || search_pos == 0)) { // Viteza Wii
            open_app(28); _wii_boot = 1;
            continue;
        }
        if (k == 'p' && !win && (!search_focus || search_pos == 0)) { // Pong
            open_app(30); draw_mac_title("Pong ⚡");
            continue;
        }
        if (k == 'b' && !win && (!search_focus || search_pos == 0)) { // Paint
            open_app(31); draw_mac_title("Paint 🎨");
            continue;
        }
        if (k == 'x' && !win && (!search_focus || search_pos == 0)) { // Maze
            open_app(32); draw_mac_title("Maze 🗺️");
            continue;
        }
        if (k == 'm' && !win && (!search_focus || search_pos == 0)) { // Mic Test
            ac97_start_capture();
            open_app(29);
            continue;
        }

        // Note widget navigation (when no window, search unfocused)
        if (!win && !search_focus) {
            if (k == ',' && note_count > 0) { note_sel--; if (note_sel < 0) note_sel = note_count-1; note_widget_draw(); continue; }
            if (k == '.' && note_count > 0) { note_sel++; if (note_sel >= note_count) note_sel = 0; note_widget_draw(); continue; }
        }

        // USB device simulation (press U) — works even when search is empty/focused
        if (k == 'u' && !win && (!search_focus || search_pos == 0)) {
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
                    } else if (act == 4) { draw_mac_title("System Settings");
                        set_cat = 0; settings_redraw();
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
                    } else if (act == 14) { open_app(14); }
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
