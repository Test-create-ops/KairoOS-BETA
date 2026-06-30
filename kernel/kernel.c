#include "kernel.h"
#include "lib/io.h"

#include "gdt/gdt.c"
#include "tss/tss.c"
#include "idt/idt.c"
#include "interrupts/isr.c"
#include "interrupts/irq.c"
#include "memory/paging.c"
#include "memory/heap.c"
#include "memory/mmu.c"
#include "drivers/graphics/gfx.c"
#include "drivers/keyboard/keyboard.c"
#include "drivers/usb/usb.c"
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
    if (!(regb & 2)) { vh &= 0x7F; } // 12-hour mode: mask off PM bit
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

    // Boot screen animation
    boot_screen(vbe_w, vbe_h);

    int w = vbe_w, h = vbe_h, win = 0, win_type = 0;
    int search_focus = 1, search_pos = 0, search_sel = 0;
    char search_buf[64] = {0};
    char term_buf[128] = {0}; int term_pos = 0, term_line_count = 0, term_scroll = 0;
    char term_lines[60][80]; for (int _t=0;_t<60;_t++) term_lines[_t][0]=0;
    char chat_buf[128] = {0}; int chat_pos = 0, chat_line_count = 0, chat_scroll = 0;
    char chat_lines[60][80]; for (int _c=0;_c<60;_c++) chat_lines[_c][0]=0;
    char notes[10][80]; int note_count = 0, note_sel = 0;
    char note_buf[80] = {0}; int note_pos = 0;
    int set_state = 0;
    int usb_popup = 0;
    int apps_installed[17] = {1,1,1,1,1,0,0,0,1,0,0,0,0,0,0,0,1};
    int studio_mode = 0;
    int vm_count = 0, vm_mode = 0, vm_sel = 0;
    char vm_name[4][32]; int vm_os[4], vm_ram[4], vm_cores[4], vm_disk[4], vm_running[4], vm_cstate[4];
    int vm_creat_step = 0, vm_creat_os = 0, vm_creat_pos = 0, vm_creat_ram = 2048, vm_creat_cores = 2, vm_creat_disk = 32;
    char vm_creat_name[32] = {0};
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

    // ─── Configuration screen ───
    gfx_clear(0x080818);
    gfx_fill_round_rect(0, 0, w, h, 12, 0x0A0A22);
    gfx_round_rect(2, 2, w-4, h-4, 10, BORDER);
    gfx_round_rect(5, 5, w-10, h-10, 8, 0x2A5EAF);

    gfx_print_scaled(w/2-150, 20, BORDER, "SYSTEM CONFIGURATION", 2);
    gfx_rect(w/2-200, 46, 400, 1, 0x2A5EAF);

    // Row 1: PROCESSOR + MEMORY
    gfx_fill_round_rect(60, 70, 260, 130, 6, PANEL_BG);
    gfx_round_rect(60, 70, 260, 130, 6, ACCENT);
    gfx_print(76, 82, ACCENT, "PROCESSOR");
    gfx_print(76, 100, TEXT, "Architecture: x86_64");
    gfx_print(76, 116, TEXT, "Mode: 64-bit Long Mode");
    gfx_print(76, 132, TEXT, "Paging: 4-level (PML4)");
    gfx_print(76, 148, TEXT, "SSE/SSE2: Disabled");
    gfx_print(76, 164, TEXT, "Red Zone: Disabled");

    gfx_fill_round_rect(380, 70, 260, 130, 6, PANEL_BG);
    gfx_round_rect(380, 70, 260, 130, 6, ACCENT);
    gfx_print(396, 82, ACCENT, "MEMORY");
    gfx_print(396, 100, TEXT, "Total RAM: 256 MB");
    gfx_print(396, 116, TEXT, "Heap: 64 KB @ 0x180000");
    gfx_print(396, 132, TEXT, "Page sizes: 4 KB, 2 MB");
    gfx_print(396, 148, TEXT, "MMU: Active");
    gfx_print(396, 164, TEXT, "Identity map: 0-2 MB");

    // Row 2: DISPLAY + BOOT
    gfx_fill_round_rect(60, 220, 260, 130, 6, PANEL_BG);
    gfx_round_rect(60, 220, 260, 130, 6, ACCENT);
    gfx_print(76, 232, ACCENT, "DISPLAY");
    char res_str[32]; int _rp=0;
    char *_rs = "Resolution: "; while(*_rs) res_str[_rp++]=*_rs++;
    int _rw=vbe_w,_rh=vbe_h;
    res_str[_rp++]='0'+_rw/1000; _rw%=1000;
    res_str[_rp++]='0'+_rw/100; _rw%=100;
    res_str[_rp++]='0'+_rw/10;
    res_str[_rp++]='0'+_rw%10;
    res_str[_rp++]=' '; res_str[_rp++]='x'; res_str[_rp++]=' ';
    res_str[_rp++]='0'+_rh/1000; _rh%=1000;
    res_str[_rp++]='0'+_rh/100; _rh%=100;
    res_str[_rp++]='0'+_rh/10;
    res_str[_rp++]='0'+_rh%10;
    res_str[_rp]=0;
    gfx_print(76, 250, TEXT, res_str);
    gfx_print(76, 266, TEXT, "Color depth: 32 bpp");
    gfx_print(76, 282, TEXT, "Pitch: 4096 bytes");
    gfx_print(76, 298, TEXT, "Driver: VBE Bochs");
    gfx_print(76, 314, TEXT, "FB addr: PCI BAR 0");

    gfx_fill_round_rect(380, 220, 260, 130, 6, PANEL_BG);
    gfx_round_rect(380, 220, 260, 130, 6, ACCENT);
    gfx_print(396, 232, ACCENT, "BOOT");
    gfx_print(396, 250, TEXT, "Kernel: KairoOS v1.0");
    gfx_print(396, 266, TEXT, "Loader: PVH / Multiboot2");
    gfx_print(396, 282, TEXT, "Entry: 64-bit long mode");
    gfx_print(396, 298, TEXT, "GDT: 64-bit, 5 entries");
    gfx_print(396, 314, TEXT, "Status: Operational");

    // Row 3: INTERRUPTS + KEYBOARD
    gfx_fill_round_rect(60, 370, 260, 130, 6, PANEL_BG);
    gfx_round_rect(60, 370, 260, 130, 6, ACCENT);
    gfx_print(76, 382, ACCENT, "INTERRUPTS");
    gfx_print(76, 400, TEXT, "IDT: 256 entries");
    gfx_print(76, 416, TEXT, "PIC: Remapped (IRQ0 -> 0x20)");
    gfx_print(76, 432, TEXT, "IRQ0 (Timer): Registered");
    gfx_print(76, 448, TEXT, "IRQ1 (Keyboard): Registered");
    gfx_print(76, 464, TEXT, "Exceptions: Handler active");

    gfx_fill_round_rect(380, 370, 260, 130, 6, PANEL_BG);
    gfx_round_rect(380, 370, 260, 130, 6, ACCENT);
    gfx_print(396, 382, ACCENT, "KEYBOARD");
    gfx_print(396, 400, TEXT, "Device: PS/2 (8042)");
    gfx_print(396, 416, TEXT, "Layout: US (Set 1)");
    gfx_print(396, 432, TEXT, "Interrupt: Enabled");
    gfx_print(396, 448, TEXT, "Buffer: Single char");

    // Live key display
    gfx_fill_round_rect(w/2-130, 520, 260, 36, 6, PANEL_BG);
    gfx_round_rect(w/2-130, 520, 260, 36, 6, PROG_FG);
    gfx_print(w/2-112, 530, DIM, "LAST KEY PRESSED:");

    gfx_rect(w/2-200, 568, 400, 1, 0x1A1A4E);
    gfx_print(w/2-136, 582, DIM, "ANIMATEOS KERNEL V1.0");

    char key_str[2] = " ";
    int key_x = w/2+48, key_y = 530;
    int config_done = 0;

    while (!config_done) {
        char k = keyboard_last_char();
        if (k) {
            key_str[0] = k;
            gfx_rect(key_x, key_y, 12, 14, 0x0A0A22);
            gfx_print(key_x, key_y, PROG_FG, key_str);
            config_done = 1;
            delay();
        }
        asm volatile("hlt");
    }

    // ─── Wi-Fi selection screen ───
    keyboard_last_char();
    delay();

    #define NET_MAX 12
    static const char *net_names[NET_MAX] = {
        "HOME-5G",        "OFFICE-NET",     "GUEST-WIFI",
        "ANIMATEOS-LAB",  "NEIGHBOR-2G",    "PUBLIC-HOTSPOT",
        "SCHOOL-CAMPUS",  "FIBRA-OPTI",     "SMART-HOME",
        "ROOFTOP-ROOF",   "CAFE-FREE",      "DORM-ROOM"
    };
    static int net_sig[NET_MAX] = { 4,3,2,4,1,3,2,4,2,1,2,3 };
    static int net_sec[NET_MAX] = { 1,1,0,1,1,0,1,1,1,0,0,1 };

    int sel = 0, scroll = 0;
    int max_visible = 7;
    int connecting = 0, connected = 0;
    int scan_tick = 0;

    gfx_clear(0x080818);
    gfx_fill_round_rect(0, 0, w, h, 12, 0x0A0A22);
    gfx_round_rect(2, 2, w-4, h-4, 10, BORDER);
    gfx_round_rect(5, 5, w-10, h-10, 8, 0x2A5EAF);

    gfx_print_scaled(w/2-110, 16, BORDER, "WI-FI NETWORKS", 2);
    gfx_rect(w/2-200, 42, 400, 1, 0x2A5EAF);

    int lx = 100, ly = 56, lw = 824, lh = 464;

    gfx_rect(w/2-200, 540, 400, 1, 0x1A1A4E);
    gfx_print(w/2-200, 554, DIM, "[Arrow Up]  [Arrow Down]  [Space] Connect  [R] Rescan  [Q] Back");
    gfx_print(w/2-136, 570, DIM, "ANIMATEOS KERNEL V1.0");

    while (1) {
        gfx_rect(lx, ly, lw, lh, PANEL_BG);
        gfx_round_rect(lx, ly, lw, lh, 6, ACCENT);

        gfx_print(lx+16, ly+10, ACCENT, "AVAILABLE NETWORKS");
        gfx_print(lx+lw-130, ly+10, DIM, "DEVICES FOUND:");
        char nstr[3] = { '0' + NET_MAX/10, '0' + NET_MAX%10, 0 };
        gfx_print(lx+lw-24, ly+10, TEXT, nstr);

        gfx_rect(lx+10, ly+30, lw-20, 1, 0x1A1A4E);

        // Scan anim
        scan_tick++;
        int ndot = (scan_tick / 15) % 4;
        char scan_lbl[16] = "Scanning";
        for (int d = 0; d < ndot; d++) scan_lbl[8+d] = '.';
        scan_lbl[8+ndot] = 0;
        gfx_print(lx+16, ly+lh-20, DIM, scan_lbl);

        if (sel < scroll) scroll = sel;
        if (sel >= scroll + max_visible) scroll = sel - max_visible + 1;

        for (int i = scroll; i < NET_MAX && i < scroll + max_visible; i++) {
            int idx = i - scroll;
            int ey = ly + 44 + idx * 56;

            if (i == sel && !connecting) {
                gfx_fill_round_rect(lx+8, ey-2, lw-16, 50, 4, 0x15154A);
            }

            int sx = lx + 20, sy = ey + 4;

            // Signal bars (4 bars, height 2/5/8/11)
            uint32_t sc = (i == sel && !connecting) ? PROG_FG : TEXT;
            for (int b = 0; b < 4; b++) {
                int bh = (b < net_sig[i]) ? (3 + b*3) : 2;
                gfx_rect(sx + b*7, sy + 14 - bh, 5, bh, sc);
            }

            // Lock icon for secured
            if (net_sec[i]) {
                gfx_rect(sx+33, sy+2, 6, 4, sc);
                gfx_rect(sx+31, sy+6, 10, 7, sc);
                gfx_rect(sx+35, sy+8, 2, 3, 0x0A0A22);
            }

            // SSID
            int tx = sx + 48;
            gfx_print(tx, sy, (i == sel && !connecting) ? PROG_FG : TEXT, net_names[i]);
            gfx_print(tx, sy+16, DIM, net_sec[i] ? "WPA2-PSK" : "Open");

            // Signal text
            static const char *sig_txt[] = { "", "Weak", "Fair", "Good", "Excellent" };
            gfx_print(lx+lw-120, sy+4, DIM, sig_txt[net_sig[i]]);
        }

        // Connecting / Connected overlay
        if (connecting && !connected) {
            gfx_rect(lx+lw-180, ly+10, 170, 16, PANEL_BG);
            gfx_print(lx+lw-176, ly+10, 0xFFAA00, "Connecting...");
            delay();
            connected = 1;
            connecting = 0;
        }
        if (connected) {
            gfx_rect(lx+lw-180, ly+10, 170, 16, PANEL_BG);
            gfx_print(lx+lw-176, ly+10, 0x44FF44, "Connected!");
        }

        char k = keyboard_last_char();

        if (!connecting && !connected) {
            if (k == KEY_UP && sel > 0) sel--;
            if (k == KEY_DOWN && sel < NET_MAX-1) sel++;
            if (k == '\n' || k == ' ') { connecting = 1; connected = 0; }
            if (k == 'r') { connecting = 0; connected = 0; }
        }
        if (k == 'q' || k == 'b') break;

        if (k) {
            gfx_rect(w/2-136, 554, 120, 14, 0x0A0A22);
            key_str[0] = k;
            gfx_print(w/2-136, 554, PROG_FG, key_str);
        }

        asm volatile("hlt");
    }

    // ─── Desktop (macOS style) ───
redraw_desktop:
    gfx_clear(0x080818);
    // Deep macOS Big Sur-style gradient (dark blue → deep purple)
    for (int y = 0; y < h; y++) {
        int t = y * 255 / h;
        int r = 8 + t/30, g = 6 + t/20, b = 24 + t/10;
        if (y < 180) { int f = 180-y; r = 18+f/12; g = 10+f/20; b = 50+f/6; }
        if (r > 32) { r = 32; }
        if (g > 28) { g = 28; }
        if (b > 60) { b = 60; }
        gfx_rect(0, y, w, 1, (r<<16)|(g<<8)|b);
    }
    // Soft radial glow from top-center
    for (int i = 0; i < 80; i++) {
        int a = (80-i)*3; if (a > 60) a = 60;
        gfx_rect(w/2 - i*5, h/5 - i/4, i*10, 2, (a*3/4<<16)|(a/2<<8)|a);
        gfx_rect(w/2 - i*5, h/5 + i/4, i*10, 2, (a*3/4<<16)|(a/2<<8)|a);
    }
    // Soft star glow dots (rounded, no pixels)
    for (int i = 0; i < 50; i++) {
        int sx = (i*691+47)%w, sy = (i*983+19)%(h*3/5);
        int sb = (i*257+13)%6;
        if (sb < 2) continue;
        uint32_t sc = sb > 4 ? 0xAAC0EE : (sb > 2 ? 0x8899CC : 0x6677AA);
        gfx_fill_round_rect(sx-1, sy-1, 3, 3, 1, sc);
    }

    // ─── Top menu bar (macOS style) ───
    int mby = 0, mbh = 24;
    gfx_rect(0, mby, w, mbh, 0x0A0A1C);
    gfx_rect(0, mby+mbh-1, w, 1, 0x1A1A3A);
    gfx_print(10, 5, 0x8899CC, "KairoOS");
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
    // Wi-Fi icon
    gfx_fill_round_rect(mb_rx-16, 8, 10, 2, 1, 0x5566AA);
    gfx_putpixel(mb_rx-11, 7, 0x5566AA);
    gfx_putpixel(mb_rx-11, 15, 0x5566AA);
    // Separator
    mb_rx -= 14;
    gfx_rect(mb_rx-10, 5, 1, 14, 0x1A1A3A);

    // ─── Dock (macOS style) ───
    int dc_w = 520, dc_h = 56, dc_r = 28;
    int dc_x = w/2 - dc_w/2, dc_y = h - dc_h - 8;
    // Dock shadow
    gfx_fill_round_rect(dc_x+3, dc_y+3, dc_w, dc_h, dc_r, 0x000000);
    // Dock background (glass-like)
    gfx_fill_round_rect(dc_x, dc_y, dc_w, dc_h, dc_r, 0x0C0C24);
    gfx_round_rect(dc_x, dc_y, dc_w, dc_h, dc_r, 0x2A2A5A);
    gfx_round_rect(dc_x+1, dc_y+1, dc_w-2, dc_h-2, dc_r-1, 0x1A1A4A);
    // Glass highlight line
    gfx_rect(dc_x+20, dc_y+3, dc_w-40, 1, 0x3A4A7A);
    gfx_rect(dc_x+40, dc_y+4, dc_w-80, 1, 0x2A3A6A);
    // Subtle dock glow beneath
    gfx_rect(dc_x+20, dc_y+dc_h, dc_w-40, 1, 0x1A2A5A);
    gfx_rect(dc_x+40, dc_y+dc_h+1, dc_w-80, 1, 0x0A1A4A);

    // ─── Dock Icons — macOS quality, no pixels ───
    int di_y = dc_y + 8, di_sz = 40, di_sp = 56;
    int di_base = dc_x + (dc_w - 5*di_sp)/2;
    int di_x, di_cy;

    #define di_sh(_i,_c) do {\
        di_x = di_base + (_i)*di_sp; di_cy = di_y;\
        gfx_fill_round_rect(di_x+1,di_cy+1,di_sz,di_sz,9,0x000000);\
        gfx_fill_round_rect(di_x,di_cy,di_sz,di_sz,10,_c);\
        gfx_round_rect(di_x,di_cy,di_sz,di_sz,10,0x4A6AAF);\
    } while(0)

    // Finder (0) — macOS smiling face
    di_sh(0,0x4A7AFF); {
        gfx_fill_round_rect(di_x+7,di_cy+7,26,26,13,0xFFFFFF);
        gfx_fill_round_rect(di_x+12,di_cy+12,6,6,3,0x4A7AFF);
        gfx_fill_round_rect(di_x+22,di_cy+12,6,6,3,0x4A7AFF);
        gfx_fill_round_rect(di_x+13,di_cy+13,2,2,1,0xFFFFFF);
        gfx_fill_round_rect(di_x+23,di_cy+13,2,2,1,0xFFFFFF);
        gfx_rect(di_x+13,di_cy+22,14,2,0x4A7AFF);
        gfx_rect(di_x+14,di_cy+23,4,1,0x4A7AFF);
        gfx_rect(di_x+22,di_cy+23,4,1,0x4A7AFF);
        gfx_rect(di_x+3,di_cy+4,3,1,0x8AB4FF);
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

    // App indicator dots under active icons
    for (int _di = 0; _di < 5; _di++) {
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

    // Search in top bar instead of dock
    int sb_x = w/2 - 120, sb_y = mbh+3, sb_w = 240, sb_h = 20;
    gfx_fill_round_rect(sb_x, sb_y, sb_w, sb_h, 10, 0x0A0A28);
    gfx_round_rect(sb_x, sb_y, sb_w, sb_h, 10, 0x2A3A6A);
    gfx_print(sb_x+8, sb_y+2, 0x3A5A8A, "Search...");

    // Keyboard shortcut bar (above dock)
    gfx_print(w/2-400, dc_y-18, 0x3A4A6A,
        "[1]  [2]  [3]Term  [4]Set  [5]Oreo  [6]Calc  [7]Notes  [8]Store  [9]Studio  [0]VM  [U]USB  [Esc]");

    // Search focused by default
    search_focus = 1; int _tick = 0;

    // Clock refresh helper (now in menu bar)
    #define clock_refresh() do {\
        rtc_read(&_hr,&_mn);\
        _time[0]='0'+_hr/10;_time[1]='0'+_hr%10;_time[2]=':';\
        _time[3]='0'+_mn/10;_time[4]='0'+_mn%10;_time[5]=0;\
        gfx_rect(_clk_x,5,55,12,0x0A0A1C);\
        gfx_print(_clk_x,5,TEXT,_time);\
    } while(0)

    #define draw_toggle(tx,ty,ton) do {\
        if(ton){gfx_fill_round_rect(tx,ty,24,12,6,0x006644);gfx_fill_round_rect(tx+12,ty+2,8,8,4,0x00FF88);}\
        else{gfx_fill_round_rect(tx,ty,24,12,6,0x2A2A2A);gfx_fill_round_rect(tx+4,ty+2,8,8,4,0x6A6A6A);}\
    } while(0)

    #define settings_redraw() do {\
        gfx_rect(wx+16,wy+76,ww-32,80,0x141452);\
        draw_toggle(wx+24,wy+84,set_state&1);\
        gfx_print(wx+56,wy+83,0x8A9ACE,"Dark Mode    [1]");\
        draw_toggle(wx+24,wy+104,set_state&2);\
        gfx_print(wx+56,wy+103,0x8A9ACE,"Wi-Fi Enabled [2]");\
        draw_toggle(wx+24,wy+124,set_state&4);\
        gfx_print(wx+56,wy+123,0x8A9ACE,"Notifications [3]");\
        draw_toggle(wx+24,wy+144,set_state&8);\
        gfx_print(wx+56,wy+143,0x8A9ACE,"Developer Mode [4]");\
    } while(0)

    #define MAX_SEARCH 10
    const char *sitems[MAX_SEARCH] = {"This PC","Network","Terminal","Settings","OreoAI","Calculator","Notes","App Store","Kairo Studio","KairoVM"};
    int saction[MAX_SEARCH] = {1,2,3,4,5,6,7,8,9,10};

    #define APP_COUNT 17
    const char *app_names[APP_COUNT] = {
        "Kairo Studio","Terminal","Calculator","Notes","OreoAI",
        "File Manager","Image Viewer","Text Editor","Settings",
        "System Monitor","Code Compiler","Web Browser","Weather",
        "Music Player","Package Manager","Clock","KairoVM"
    };
    const char *app_cats[APP_COUNT] = {
        "Development","System","Utilities","Productivity","AI",
        "System","Multimedia","Productivity","System",
        "System","Development","Internet","Utilities",
        "Multimedia","System","Utilities","Virtualization"
    };
    uint32_t app_colors[APP_COUNT] = {
        0x6A5ACD,0x00CC44,0x4A9EFF,0xFFAA00,0xBB88FF,
        0x4A7AFF,0xFF8844,0x88AACC,0x8A8A9A,
        0x44CCAA,0xFF6644,0x44AAFF,0x66DDFF,
        0xFF66AA,0x66AA44,0x88AACC,0xCC4444
    };

    int wx=w/2-200, wy=h/2-160, ww=400, wh=280;

    // Helper to close window (restore macOS wallpaper background + stars)
    #define close_win() do {\
        win=0;win_type=0;\
        int _cx1=wx,_cy1=wy,_cx2=wx+ww+8,_cy2=wy+wh+8;\
        if(_cx2>w){_cx2=w;}if(_cy2>h){_cy2=h;}\
        for(int _y=_cy1;_y<_cy2;_y++){\
            int _t=_y*255/h,_r=8+_t/30,_g=6+_t/20,_b=24+_t/10;\
            if(_y<180){int _f=180-_y;_r=18+_f/12;_g=10+_f/20;_b=50+_f/6;}\
            if(_r>32){_r=32;}if(_g>28){_g=28;}if(_b>60){_b=60;}\
            gfx_rect(_cx1,_y,_cx2-_cx1,1,(_r<<16)|(_g<<8)|_b);\
        }\
        for(int _i=0;_i<60;_i++){\
            int _sx=(_i*691+47)%w,_sy=(_i*983+19)%(h*3/5);\
            if(_sx>=_cx1&&_sx<_cx2&&_sy>=_cy1&&_sy<_cy2){\
                int _sb=(_i*257+13)%6;if(_sb<2)continue;\
                uint32_t _sc=_sb>4?0xAAC0EE:(_sb>2?0x8899CC:0x6677AA);\
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
        gfx_fill_round_rect(nw_x,nw_y,nw_w,nw_h,8,0x080820);\
        gfx_round_rect(nw_x,nw_y,nw_w,nw_h,8,0x2A3A6A);\
        gfx_rect(nw_x+10,nw_y+26,nw_w-20,1,0x1A2A5A);\
        gfx_print(nw_x+12,nw_y+5,0x6A8ABE,"Notes");\
        gfx_fill_round_rect(nw_x+nw_w-30,nw_y+4,18,14,7,0xCC4444);\
        if(note_count<10){char _nb[2];_nb[0]='0'+note_count;_nb[1]=0;gfx_print(nw_x+nw_w-26,nw_y+4,0xFFFFFF,_nb);}\
        else{char _nb[3];_nb[0]='0'+note_count/10;_nb[1]='0'+note_count%10;_nb[2]=0;gfx_print(nw_x+nw_w-26,nw_y+4,0xFFFFFF,_nb);}\
        if(note_count>0&&note_sel<note_count){\
            char _nn[26];int _ni;for(_ni=0;notes[note_sel][_ni]&&_ni<21;_ni++)_nn[_ni]=notes[note_sel][_ni];\
            if(notes[note_sel][_ni]){_nn[_ni++]='.';_nn[_ni++]='.';_nn[_ni++]='.';}_nn[_ni]=0;\
            gfx_print(nw_x+12,nw_y+30,0x8899CC,_nn);\
            gfx_print(nw_x+10,nw_y+nw_h-18,0x4A6A8A,"<");\
            if(note_sel+1<10){char _np[3];_np[0]='0'+(note_sel+1);_np[1]='/';_np[2]=0;gfx_print(nw_x+80,nw_y+nw_h-20,0x5A6A8A,_np);}\
            else{char _np[4];_np[0]='0'+(note_sel+1)/10;_np[1]='0'+(note_sel+1)%10;_np[2]='/';_np[3]=0;gfx_print(nw_x+76,nw_y+nw_h-20,0x5A6A8A,_np);}\
            if(note_count<10){char _nt[2];_nt[0]='0'+note_count;_nt[1]=0;gfx_print(nw_x+92,nw_y+nw_h-20,0x5A6A8A,_nt);}\
            else{char _nt[3];_nt[0]='0'+note_count/10;_nt[1]='0'+note_count%10;_nt[2]=0;gfx_print(nw_x+92,nw_y+nw_h-20,0x5A6A8A,_nt);}\
            gfx_print(nw_x+nw_w-22,nw_y+nw_h-18,0x4A6A8A,">");\
        }else{\
            gfx_print(nw_x+12,nw_y+36,0x3A4A6A,"No notes  [7]");\
        }\
    } while(0)

    // macOS window chrome — realistic traffic light buttons with shine
    #define draw_mac_title(k_) do {\
        gfx_fill_round_rect(wx+4,wy+4,ww-8,30,8,0x1E1E3E);\
        gfx_fill_round_rect(wx+4,wy+4,ww-8,20,8,0x282852);\
        gfx_rect(wx+8,wy+22,ww-16,12,0x1E1E3E);\
        gfx_rect(wx+4,wy+35,ww-8,1,0x3A3A6A);\
        gfx_print(wx+ww/2-28,wy+7,0x8899CC,k_);\
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
        gfx_fill_round_rect(wx+6,wy+6,ww,wh,12,0x000000);\
        gfx_fill_round_rect(wx+3,wy+3,ww,wh,12,0x060620);\
        gfx_fill_round_rect(wx,wy,ww,wh,12,0x141452);\
        gfx_round_rect(wx,wy,ww,wh,12,0x4A6ADF);\
        gfx_round_rect(wx+1,wy+1,ww-2,wh-2,11,0x2A4ABE);\
    } while(0)

    // Add a wrapped terminal line (max 44 chars per line)
    #define term_add(fmt,lbl) do {\
        if(term_line_count<60){\
            int _i,_n;for(_n=0;fmt[_n];_n++);\
            if(_n<=44){\
                for(_i=0;fmt[_i]&&_i<79;_i++)term_lines[term_line_count][_i]=fmt[_i];\
                term_lines[term_line_count][_i]=0;term_line_count++;\
            }else{\
                int _sp=44;while(_sp>0&&fmt[_sp]!=' ')_sp--;\
                if(_sp<1)_sp=44;\
                for(_i=0;_i<_sp;_i++)term_lines[term_line_count][_i]=fmt[_i];\
                term_lines[term_line_count][_i]=0;term_line_count++;\
                int _j=_sp+1;\
                if(term_line_count<60){\
                    for(_i=0;fmt[_j+_i]&&_i<79;_i++)term_lines[term_line_count][_i]=fmt[_j+_i];\
                    term_lines[term_line_count][_i]=0;term_line_count++;\
                }\
            }\
            if(term_scroll==term_line_count-1||term_scroll==term_line_count-2)term_scroll=term_line_count-1;\
            if(term_scroll>term_line_count-1)term_scroll=term_line_count-1;\
        }\
    } while(0)

    // Add a wrapped chat line (max 44 chars per line)
    #define chat_add(fmt) do {\
        if(chat_line_count<60){\
            int _i,_n;for(_n=0;fmt[_n];_n++);\
            if(_n<=44){\
                for(_i=0;fmt[_i]&&_i<79;_i++)chat_lines[chat_line_count][_i]=fmt[_i];\
                chat_lines[chat_line_count][_i]=0;chat_line_count++;\
            }else{\
                int _sp=44;while(_sp>0&&fmt[_sp]!=' ')_sp--;\
                if(_sp<1)_sp=44;\
                for(_i=0;_i<_sp;_i++)chat_lines[chat_line_count][_i]=fmt[_i];\
                chat_lines[chat_line_count][_i]=0;chat_line_count++;\
                int _j=_sp+1;\
                if(chat_line_count<60){\
                    for(_i=0;fmt[_j+_i]&&_i<79;_i++)chat_lines[chat_line_count][_i]=fmt[_j+_i];\
                    chat_lines[chat_line_count][_i]=0;chat_line_count++;\
                }\
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
        {char _ws[]="user@kairoos:~$ ";for(_pf=0;_ws[_pf];_pf++)_pfix[_pf]=_ws[_pf];_pfix[_pf]=0;}\
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

    while (1) {
        char k = keyboard_last_char();

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
            for (int _y=py; _y<py+ph+4; _y++) {
                int _t=_y*255/h,_r=8+_t/30,_g=6+_t/20,_b=24+_t/10;
                if(_y<180){int _f=180-_y;_r=18+_f/12;_g=10+_f/20;_b=50+_f/6;}
                if(_r>32){_r=32;}if(_g>28){_g=28;}if(_b>60){_b=60;}
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
            if ((k == 27 || k == 'q') && win_type != 3 && win_type != 5) { close_win(); search_focus = 1; continue; }

            // Terminal input
            if (win_type == 3) {
                if (k == 27 || k == 'q') { close_win(); search_focus = 1; continue; }
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
                            term_add("user@kairoos","");
                        } else if (term_buf[0]=='v'&&term_buf[1]=='e'&&term_buf[2]=='r'&&!term_buf[3]) {
                            term_add("KairoOS Kernel v1.0 (x86_64)","");
                        } else if (term_buf[0]=='d'&&term_buf[1]=='a'&&term_buf[2]=='t'&&term_buf[3]=='e'&&!term_buf[4]) {
                            term_add("Sun Jun 28 2026","");
                        } else if (term_buf[0]=='n'&&term_buf[1]=='e'&&term_buf[2]=='o'&&term_buf[3]=='f'&&term_buf[4]=='e'&&term_buf[5]=='t'&&term_buf[6]=='c'&&term_buf[7]=='h'&&!term_buf[8]) {
                            term_add("OS: KairoOS v1.0","");term_add("Kernel: x86_64 Long Mode","");
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
                if (k == 27 || k == 'q') { close_win(); search_focus = 1; continue; }
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
                        // Predefined answers
                        char _q[80]; for(_ti=0;chat_buf[_ti];_ti++) {
                            char _c=chat_buf[_ti];
                            if(_c>='A'&&_c<='Z')_c+=32;
                            _q[_ti]=_c;
                        } _q[_ti]=0;
                        char *resp = "I don't understand yet.";
                        if (strmatch(_q,"hello")||strmatch(_q,"hi")||strmatch(_q,"hey")) resp = "Hello! I'm OreoAI, your KairoOS (OreoOS) assistant.";
                        else if (strmatch(_q,"who are you")) resp = "I'm OreoAI, the built-in assistant of KairoOS (OreoOS).";
                        else if (strmatch(_q,"what can you do")) resp = "I can answer predefined questions about KairoOS (OreoOS).";
                        else if (strmatch(_q,"what is animateos")||strmatch(_q,"what is kairoos")||strmatch(_q,"what is oreoos")) resp = "KairoOS (OreoOS) is a custom x86_64 operating system kernel.";
                        else if (strmatch(_q,"version")) resp = "KairoOS Kernel v1.0 (x86_64 Long Mode)";
                        else if (strmatch(_q,"help")) resp = "Try: hello, who are you, what can you do, what is animateos/kairoos/oreoos, version, creator, github";
                        else if (strmatch(_q,"creator")||strmatch(_q,"who made you")) resp = "I was created by the KairoOS (OreoOS) developer.";
                        else if (strmatch(_q,"github")) resp = "KairoOS (OreoOS) is planned to be published on GitHub soon!";
                        else if (strmatch(_q,"bye")||strmatch(_q,"goodbye")) resp = "Goodbye! Type any question anytime.";
                        // Add response
                        char _rsp[80]; for(_ti=0;resp[_ti];_ti++) {
                            _rsp[_ti]=resp[_ti];
                        } _rsp[_ti]=0;
                        chat_add(_rsp);
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
                if (k == 27 || k == 'q') { close_win(); search_focus = 1; continue; }
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

            // Kairo Studio (9)
            if (win_type == 9) {
                if (k == 27 || k == 'q') { close_win(); search_focus = 1; continue; }
                if (k == '1') { studio_mode = 0;
                    gfx_fill_round_rect(wx+16,wy+50,ww-32,20,4,0x12124A);
                    gfx_print(wx+24,wy+53,0x6A5ACD,"Kairo Studio  —  ");
                    gfx_print(wx+180,wy+53,0x44AADD,"[XML]");
                    gfx_fill_round_rect(wx+16,wy+76,ww-32,wh-90,4,0x08080E);
                    // Sample XML
                    gfx_print(wx+24,wy+84,0x4488AA,"<");gfx_print(wx+32,wy+84,0x44AADD,"catalog");gfx_print(wx+88,wy+84,0x4488AA,">");
                    gfx_print(wx+40,wy+100,0x4488AA,"<");gfx_print(wx+48,wy+100,0x44AADD,"book");gfx_print(wx+80,wy+100,0xFFFFFF,"id=");gfx_print(wx+100,wy+100,0x44AA44,"\"bk101\"");gfx_print(wx+148,wy+100,0x4488AA,">");
                    gfx_print(wx+56,wy+116,0x4488AA,"<");gfx_print(wx+64,wy+116,0x44AADD,"title");gfx_print(wx+96,wy+116,0x4488AA,">");gfx_print(wx+104,wy+116,0xFFFFFF,"KairoOS");gfx_print(wx+160,wy+116,0x4488AA,"</");gfx_print(wx+176,wy+116,0x44AADD,"title");gfx_print(wx+208,wy+116,0x4488AA,">");
                    gfx_print(wx+56,wy+132,0x4488AA,"<");gfx_print(wx+64,wy+132,0x44AADD,"author");gfx_print(wx+108,wy+132,0x4488AA,">");gfx_print(wx+116,wy+132,0xFFFFFF,"Kairo Dev");gfx_print(wx+184,wy+132,0x4488AA,"</");gfx_print(wx+200,wy+132,0x44AADD,"author");gfx_print(wx+244,wy+132,0x4488AA,">");
                    gfx_print(wx+56,wy+148,0x4488AA,"<");gfx_print(wx+64,wy+148,0x44AADD,"price");gfx_print(wx+96,wy+148,0xFFFFFF,"currency=");gfx_print(wx+152,wy+148,0x44AA44,"\"USD\"");gfx_print(wx+192,wy+148,0x4488AA,">");gfx_print(wx+200,wy+148,0xFFAA44,"29.99");gfx_print(wx+240,wy+148,0x4488AA,"</");gfx_print(wx+256,wy+148,0x44AADD,"price");gfx_print(wx+288,wy+148,0x4488AA,">");
                    gfx_print(wx+24,wy+180,0x3A3A5A,"[1] XML  [2] JS  [Esc] back");
                    continue;
                }
                if (k == '2') { studio_mode = 1;
                    gfx_fill_round_rect(wx+16,wy+50,ww-32,20,4,0x12124A);
                    gfx_print(wx+24,wy+53,0x6A5ACD,"Kairo Studio  —  ");
                    gfx_print(wx+180,wy+53,0xFFAA44,"[JS]");
                    gfx_fill_round_rect(wx+16,wy+76,ww-32,wh-90,4,0x08080E);
                    // Sample JS
                    gfx_print(wx+24,wy+84,0x8888CC,"// KairoOS App Controller");
                    gfx_print(wx+24,wy+100,0x4466DD,"class");gfx_print(wx+64,wy+100,0xFFFFFF," AppController");
                    gfx_print(wx+24,wy+116,0x4466DD,"constructor");gfx_print(wx+108,wy+116,0xFFFFFF,"(name, ver)");gfx_print(wx+176,wy+116,0x8888CC,"{");
                    gfx_print(wx+40,wy+132,0x4466DD,"this");gfx_print(wx+72,wy+132,0xFFFFFF,".name = name;");
                    gfx_print(wx+40,wy+148,0x4466DD,"this");gfx_print(wx+72,wy+148,0xFFFFFF,".version = ver;");
                    gfx_print(wx+24,wy+164,0x8888CC,"}");
                    gfx_print(wx+24,wy+180,0x3A3A5A,"[1] XML  [2] JS  [Esc] back");
                    continue;
                }
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            // KairoVM (10)
            if (win_type == 10) {
                if (k == 27 || k == 'q') { close_win(); search_focus = 1; continue; }

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

            // Settings interactive toggles
            if (win_type == 4) {
                if (k == '1') { set_state ^= 1; settings_redraw(); continue; }
                if (k == '2') { set_state ^= 2; settings_redraw(); continue; }
                if (k == '3') { set_state ^= 4; settings_redraw(); continue; }
                if (k == '4') { set_state ^= 8; settings_redraw(); continue; }
                if (!k) { asm volatile("hlt"); continue; }
                continue;
            }

            if (!k) { asm volatile("hlt"); continue; }
            continue; // other windows: just wait for Esc
        }

        // ─── NO WINDOW OPEN ───

        // ALT+B → BIOS Setup
        if (is_alt_pressed() && k == 'b' && !win) {
            gfx_clear(0x0000AA);
            gfx_rect(0, 0, w, 1, 0xFFFFFF);
            gfx_rect(0, h-14, w, 14, 0x000080);
            gfx_print_scaled(w/2-140, 10, 0xFFFFFF, "KAIROOS BIOS SETUP v1.0", 2);
            gfx_rect(w/2-220, 36, 440, 1, 0xFFFFFF);
            gfx_print(40, 56, 0x8080FF, "Main    Advanced    Boot    Security    Save & Exit");
            gfx_rect(40, 70, 460, 1, 0x8080FF);

            gfx_print(40, 98, 0xFFFF00, "[1]");
            gfx_print(80, 98, 0xFFFFFF, "System Information");

            gfx_print(40, 124, 0xFFFF00, "[2]");
            gfx_print(80, 124, 0xFFFFFF, "Exit");

            gfx_fill_round_rect(w/2-160, h-110, 320, 50, 6, 0x000080);
            gfx_print(60, h-96, 0x00FF00, "KairoOS v1.0  |  x86_64 Long Mode  |  OreoWM");
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
            gfx_print(wx+24,wy+84,0x8A9ACE,"Kernel:    KairoOS v1.0");
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
            term_add("[KairoOS Terminal v1.0]",0);
            term_add("Type 'help' for commands.",0);
            term_redraw(); continue;
        }
        if (k == '4' && !win) { open_app(4); draw_mac_title("Settings");
            gfx_fill_round_rect(wx+16,wy+50,ww-32,20,4,0x12124A);gfx_print(wx+24,wy+53,0x4A9EFF,"System Settings");
            draw_toggle(wx+24,wy+84,set_state&1);
            gfx_print(wx+56,wy+83,0x8A9ACE,"Dark Mode    [1]");
            draw_toggle(wx+24,wy+104,set_state&2);
            gfx_print(wx+56,wy+103,0x8A9ACE,"Wi-Fi Enabled [2]");
            draw_toggle(wx+24,wy+124,set_state&4);
            gfx_print(wx+56,wy+123,0x8A9ACE,"Notifications [3]");
            draw_toggle(wx+24,wy+144,set_state&8);
            gfx_print(wx+56,wy+143,0x8A9ACE,"Developer Mode [4]");
            gfx_fill_round_rect(wx+16,wy+166,ww-32,20,4,0x12124A);gfx_print(wx+24,wy+169,0x4A9EFF,"About");
            gfx_print(wx+24,wy+194,0x6A7A9E,"KairoOS v1.0 — x86_64");
            gfx_print(wx+24,wy+220,0x3A4A6A,"[1-4] toggle  [Esc] back"); continue;
        }
        if (k == '5' && !win) { open_app(5); draw_mac_title("OreoAI Assistant");
            chat_line_count=0;chat_scroll=0;chat_pos=0;
            chat_add("[OreoAI v1.0 — Ask me anything!]");
            chat_add("Try: hello, who are you, help");
            chat_redraw(); continue;
        }
        if (k == '6' && !win) { open_app(6); draw_mac_title("Calculator");
            gfx_fill_round_rect(wx+16,wy+50,ww-32,20,4,0x12124A);gfx_print(wx+24,wy+53,0x4A9EFF,"Calc");
            gfx_fill_round_rect(wx+40,wy+84,ww-80,32,4,0x000000);gfx_round_rect(wx+40,wy+84,ww-80,32,4,0x2A4A7A);
            gfx_print(wx+48,wy+92,0x00FF44,"calc N+M");
            gfx_print(wx+24,wy+130,0x6A7A9E,"Use terminal: calc N+M");
            gfx_print(wx+24,wy+148,0x6A7A9E,"(press 3 for Terminal)");
            gfx_print(wx+24,wy+174,0x6A7A9E,"Or type in terminal:");
            gfx_print(wx+36,wy+192,0x8A8ACC,"calc 12+34  →  46");
            gfx_print(wx+36,wy+210,0x8A8ACC,"calc 100-27 →  73");
            gfx_print(wx+24,wy+230,0x3A4A6A,"[Esc] to close"); continue;
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
            studio_mode = 0;
            gfx_fill_round_rect(wx+16,wy+50,ww-32,20,4,0x12124A);
            gfx_print(wx+24,wy+53,0x6A5ACD,"Kairo Studio  —  ");
            if(studio_mode){gfx_print(wx+180,wy+53,0xFFAA44,"[JS]");}else{gfx_print(wx+180,wy+53,0x44AADD,"[XML]");}
            gfx_fill_round_rect(wx+16,wy+76,ww-32,wh-90,4,0x08080E);
            gfx_print(wx+24,wy+84,0x4488AA,"<");
            gfx_print(wx+32,wy+84,0x44AADD,"catalog");
            gfx_print(wx+88,wy+84,0xAAAA44,"book");
            gfx_print(wx+116,wy+84,0xFFFFFF,"id=");
            gfx_print(wx+136,wy+84,0x44AA44,"\"bk101\"");
            gfx_print(wx+184,wy+84,0x4488AA,">");
            gfx_print(wx+24,wy+100,0x4488AA,"<");
            gfx_print(wx+32,wy+100,0x44AADD,"title");
            gfx_print(wx+64,wy+100,0x4488AA,">");
            gfx_print(wx+72,wy+100,0xFFFFFF,"KairoOS Handbook");
            gfx_print(wx+188,wy+100,0x4488AA,"<");
            gfx_print(wx+196,wy+100,0x44AADD,"/title");
            gfx_print(wx+236,wy+100,0x4488AA,">");
            gfx_print(wx+24,wy+132,0x3A3A5A,"[1] XML  [2] JS  [Esc] back");
            continue;
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
                        gfx_print(wx+24,wy+84,0x8A9ACE,"Kernel:  KairoOS v1.0");
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
                        term_add("[KairoOS Terminal v1.0]",0);
                        term_add("Type 'help' for commands.",0);
                        term_redraw();
                    } else if (act == 4) { draw_mac_title("Settings");
                        gfx_fill_round_rect(wx+16,wy+50,ww-32,20,4,0x12124A);gfx_print(wx+24,wy+53,0x4A9EFF,"System");
                        draw_toggle(wx+24,wy+84,set_state&1);
                        gfx_print(wx+56,wy+83,0x8A9ACE,"Dark Mode");
                        draw_toggle(wx+24,wy+104,set_state&2);
                        gfx_print(wx+56,wy+103,0x8A9ACE,"Wi-Fi Enabled");
                        draw_toggle(wx+24,wy+124,set_state&4);
                        gfx_print(wx+56,wy+123,0x8A9ACE,"Notifications");
                        draw_toggle(wx+24,wy+144,set_state&8);
                        gfx_print(wx+56,wy+143,0x8A9ACE,"Developer Mode");
                        gfx_print(wx+24,wy+170,0x6A7A9E,"KairoOS v1.0 — x86_64");
                        gfx_print(wx+24,wy+210,0x3A4A6A,"[1-4] toggle  [Esc] close");
                    } else if (act == 5) { draw_mac_title("OreoAI Assistant");
                        chat_line_count=0;chat_scroll=0;chat_pos=0;
                        chat_add("[OreoAI v1.0 — Ask me anything!]");
                        chat_add("Try: hello, who are you, help");
                        chat_redraw();
                    } else if (act == 6) { draw_mac_title("Calculator");
                        gfx_fill_round_rect(wx+16,wy+50,ww-32,20,4,0x12124A);gfx_print(wx+24,wy+53,0x4A9EFF,"Calc");
                        gfx_fill_round_rect(wx+40,wy+84,ww-80,32,4,0x000000);gfx_round_rect(wx+40,wy+84,ww-80,32,4,0x2A4A7A);
                        gfx_print(wx+48,wy+92,0x00FF44,"calc N+M");
                        gfx_print(wx+24,wy+130,0x6A7A9E,"Use terminal: calc N+M");
                        gfx_print(wx+24,wy+148,0x6A7A9E,"(press 3 for Terminal)");
                        gfx_print(wx+24,wy+174,0x6A7A9E,"Or type in terminal:");
                        gfx_print(wx+36,wy+192,0x8A8ACC,"calc 12+34  →  46");
                        gfx_print(wx+36,wy+210,0x8A8ACC,"calc 100-27 →  73");
                        gfx_print(wx+24,wy+230,0x3A4A6A,"[Esc] to close");
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
                        studio_mode=0;
                        gfx_fill_round_rect(wx+16,wy+50,ww-32,20,4,0x12124A);
                        gfx_print(wx+24,wy+53,0x6A5ACD,"Kairo Studio  —  ");gfx_print(wx+180,wy+53,0x44AADD,"[XML]");
                        gfx_fill_round_rect(wx+16,wy+76,ww-32,wh-90,4,0x08080E);
                        gfx_print(wx+24,wy+84,0x4488AA,"<");gfx_print(wx+32,wy+84,0x44AADD,"catalog");gfx_print(wx+88,wy+84,0x4488AA,">");
                        gfx_print(wx+40,wy+100,0x4488AA,"<");gfx_print(wx+48,wy+100,0x44AADD,"book");gfx_print(wx+80,wy+100,0xFFFFFF,"id=");gfx_print(wx+100,wy+100,0x44AA44,"\"bk101\"");gfx_print(wx+148,wy+100,0x4488AA,">");
                        gfx_print(wx+56,wy+116,0x4488AA,"<");gfx_print(wx+64,wy+116,0x44AADD,"title");gfx_print(wx+96,wy+116,0x4488AA,">");gfx_print(wx+104,wy+116,0xFFFFFF,"KairoOS");gfx_print(wx+160,wy+116,0x4488AA,"</");gfx_print(wx+176,wy+116,0x44AADD,"title");gfx_print(wx+208,wy+116,0x4488AA,">");
                        gfx_print(wx+56,wy+132,0x4488AA,"<");gfx_print(wx+64,wy+132,0x44AADD,"author");gfx_print(wx+108,wy+132,0x4488AA,">");gfx_print(wx+116,wy+132,0xFFFFFF,"Kairo Dev");gfx_print(wx+184,wy+132,0x4488AA,"</");gfx_print(wx+200,wy+132,0x44AADD,"author");gfx_print(wx+244,wy+132,0x4488AA,">");
                        gfx_print(wx+56,wy+148,0x4488AA,"<");gfx_print(wx+64,wy+148,0x44AADD,"price");gfx_print(wx+96,wy+148,0xFFFFFF,"currency=");gfx_print(wx+152,wy+148,0x44AA44,"\"USD\"");gfx_print(wx+192,wy+148,0x4488AA,">");gfx_print(wx+200,wy+148,0xFFAA44,"29.99");gfx_print(wx+240,wy+148,0x4488AA,"</");gfx_print(wx+256,wy+148,0x44AADD,"price");gfx_print(wx+288,wy+148,0x4488AA,">");
                        gfx_print(wx+24,wy+180,0x3A3A5A,"[1] XML  [2] JS  [Esc] back");
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
