// system_transfer.c — Peer-to-peer file transfer between AnimateOS instances

#define ST_MENU 0
#define ST_SELECT 1
#define ST_RULES 2
#define ST_PRG 3
#define ST_RECEIVE 4
#define ST_DONE 5

#define MAX_ST_FILES 16
#define MAX_ST_FNAME 64

static int st_mode = ST_MENU;
static int st_sel = 0;
static int st_file_idx = 0;
static int st_progress = 0;
static int st_total = 0;
static int st_done_show = 0;
static int st_cancel = 0;

// Local file storage (independent of RAMFS)
static const char *st_fnames[MAX_ST_FILES];
static const unsigned char *st_fdata[MAX_ST_FILES];
static unsigned long st_fsize[MAX_ST_FILES];
static int st_fcount = 0;

// Demo files pre-loaded on first access
static int st_initialized = 0;
static void st_add_file(const char *name, const unsigned char *data, unsigned long size);
static void st_init_demo(void) {
    if (st_initialized) return;
    st_initialized = 1;
    st_add_file("hello.txt", (const unsigned char*)"Hello from AnimateOS!\nSystem Transfer ready.\n", 46);
    st_add_file("readme.md", (const unsigned char*)"# System Transfer\nPeer-to-peer file transfer\nover serial between two\nAnimateOS instances.\n", 93);
    st_add_file("demo.bin", (const unsigned char*)"\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0A\x0B\x0C\x0D\x0E\x0F\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1A\x1B\x1C\x1D\x1E\x1F", 32);
}

static void st_add_file(const char *name, const unsigned char *data, unsigned long size) {
    if (st_fcount >= MAX_ST_FILES) return;
    // Copy name
    char *nm = (char*)kmalloc(MAX_ST_FNAME);
    int i = 0; while (name[i] && i < MAX_ST_FNAME-1) { nm[i] = name[i]; i++; }
    nm[i] = 0;
    // Copy data
    unsigned char *cp = (unsigned char*)kmalloc(size + 1);
    for (unsigned long j = 0; j < size; j++) cp[j] = data[j];
    cp[size] = 0;
    st_fnames[st_fcount] = nm;
    st_fdata[st_fcount] = cp;
    st_fsize[st_fcount] = size;
    st_fcount++;
}

// Protocol
#define STX 0x02
#define ETX 0x03
#define ACK 0x06
#define NAK 0x15

static int st_recv_byte(int t) { char c; return serial_read_timeout(&c, t) ? (unsigned char)c : -1; }

static void st_drain(void) { while (serial_available()) serial_read(); }

static unsigned char st_xor(const unsigned char *d, int n) {
    unsigned char x = 0;
    for (int i = 0; i < n; i++) x ^= d[i];
    return x;
}

// ─── UI Drawing ───

static void st_draw_bg(int w, int h) {
    for (int y = 0; y < h; y += 2) {
        uint32_t c = (y < 100) ? 0x04040E : 0x080818;
        gfx_rect(0, y, w, 1, c);
        gfx_rect(0, y+1, w, 1, 0x0C0C22);
    }
    gfx_rect(0, 0, w, 1, 0x1A2A4A);
    gfx_rect(0, h-1, w, 1, 0x0A0A20);
}

static void st_draw_title(int w, const char *sub) {
    gfx_rect(0, 3, w, 1, 0x2A4A8A);
    gfx_print(w/2-60, 5, 0x4488FF, "System Transfer");
    gfx_rect(0, 22, w, 1, 0x1A1A3A);
    if (sub) { gfx_print(w/2-50, 26, 0x6688AA, sub); gfx_rect(0, 40, w, 1, 0x0E0E28); }
}

void draw_system_transfer(int w, int h, int *close) {
    st_init_demo();
    *close = 0;
    if (st_mode == ST_MENU) {
        st_draw_bg(w, h); st_draw_title(w, 0);
        for (int i = 0; i < 2; i++) {
            int bx = w/2-100, by = 100 + i*60;
            if (i == st_sel) {
                gfx_fill_round_rect(bx-4, by-4, 208, 44, 10, 0x1A2A5A);
                gfx_round_rect(bx-4, by-4, 208, 44, 10, 0x4488FF);
            }
            gfx_fill_round_rect(bx, by, 200, 36, 8, 0x0E0E28);
            gfx_round_rect(bx, by, 200, 36, 8, 0x2A3A5A);
            gfx_print(bx+12, by+8, i==st_sel?0xFFFFFF:0x8899CC, i==0?"Send File":"Receive File");
        }
        gfx_print(w/2-100, h-30, 0x3A4A6A, "[Arrow]  [Enter]  [Esc]");
    }
    else if (st_mode == ST_SELECT) {
        st_draw_bg(w, h); st_draw_title(w, "Select file to send");
        int max_show = (h-90)/22, scroll = (st_sel >= max_show) ? st_sel-max_show+1 : 0;
        for (int i = 0; i < st_fcount && i < max_show; i++) {
            int idx = i+scroll;
            if (idx >= st_fcount) break;
            int iy = 50+i*22;
            uint32_t bg = (idx==st_sel) ? 0x1A2A5A : 0x0A0A20;
            gfx_rect(20, iy, w-40, 20, bg);
            if (idx==st_sel) gfx_rect(20, iy, 3, 20, 0x4488FF);
            char sz[8]; int sp=0; unsigned long s = st_fsize[idx];
            if (s > 1024) { sp += (s/1024 >= 10) ? 2 : 1; sz[0]='0'+s/1024%10; if(s/1024>=10)sz[1]='0'+s/1024/10; }
            else { sz[sp++]='0'+s%10; }
            char _ss[4]; int _si=0;
            if(s>1024){if(s/1024>=10){_ss[_si++]='0'+s/1024/10;}_ss[_si++]='0'+s/1024%10;_ss[_si++]='K';}
            else{_ss[_si++]='0'+s%10;_ss[_si++]='B';}
            _ss[_si]=0;
            gfx_print(28, iy+3, idx==st_sel?0xFFFFFF:0x8899CC, st_fnames[idx]);
            gfx_print(w-60, iy+3, 0x4A6A8A, _ss);
        }
        if (st_fcount == 0) gfx_print(w/2-70, h/2, 0x4A4A6A, "No files to send");
        gfx_print(w/2-100, h-30, 0x3A4A6A, "[Arrow]  [Enter]  [Esc]");
    }
    else if (st_mode == ST_RULES) {
        st_draw_bg(w, h); st_draw_title(w, "Rules");
        const char *r[] = {
            "By using System Transfer you agree:","",
            "  NO adult or inappropriate content",
            "  NO images/videos of sexual nature",
            "  Be patient (~12 KB/s transfer speed)",
            "  Only send what you'd show a child","",
            "Violations = permanent ban"
        };
        int nr = sizeof(r)/sizeof(r[0]);
        for (int i = 0; i < nr; i++) gfx_print(w/2-170, 60+i*18, 0x8899BB, r[i]);
        gfx_fill_round_rect(w/2-80, h-100, 160, 36, 8, 0x1A3A1A);
        gfx_round_rect(w/2-80, h-100, 160, 36, 8, 0x44AA44);
        gfx_print(w/2-44, h-92, 0x88FF88, "Accept");
        gfx_print(w/2-70, h-30, 0x3A4A6A, "[Enter] Accept  [Esc] Back");
    }
    else if (st_mode == ST_PRG) {
        char t[40]; const char *pfx = "Sending:"; int tl=0;
        while(pfx[tl]){t[tl]=pfx[tl];tl++;}
        if (st_file_idx >= 0 && st_file_idx < st_fcount) {
            int fl=0; while(st_fnames[st_file_idx][fl])fl++;
            for(int i=0;i<fl&&tl<38;i++)t[tl++]=st_fnames[st_file_idx][i];
        }
        t[tl]=0;
        st_draw_bg(w, h); st_draw_title(w, t);
        if (st_total > 0) {
            int bw = w-160, bx = 80, by = h/2-10;
            int fill = st_progress * bw / st_total;
            if (fill < 0) fill = 0;
            gfx_fill_round_rect(bx-2, by-2, bw+4, 24, 6, 0x0A0A20);
            if (fill > 0) gfx_fill_round_rect(bx, by, fill, 20, 4, 0x4488FF);
            gfx_round_rect(bx, by, bw, 20, 4, 0x2A3A5A);
            int pct = st_total > 0 ? st_progress*100/st_total : 0;
            char _p[4]; _p[0]='0'+pct/10; _p[1]='0'+pct%10; _p[2]='%'; _p[3]=0;
            gfx_print(bx+bw/2-8, by+3, 0xFFFFFF, _p);
        }
        gfx_print(w/2-60, h-30, 0x4A3A3A, "[Esc] Cancel");
    }
    else if (st_mode == ST_DONE) {
        if (!st_done_show) { draw_system_transfer(w, h, &(int){0}); return; }
        st_draw_bg(w, h);
        st_draw_title(w, st_cancel ? "Cancelled" : "Complete");
        gfx_print(w/2-100, h/2-10, st_cancel?0xFF6666:0x88FF88,
            st_cancel ? "Transfer cancelled" : "Transfer complete!");
        if (!st_cancel && st_file_idx >= 0 && st_file_idx < st_fcount)
            gfx_print(w/2-80, h/2+10, 0xAACCFF, st_fnames[st_file_idx]);
        gfx_print(w/2-60, h-30, 0x3A4A6A, "[Enter] OK");
    }
}

// ─── Transfer Logic ───

void st_start_send(int w, int h) {
    if (st_file_idx < 0 || st_file_idx >= st_fcount) { st_mode = ST_DONE; return; }
    const unsigned char *data = st_fdata[st_file_idx];
    unsigned long size = st_fsize[st_file_idx];
    const char *name = st_fnames[st_file_idx];
    if (!data || !name || size == 0) { st_mode = ST_DONE; return; }

    st_drain();

    // Send header
    serial_write(STX); serial_write('B');
    int nl = 0; while (name[nl]) nl++;
    serial_write(nl & 0xFF); serial_write((nl>>8) & 0xFF);
    for (int i = 0; i < nl; i++) serial_write(name[i]);
    serial_write(size & 0xFF); serial_write((size>>8) & 0xFF);
    serial_write((size>>16) & 0xFF); serial_write((size>>24) & 0xFF);
    unsigned char hx = st_xor((const unsigned char*)name, nl) ^ (size&0xFF) ^ ((size>>8)&0xFF) ^ ((size>>16)&0xFF) ^ ((size>>24)&0xFF);
    serial_write(hx); serial_write(ETX);

    if (st_recv_byte(10000) != ACK) { st_mode = ST_DONE; st_cancel = 1; return; }

    // Send chunks
    int off = 0;
    while (off < (int)size) {
        int len = (size-off > 128) ? 128 : size-off;
        unsigned char xor = st_xor(data+off, len);
        serial_write(STX); serial_write('D');
        serial_write(len & 0xFF); serial_write((len>>8) & 0xFF);
        for (int i = 0; i < len; i++) serial_write(data[off+i]);
        serial_write(xor); serial_write(ETX);
        if (st_recv_byte(10000) != ACK) { st_cancel = 1; break; }
        off += len; st_progress = off; st_total = size;
        if (keyboard_last_char() == 27) { st_cancel = 1; break; }
        draw_system_transfer(w, h, &(int){0});
    }

    // End marker
    serial_write(STX); serial_write('E');
    serial_write(off & 0xFF); serial_write((off>>8) & 0xFF);
    serial_write((off>>16) & 0xFF); serial_write((off>>24) & 0xFF);
    serial_write(ETX);
    st_recv_byte(10000);
    st_progress = off; st_total = size;
    st_done_show = 1;
    st_mode = ST_DONE;
    draw_system_transfer(w, h, &(int){0});
}

void st_start_receive(int w, int h) {
    st_drain();
    st_mode = ST_PRG;
    st_progress = 0; st_total = 100;
    draw_system_transfer(w, h, &(int){0});

    // Wait for STX
    int sync = 0;
    for (int tw = 0; tw < 100000; tw++) {
        if (st_recv_byte(10) == STX) { sync = 1; break; }
        if (keyboard_last_char() == 27) { st_mode = ST_DONE; return; }
    }
    if (!sync) { st_mode = ST_DONE; st_cancel = 1; return; }

    char t = st_recv_byte(5000);
    if (t != 'B') { st_mode = ST_DONE; st_cancel = 1; return; }

    // Read header
    int nl = st_recv_byte(5000); nl |= st_recv_byte(5000) << 8;
    if (nl <= 0 || nl > 120) { st_mode = ST_DONE; st_cancel = 1; return; }
    char rname[128];
    for (int i = 0; i < nl; i++) { int c = st_recv_byte(5000); if (c<0) {st_mode=ST_DONE;return;} rname[i]=c; }
    rname[nl] = 0;

    unsigned long rsize = 0;
    for (int i = 0; i < 4; i++) { int c = st_recv_byte(5000); if (c<0) {st_mode=ST_DONE;return;} rsize |= (unsigned long)c << (i*8); }
    st_recv_byte(5000); st_recv_byte(5000); // xor + ETX
    if (rsize > 512*1024) { serial_write(NAK); st_mode=ST_DONE; st_cancel=1; return; }

    serial_write(ACK);

    // Allocate buffer
    unsigned char *buf = (unsigned char*)kmalloc(rsize+1);
    if (!buf) { serial_write(NAK); st_mode=ST_DONE; return; }

    int roff = 0;
    while (roff < (int)rsize) {
        int b = st_recv_byte(50000);
        if (b != STX) { serial_write(NAK); kfree(buf); st_mode=ST_DONE; st_cancel=1; return; }
        t = st_recv_byte(5000);
        if (t == 'E') { serial_write(ACK); break; }
        if (t != 'D') { serial_write(NAK); kfree(buf); st_mode=ST_DONE; return; }
        int cl = st_recv_byte(5000); cl |= st_recv_byte(5000) << 8;
        for (int i = 0; i < cl; i++) { int c = st_recv_byte(5000); if (c<0) {kfree(buf);st_mode=ST_DONE;return;} buf[roff++]=c; }
        st_recv_byte(5000); st_recv_byte(5000);
        serial_write(ACK);
        st_progress = roff; st_total = rsize;
        if (keyboard_last_char() == 27) { st_cancel=1; serial_write(NAK); kfree(buf); st_mode=ST_DONE; return; }
        draw_system_transfer(w, h, &(int){0});
    }
    buf[roff] = 0;

    // Save to local storage
    st_add_file(rname, buf, roff);
    st_file_idx = st_fcount-1;
    st_progress = roff; st_total = rsize;
    st_done_show = 1; st_cancel = 0;
    st_mode = ST_DONE;
    draw_system_transfer(w, h, &(int){0});
}

void handle_system_transfer_key(int w, int h, char k, int *close) {
    st_init_demo();
    *close = 0;
    if (st_mode == ST_MENU) {
        if (k == KEY_UP || k == KEY_DOWN) { st_sel = 1 - st_sel; draw_system_transfer(w,h,&(int){0}); }
        if (k == '\n' || k == ' ') {
            if (st_sel == 0) { st_mode = ST_SELECT; st_sel = 0; draw_system_transfer(w,h,&(int){0}); }
            else st_start_receive(w, h);
        }
        if (k == 27) *close = 1;
    }
    else if (st_mode == ST_SELECT) {
        if (k == KEY_UP && st_sel > 0) { st_sel--; draw_system_transfer(w,h,&(int){0}); }
        if (k == KEY_DOWN && st_sel < st_fcount-1) { st_sel++; draw_system_transfer(w,h,&(int){0}); }
        if ((k == '\n' || k == ' ') && st_sel >= 0 && st_sel < st_fcount) {
            st_file_idx = st_sel;
            st_mode = ST_RULES;
            draw_system_transfer(w,h,&(int){0});
        }
        if (k == 27) { st_mode = ST_MENU; st_sel = 0; draw_system_transfer(w,h,&(int){0}); }
    }
    else if (st_mode == ST_RULES) {
        if (k == '\n' || k == ' ') { st_mode = ST_PRG; st_progress=0; st_total=st_fsize[st_file_idx]; st_cancel=0; st_start_send(w, h); }
        if (k == 27) { st_mode = ST_SELECT; draw_system_transfer(w,h,&(int){0}); }
    }
    else if (st_mode == ST_DONE || st_mode == ST_PRG) {
        if (k == '\n' || k == ' ' || k == 27) { st_mode = ST_MENU; st_sel = 0; draw_system_transfer(w,h,&(int){0}); }
    }
}
