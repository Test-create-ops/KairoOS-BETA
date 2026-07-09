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
    while (cmos_read(0x0A) & 0x80); // wait for RTC update
    int regb = cmos_read(0x0B);
    int bin = regb & 4;  // bit 2 = binary mode
    int vh = cmos_read(0x04), vm = cmos_read(0x02);
    if (!(regb & 2)) {
        // 12-hour mode: convert to 24-hour
        int pm = vh & 0x80;
        vh &= 0x7F;
        if (pm) { vh = (vh % 12) + 12; }
        else if (vh == 12) { vh = 0; } // 12 AM = 0
    }
    if (bin) { *h = vh; *m = vm; }
    else { *h = ((vh>>4)*10)+(vh&0x0F); *m = ((vm>>4)*10)+(vm&0x0F); }
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
// Menu bar icon positions
int cc_icon_x = 0, nc_icon_x = 0;
// Launchpad
int lp_sel_x = 0, lp_sel_y = 0;
// Tetris
int tet_grid[10][18]; int tet_piece = 0, tet_rot = 0, tet_x = 3, tet_y = 0, tet_next = 0, tet_score = 0, tet_lines = 0, tet_drop = 0, tet_gameover = 0;

// Snake
int snake_body[400][2]; int snake_len = 3, snake_dir = 0, snake_food_x = 8, snake_food_y = 8, snake_score = 0, snake_gameover = 0, snake_drop = 0;
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
    // Deep macOS Big Sur-style gradient (dark blue → deep purple)
    for (int y = 0; y < h; y++) {
        int t = y * 255 / h;
        int r, g, b;
        if (dm) {
            r = 2 + t/60; g = 1 + t/40; b = 8 + t/30;
            if (y < 180) { int f = 180-y; r = 4+f/40; g = 2+f/60; b = 12+f/15; }
            if (r > 8) r = 8; if (g > 6) g = 6; if (b > 20) b = 20;
        } else {
            r = 8 + t/30; g = 6 + t/20; b = 24 + t/10;
            if (y < 180) { int f = 180-y; r = 18+f/12; g = 10+f/20; b = 50+f/6; }
            if (r > 32) r = 32; if (g > 28) g = 28; if (b > 60) b = 60;
        }
        gfx_rect(0, y, w, 1, (r<<16)|(g<<8)|b);
    }
    // Soft radial glow from top-center
    if (!dm) for (int i = 0; i < 80; i++) {
        int a = (80-i)*3; if (a > 60) a = 60;
        gfx_rect(w/2 - i*5, h/5 - i/4, i*10, 2, (a*3/4<<16)|(a/2<<8)|a);
        gfx_rect(w/2 - i*5, h/5 + i/4, i*10, 2, (a*3/4<<16)|(a/2<<8)|a);
    }
    // Soft star glow dots (rounded, no pixels)
    for (int i = 0; i < (dm ? 20 : 50); i++) {
        int sx = (i*691+47)%w, sy = (i*983+19)%(h*3/5);
        int sb = (i*257+13)%6;
        if (sb < 2) continue;
        uint32_t sc = sb > 4 ? (dm ? 0x335577 : 0xAAC0EE) : (sb > 2 ? (dm ? 0x224466 : 0x8899CC) : (dm ? 0x1A3355 : 0x6677AA));
        gfx_fill_round_rect(sx-1, sy-1, 3, 3, 1, sc);
    }

    // ─── Top menu bar (macOS style) ───
    int mby = 0, mbh = 24;
    dm = set_state & 1;
    gfx_rect(0, mby, w, mbh, dm ? 0x06060E : 0x0A0A1C);
    gfx_rect(0, mby+mbh-1, w, 1, dm ? 0x101020 : 0x1A1A3A);
    gfx_print(10, 5, dm ? 0x556688 : 0x8899CC, "Viteza");
    int _hr = 12, _mn = 0; rtc_read(&_hr, &_mn);
    char _time[6]; _time[0]='0'+_hr/10; _time[1]='0'+_hr%10; _time[2]=':';
    _time[3]='0'+_mn/10; _time[4]='0'+_mn%10; _time[5]=0;
    // Right-side items in menu bar
    int mb_rx = w-10, _clk_x;
    // Battery
    gfx_round_rect(mb_rx-24, 6, 18, 10, 2, 0x5566AA);
    gfx_rect(mb_rx-6, 8, 3, 6, 0x5566AA);
    gfx_fill_round_rect(mb_rx-22, 8, 10, 6, 2, 0x44AA66);
    mb_rx -= 34;
    // Clock
    _clk_x = mb_rx-50;
    gfx_print(_clk_x, 5, TEXT, _time);
    mb_rx -= 56;
    // Notification Center icon (bell)
    nc_icon_x = mb_rx - 16;
    gfx_round_rect(nc_icon_x, 5, 12, 12, 2, 0x5566AA);
    gfx_fill_round_rect(nc_icon_x+4, 4, 4, 2, 1, 0x5566AA);
    gfx_fill_round_rect(nc_icon_x+3, 9, 6, 2, 1, 0x5566AA);
    gfx_rect(nc_icon_x+4, 10, 4, 2, 0x5566AA);
    mb_rx -= 20;
    // Control Center icon (3 lines)
    cc_icon_x = mb_rx - 14;
    gfx_rect(cc_icon_x, 7, 10, 2, 0x5566AA);
    gfx_rect(cc_icon_x, 11, 10, 2, 0x5566AA);
    gfx_rect(cc_icon_x, 15, 10, 2, 0x5566AA);
    mb_rx -= 18;
    // Wi-Fi icon
    gfx_fill_round_rect(mb_rx-16, 8, 10, 2, 1, 0x5566AA);
    gfx_putpixel(mb_rx-11, 7, 0x5566AA);
    gfx_putpixel(mb_rx-11, 15, 0x5566AA);
    // Separator
    mb_rx -= 14;
    gfx_rect(mb_rx-10, 5, 1, 14, 0x1A1A3A);

    // ─── Dock (macOS style) ───
    int dc_w = 640, dc_h = 56, dc_r = 28;
    int dc_x = w/2 - dc_w/2, dc_y = h - dc_h - 8;
    // Dock shadow
    gfx_fill_round_rect(dc_x+3, dc_y+3, dc_w, dc_h, dc_r, 0x000000);
    // Dock background (glass-like)
    gfx_fill_round_rect(dc_x, dc_y, dc_w, dc_h, dc_r, dm ? 0x060612 : 0x0C0C24);
    gfx_round_rect(dc_x, dc_y, dc_w, dc_h, dc_r, dm ? 0x18183A : 0x2A2A5A);
    gfx_round_rect(dc_x+1, dc_y+1, dc_w-2, dc_h-2, dc_r-1, dm ? 0x0E0E2A : 0x1A1A4A);
    // Glass highlight line
    gfx_rect(dc_x+20, dc_y+3, dc_w-40, 1, dm ? 0x1A2A4A : 0x3A4A7A);
    gfx_rect(dc_x+40, dc_y+4, dc_w-80, 1, dm ? 0x121A3A : 0x2A3A6A);
    // Subtle dock glow beneath
    gfx_rect(dc_x+20, dc_y+dc_h, dc_w-40, 1, dm ? 0x0E1A3A : 0x1A2A5A);
    gfx_rect(dc_x+40, dc_y+dc_h+1, dc_w-80, 1, dm ? 0x060E2A : 0x0A1A4A);

    // ─── Dock Icons — macOS quality, no pixels ───
    int di_y = dc_y + 8, di_sz = 40, di_sp = 56;
    int di_base = dc_x + (dc_w - 9*di_sp)/2;
    int di_x, di_cy;

    #define di_sh(_i,_c) do {\
        di_x = di_base + (_i)*di_sp; di_cy = di_y;\
        gfx_fill_round_rect(di_x+1,di_cy+1,di_sz,di_sz,9,0x000000);\
        gfx_fill_round_rect(di_x,di_cy,di_sz,di_sz,10,_c);\
        gfx_round_rect(di_x,di_cy,di_sz,di_sz,10,0x4A6AAF);\
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

    // App indicator dots under active icons
    for (int _di = 0; _di < 9; _di++) {
        gfx_fill_round_rect(di_base+_di*di_sp+14, dc_y+dc_h-6, 12, 3, 2, 0x3A4A7A);
    }

    // Separator + Trash on right side
    int dc_tr = dc_x+dc_w-40;
    gfx_rect(dc_tr-8, dc_y+10, 1, dc_h-20, 0x2A2A5A);
    gfx_rect(dc_tr-6, dc_y+18, 14, 16, 0x3A4A6A);
    gfx_rect(dc_tr-2, dc_y+14, 6, 4, 0x3A4A6A);

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
        /* Refresh desktop digital clock widget */\
        if(!win){\
            char _wd[9];_wd[0]='0'+_hr/10;_wd[1]='0'+_hr%10;_wd[2]=':';\
            _wd[3]='0'+_mn/10;_wd[4]='0'+_mn%10;_wd[5]=0;\
            int _dmc = set_state & 1;\
            gfx_fill_round_rect(dc_x2,dc_y2,140,60,8,_dmc?0x040410:0x080820);\
            gfx_print_scaled(dc_x2+16,dc_y2+8,_dmc?0x3366CC:0x4488FF,_wd,2);\
            gfx_print(dc_x2+12,dc_y2+44,_dmc?0x1A2A4A:0x3A4A6A,"Viteza OS");\
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
        gfx_fill_round_rect(wx+4,wy+4,ww-8,30,8,_dmc?0x141430:0x1E1E3E);\
        gfx_fill_round_rect(wx+4,wy+4,ww-8,20,8,_dmc?0x1A1A38:0x282852);\
        gfx_rect(wx+8,wy+22,ww-16,12,_dmc?0x141430:0x1E1E3E);\
        gfx_rect(wx+4,wy+35,ww-8,1,_dmc?0x24244A:0x3A3A6A);\
        gfx_print(wx+ww/2-28,wy+7,_dmc?0x667799:0x8899CC,k_);\
        /* Close — red with shine */\
        gfx_fill_round_rect(wx+10,wy+8,12,12,6,0xDD4A4A);\
        gfx_fill_round_rect(wx+12,wy+9,4,3,2,0xFF7A7A);\
        gfx_fill_round_rect(wx+11,wy+9,2,2,1,0xFF9999);\
        /* Minimise — yellow with shine */\
        gfx_fill_round_rect(wx+26,wy+8,12,12,6,0xCCAA33);\
        gfx_fill_round_rect(wx+28,wy+9,4,3,2,0xEECC66);\
        gfx_fill_round_rect(wx+27,wy+9,2,2,1,0xEEDD88);\
        /* Zoom — green with shine */\
        gfx_fill_round_rect(wx+42,wy+8,12,12,6,0x44AA44);\
        gfx_fill_round_rect(wx+44,wy+9,4,3,2,0x66DD66);\
        gfx_fill_round_rect(wx+43,wy+9,2,2,1,0x88EE88);\
    } while(0)

    #define open_app(n) do {\
        win=1;win_type=n;\
        gfx_fill_round_rect(wx+5,wy+7,ww,wh,12,0x000010);\
        gfx_fill_round_rect(wx+3,wy+5,ww,wh,12,0x00081A);\
        gfx_fill_round_rect(wx+6,wy+6,ww,wh,12,0x000000);\
        gfx_fill_round_rect(wx+3,wy+3,ww,wh,12,0x060620);\
        gfx_fill_round_rect(wx,wy,ww,wh,12,0x141452);\
        gfx_round_rect(wx,wy,ww,wh,12,0x4A6ADF);\
        gfx_round_rect(wx+1,wy+1,ww-2,wh-2,11,0x2A4ABE);\
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
        gfx_fill_round_rect(wx+8,wy+44,ww-16,wh-56,6,0x050508);\
        gfx_round_rect(wx+8,wy+44,ww-16,wh-56,6,0x0A3A0A);\
        char _pb[80]; int _pi,_pn=0;\
        char _pfix[40]; int _pf=0;\
        {char _ws[]="user@viteza:~$ ";for(_pf=0;_ws[_pf];_pf++)_pfix[_pf]=_ws[_pf];_pfix[_pf]=0;}\
        for(_pi=0;_pfix[_pi];_pi++)_pb[_pn++]=_pfix[_pi];\
        for(_pi=0;_pi<term_pos&&_pi<44;_pi++)_pb[_pn++]=term_buf[_pi];\
        _pb[_pn]=0;\
        gfx_print(wx+16,wy+44+wh-56-16,0x00FF44,_pb);\
        int _ml = (wh-72)/16 - 1; if(_ml<1) _ml=1;\
        int _start = term_scroll - _ml + 1; if(_start<0) _start=0;\
        int _ty = wy+56;\
        for(int _i=_start; _i<=term_scroll && _i<term_line_count; _i++){\
            uint32_t _tc = 0x00CC44;\
            if(term_lines[_i][0]=='['){_tc=0x00AA44;}if(term_lines[_i][0]=='#'){_tc=0xFF4444;}\
            if(term_lines[_i][0]=='%'){_tc=0xFFFF44;}\
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
    mouse_poll();

    // Debug: USB count + AC97 status
    gfx_print(8, 4, 0x00FF00, "U:");
    char _ud[8]; int _udi=7; _ud[_udi]=0; int _udn=usb_device_count(); do { _ud[--_udi]='0'+_udn%10; _udn/=10; } while(_udn); gfx_print(24, 4, 0x00FF00, _ud+_udi);
    extern int ac97_is_init(void);
    gfx_print(48, 4, ac97_is_init() ? 0x00FF00 : 0xFF4444, ac97_is_init() ? "A:OK" : "A:NO");

    while (1) {
        mouse_poll();
        draw_cursor();
        char k = keyboard_last_char();

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
            ss_frame++;
            gfx_clear(0x000008);
            for (int _si = 0; _si < 120; _si++) {
                int _sx = (_si*691+ss_frame*2+47)%w, _sy = (_si*983+19)%(h*3/5);
                int _sd = (ss_frame+_si)%4;
                uint32_t _sc = 0x2244AA+((_si*37+ss_frame)%64)*0x010101;
                gfx_fill_round_rect(_sx,_sy,_sd+1,_sd+1,1,_sc);
            }
            for (int _pi = 0; _pi < 20; _pi++) {
                int _px = (_pi*613+ss_frame*5+31)%w, _py = (_pi*829+ss_frame*7+17)%(h-100)+50;
                int _ps = (_pi*47+ss_frame)%4+1;
                gfx_fill_round_rect(_px,_py,_ps,_ps,1,0x4466AA+_ps*0x222222);
            }
            gfx_print_scaled(w/2-80,h/2-30,0x4488FF,"VITEZA",3);
            int _vgl = (ss_frame*4)%100;
            uint32_t _vc2 = 0x224488+((_vgl<50?_vgl*2:(100-_vgl)*2))*0x010101;
            gfx_print_scaled(w/2-60,h/2+20,_vc2,"v1.0",2);
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

                gfx_print(wx+16, wy+50, 0x4A9EFF, "Camera — Hardware Status");
                gfx_rect(wx+16, wy+66, ww-32, 1, 0x2A3A6A);

                if (camera_is_present()) {
                    gfx_print(wx+24, wy+80, 0x00CC44, "Device: Present");
                    gfx_print(wx+24, wy+100, 0x8A9ACE, "Name: ");
                    gfx_print(wx+80, wy+100, 0xFFFFFF, camera_get_name());

                    gfx_print(wx+24, wy+120, 0x8A9ACE, "Streaming: ");
                    if (camera_is_streaming()) {
                        gfx_print(wx+100, wy+120, 0x00CC44, "Active");
                    } else {
                        gfx_print(wx+100, wy+120, 0xCCAA00, "Idle  [S]tart stream");
                    }

                    gfx_print(wx+24, wy+160, 0x6A7A9E, "USB Video Class (UVC) camera detected.");
                    gfx_print(wx+24, wy+180, 0x6A7A9E, "Full streaming requires interface descriptor");
                    gfx_print(wx+24, wy+200, 0x6A7A9E, "parsing and isochronous transfer support.");

                    // Toggle streaming on S key
                    if (k == 's' && camera_is_present()) {
                        if (camera_is_streaming()) {
                            camera_stop_stream();
                            gfx_fill_round_rect(wx+16,wy+50,ww-32,wh-64,4,0x0E0E30);
                        } else {
                            camera_start_stream();
                            gfx_fill_round_rect(wx+16,wy+50,ww-32,wh-64,4,0x0E0E30);
                        }
                        continue;
                    }
                } else {
                    gfx_print(wx+24, wy+84, 0xCC4444, "Device: Not Found");
                    gfx_print(wx+24, wy+110, 0x6A7A9E, "No UVC camera hardware detected.");
                    gfx_print(wx+24, wy+130, 0x6A7A9E, "Connect a USB Video Class camera and");
                    gfx_print(wx+24, wy+150, 0x6A7A9E, "re-run. On QEMU, use usb-host passthrough:");
                    gfx_print(wx+24, wy+170, 0x444488, "  -device usb-host,vendorid=0xXXXX,productid=0xYYYY");
                }

                gfx_print(wx+24, wy+wh-30, 0x3A4A6A, "[Esc] close");
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
                if (k == KEY_UP && lp_sel_y > 0) { lp_sel_y--; continue; }
                if (k == KEY_DOWN && lp_sel_y < 3) { lp_sel_y++; continue; }
                if (k == KEY_UP && lp_sel_y == 0) { lp_sel_y = 3; continue; }
                if (k == KEY_DOWN && lp_sel_y == 3) { lp_sel_y = 0; continue; }
                if (k == KEY_LEFT && lp_sel_x > 0) { lp_sel_x--; continue; }
                if (k == KEY_LEFT && lp_sel_x == 0) { lp_sel_x = 2; continue; }
                if (k == KEY_RIGHT && lp_sel_x < 2) { lp_sel_x++; continue; }
                if (k == KEY_RIGHT && lp_sel_x == 2) { lp_sel_x = 0; continue; }
                if (k == '\n' || k == ' ') {
                    int _la = lp_sel_y * 3 + lp_sel_x;
                    if (_la < 12) {
                        int _act = saction[_la];
                        close_win(); search_focus = 1;
                        // Open the selected app
                        if (_act == 1) { open_app(1); draw_mac_title("This PC");
                            gfx_fill_round_rect(wx+16,wy+50,ww-32,20,4,0x12124A);gfx_print(wx+24,wy+53,0x4A9EFF,"System Information");
                            gfx_print(wx+24,wy+84,0x8A9ACE,"Kernel:    Viteza v1.0");
                            gfx_print(wx+24,wy+102,0x8A9ACE,"CPU:       x86_64 Long Mode");
                            gfx_print(wx+24,wy+120,0x8A9ACE,"RAM:       256 MB");
                            char _dstr[32];_dstr[0]=0;
                            _dstr[0]='0'+w/100%10;_dstr[1]='0'+w/10%10;_dstr[2]='0'+w%10;
                            _dstr[3]='x';_dstr[4]='0'+h/100%10;_dstr[5]='0'+h/10%10;_dstr[6]='0'+h%10;_dstr[7]=0;
                            gfx_print(wx+24,wy+138,0x8A9ACE,"Display:   ");gfx_print(wx+100,wy+138,0x00E5FF,_dstr);
                            gfx_print(wx+24,wy+162,0x6A7A9E,"No drives detected");
                            int _udc = usb_device_count();
                            if (_udc > 0) { gfx_print(wx+24, wy+184, 0x4A9EFF, "USB Devices:");
                                for (int _uj = 0; _uj < _udc && _uj < 2; _uj++) gfx_print(wx+24, wy+204+_uj*16, 0x6A8ABE, usb_device_name(_uj));
                            } else { gfx_print(wx+24, wy+184, 0x6A7A9E, "No USB devices"); }
                            gfx_print(wx+24,wy+240,0x3A4A6A,"[Esc] to close"); continue;
                        }
                        if (_act == 2) { open_app(2); draw_mac_title("Network");
                            gfx_fill_round_rect(wx+16,wy+50,ww-32,20,4,0x12124A);gfx_print(wx+24,wy+53,0x4A9EFF,"Wi-Fi Status");
                            gfx_print(wx+24,wy+84,0x8A9ACE,"SSID:     HOME-5G");
                            gfx_print(wx+24,wy+102,0x8A9ACE,"Signal:   Excellent");
                            gfx_print(wx+24,wy+120,0x8A9ACE,"Security: WPA2-PSK");
                            gfx_print(wx+24,wy+144,0x6A7A9E,"IP: 0.0.0.0 (pending)");
                            gfx_print(wx+24,wy+220,0x3A4A6A,"[Esc] to close"); continue;
                        }
                        if (_act == 3) { open_app(3); draw_mac_title("Terminal");
                            term_line_count=0;term_scroll=0;
                            term_add("[Viteza Terminal v1.0]",0);
                            term_add("Type 'help' for commands.",0);
                            term_redraw(); continue;
                        }
                        if (_act == 4) { open_app(4); draw_mac_title("System Settings");
                            settings_redraw(); continue;
                        }
                        if (_act == 5) { open_app(5); draw_mac_title("OreoAI Assistant");
                            chat_line_count=0;chat_scroll=0;chat_pos=0;
                            chat_add("[OreoAI v1.0 - Ask me anything!]");
                            chat_add("Try: hello, who are you, help");
                            chat_redraw(); continue;
                        }
                        if (_act == 6) { open_app(6); draw_mac_title("Calculator");
                            calc_val=0;calc_cur=0;calc_op=0;calc_state=0;
                            calc_redraw(); continue;
                        }
                        if (_act == 7) { open_app(7); draw_mac_title("Notes");
                            gfx_fill_round_rect(wx+16,wy+50,ww-32,20,4,0x12124A);gfx_print(wx+24,wy+53,0x4A9EFF,"My Notes");
                            for(int _ni=0;_ni<note_count&&_ni<8;_ni++){
                                char _ns[4];_ns[0]='0'+(_ni+1)%10;_ns[1]='.';_ns[2]=' ';_ns[3]=0;
                                gfx_print(wx+20,wy+78+_ni*16,0x8899CC,_ns);
                                char _nt[44];note_short(_ni,_nt);
                                gfx_print(wx+40,wy+78+_ni*16,0x6A8ABE,_nt);
                            }
                            gfx_rect(wx+12,wy+210,ww-24,1,0x2A3A6A);
                            gfx_print(wx+16,wy+218,0x4A6A8A,"> "); print_note_buf();
                            gfx_print(wx+16,wy+248,0x3A4A6A,"[Enter] save  [Esc] back"); continue;
                        }
                        if (_act == 8) { open_app(8); draw_mac_title("App Store");
                            gfx_fill_round_rect(wx+16,wy+50,ww-32,20,4,0x12124A);gfx_print(wx+24,wy+53,0x4A9EFF,"Available Apps");
                            for(int _asi=0;_asi<APP_COUNT&&_asi<10;_asi++){
                                int _asy=wy+78+_asi*20;
                                gfx_fill_round_rect(wx+16,_asy-2,ww-32,18,3,app_colors[_asi]);gfx_rect(wx+16,_asy-2,4,18,app_colors[_asi]);
                                gfx_print(wx+28,_asy+1,0xFFFFFF,app_names[_asi]);
                                gfx_print(wx+180,_asy+1,0x8080AA,app_cats[_asi]);
                                if(apps_installed[_asi]){gfx_print(wx+300,_asy+1,0x44FF44,"[Installed]");}
                                else{gfx_print(wx+300,_asy+1,0x808080,"[ ");char _ak[2];_ak[0]='0'+_asi%10;_ak[1]=0;gfx_print(wx+312,_asy+1,0xFFAA00,_ak);gfx_print(wx+324,_asy+1,0x808080," ]");}
                            }
                            gfx_print(wx+16,wy+240,0x3A4A6A,"[0-9] install/uninstall  [Esc] back"); continue;
                        }
                        if (_act == 9) { open_app(9); draw_mac_title("Kairo Studio");
                            studio_init_data(); studio_draw_all(); continue;
                        }
                        if (_act == 10) { open_app(10); draw_mac_title("KairoVM");
                            vm_mode=0;
                            gfx_fill_round_rect(wx+16,wy+50,ww-32,20,4,0x12124A);gfx_print(wx+24,wy+53,0xFF6644,"KairoVM - Virtual Machine Manager");
                            if(vm_count==0){gfx_print(wx+60,wy+100,0x6A7A9E,"No virtual machines yet.");gfx_print(wx+60,wy+124,0x4A9EFF,"Press [c] to create one.");}
                            else{
                                gfx_print(wx+16,wy+76,0x8A8A9A,"Name                  OS                    RAM   CPUs  Disk  Status");
                                gfx_rect(wx+16,wy+92,ww-32,1,0x2A2A4A);
                                for(int _vi=0;_vi<vm_count&&_vi<4;_vi++){
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
                        if (_act == 11) { open_app(11); draw_mac_title("Camera");
                            gfx_print(wx+16, wy+44, 0x4A9EFF, "Camera - Hardware Status"); continue;
                        }
                        if (_act == 12) { open_app(12); draw_mac_title("Kairo Player");
                            gfx_fill_round_rect(wx+20, wy+48, ww-40, 130, 4, 0x000000);
                            gfx_round_rect(wx+20, wy+48, ww-40, 130, 4, 0x3A5A8A);
                            gfx_print(wx+28, wy+56, 0xFFFFFF, "Kairo Visual Engine");
                            for (int _vy = 0; _vy < 100; _vy++) for (int _vx = 0; _vx < 320; _vx++) {
                                int _vc = ((_vx * 5) ^ (_vy * 7)) & 0xFF;
                                gfx_putpixel(wx+30 + _vx, wy+70 + _vy, (_vc<<16)|(_vc<<8)|_vc);
                            }
                            gfx_round_rect(wx+30, wy+70, 320, 100, 2, 0x4A6ADF);
                            gfx_print(wx+ww-130, wy+155, 0x00E5FF, "Kairo Audio");
                            gfx_print(wx+ww-130, wy+163, 0x3A5A8A, "Ready");
                            gfx_print(wx+24, wy+196, 0x8A9ACE, "[P] Play  [S] Stop");
                            gfx_print(wx+20, wy+wh-18, 0x3A4A6A, "Powered by Kairo Visual & Kairo Audio"); continue;
                        }
                    }
                    continue;
                }
                // Redraw Launchpad
                {   gfx_clear(0x000008);
                    for(int _ly=0;_ly<h;_ly+=4){gfx_rect(0,_ly,w,2,0x080820);gfx_rect(0,_ly+2,w,1,0x0A0A30);}
                    gfx_print_scaled(w/2-80, 20, 0x4488FF, "Launchpad", 2);
                    gfx_print_scaled(w/2-24, 48, 0x3A5A8A, "Viteza OS", 1);
                    const char _lpc[12] = {'P','#','>','*','A','+','N','$','{','V','C','~'};
                    uint32_t _lpcols[12] = {0x3A6AFF,0x33BB33,0x3A3A4A,0x7A7A8A,0xAA77FF,0x3A8AEE,0xEE9900,0x3A77EE,0x5A4ABB,0xBB3333,0x55CCEE,0xBB3355};
                    int _ic = 0;
                    for(int _r=0;_r<4;_r++){for(int _c=0;_c<3;_c++){
                        if(_ic>=12||_ic>=MAX_SEARCH)break;
                        int _ix = w/2-160 + _c*110, _iy = 100 + _r*130;
                        int _sel = (lp_sel_y==_r && lp_sel_x==_c);
                        // shadow
                        gfx_fill_round_rect(_ix+4, _iy+4, 80, 80, 16, 0x000008);
                        // icon box
                        gfx_fill_round_rect(_ix, _iy, 80, 80, 16, _lpcols[_ic]);
                        // inner highlight
                        gfx_fill_round_rect(_ix+4, _iy+4, 72, 72, 14, (_lpcols[_ic]&0xFEFEFE)>>1);
                        if(_sel){gfx_round_rect(_ix-3, _iy-3, 86, 86, 18, 0xFFFFFF);gfx_round_rect(_ix-1, _iy-1, 82, 82, 16, 0x4488FF);}
                        // icon character
                        char _ich[2] = {_lpc[_ic],0};
                        gfx_print_scaled(_ix+24, _iy+24, 0xFFFFFF, _ich, 4);
                        // label
                        int _ln = 0; while(sitems[_ic][_ln]) _ln++;
                        gfx_print(_ix+40 - _ln*4, _iy+86, _sel?0xFFFFFF:0x8A9ACE, sitems[_ic]);
                        _ic++;
                    }}
                    gfx_print(w/2-130, h-60, 0x3A4A6A, "[Arrows] navigate  [Enter] launch  [Esc] close");
                }
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
                if (_di >= 0 && _di < 9 && _mx >= _di_base + _di*_di_sp && _mx < _di_base + _di*_di_sp + _di_sz) {
                    int acts[] = {25, 3, 4, 5, 6, 7, 8, 18, 24};
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
            gfx_clear(0x000008);
            for(int _ly=0;_ly<h;_ly+=4){gfx_rect(0,_ly,w,2,0x080820);gfx_rect(0,_ly+2,w,1,0x0A0A30);}
            gfx_print_scaled(w/2-80, 20, 0x4488FF, "Launchpad", 2);
            gfx_print_scaled(w/2-24, 48, 0x3A5A8A, "Viteza OS", 1);
            const char _lpc[12] = {'P','#','>','*','A','+','N','$','{','V','C','~'};
            uint32_t _lpcols[12] = {0x3A6AFF,0x33BB33,0x3A3A4A,0x7A7A8A,0xAA77FF,0x3A8AEE,0xEE9900,0x3A77EE,0x5A4ABB,0xBB3333,0x55CCEE,0xBB3355};
            int _ic = 0;
            for(int _r=0;_r<4;_r++){for(int _c=0;_c<3;_c++){
                if(_ic>=12||_ic>=MAX_SEARCH)break;
                int _ix = w/2-160 + _c*110, _iy = 100 + _r*130;
                // shadow
                gfx_fill_round_rect(_ix+4, _iy+4, 80, 80, 16, 0x000008);
                // icon box
                gfx_fill_round_rect(_ix, _iy, 80, 80, 16, _lpcols[_ic]);
                gfx_fill_round_rect(_ix+4, _iy+4, 72, 72, 14, (_lpcols[_ic]&0xFEFEFE)>>1);
                // icon character
                char _ich[2] = {_lpc[_ic],0};
                gfx_print_scaled(_ix+24, _iy+24, 0xFFFFFF, _ich, 4);
                int _ln = 0; while(sitems[_ic][_ln]) _ln++;
                gfx_print(_ix+40 - _ln*4, _iy+86, 0x8A9ACE, sitems[_ic]);
                _ic++;
            }}
            gfx_print(w/2-130, h-60, 0x3A4A6A, "[Arrows] navigate  [Enter] launch  [Esc] close");
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
