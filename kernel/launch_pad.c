// launch_pad.c — Xeneva-style app launcher (L key overlay)

#define LPC 4
#define LPR 3
#define LPI 10

static int lw, lh;

// Exposed for click/keyboard handling in kernel_main
int lpg_x[LPI], lpg_y[LPI], lpg_cw, lpg_ch;
int lpg_actions[LPI] = { 3, 6, 23, 12, 4, 16, 8, 1, 14, 25 };
const char *lpg_names[LPI] = {
    "Xeneva Terminal", "Calculator", "Files", "Xeneva Player",
    "Controls", "Calendar", "Store", "Xeneva",
    "Glimpse", "Doom"
};

static void fill_tri(int x1, int y1, int x2, int y2, int x3, int y3, uint32_t c) {
    if (y1 > y2) { int t; t=x1;x1=x2;x2=t; t=y1;y1=y2;y2=t; }
    if (y2 > y3) { int t; t=x2;x2=x3;x3=t; t=y2;y2=y3;y3=t; }
    if (y1 > y2) { int t; t=x1;x1=x2;x2=t; t=y1;y1=y2;y2=t; }
    for (int y = y1; y <= y3; y++) {
        int xa, xb;
        if (y < y2) { xa = x1+(x2-x1)*(y-y1)/(y2-y1+1); xb = x1+(x3-x1)*(y-y1)/(y3-y1+1); }
        else { xa = x2+(x3-x2)*(y-y2)/(y3-y2+1); xb = x1+(x3-x1)*(y-y1)/(y3-y1+1); }
        if (xa > xb) { int t = xa; xa = xb; xb = t; }
        if (xb >= xa) gfx_rect(xa, y, xb-xa+1, 1, c);
    }
}

static void lp_draw_icon(int idx, int cx, int cy, int sz) {
    switch (idx) {
    case 0: // Terminal: terminal window with green prompt
        gfx_fill_round_rect(cx, cy, sz, sz, sz/5, 0x2D2D3F);
        gfx_round_rect(cx, cy, sz, sz, sz/5, 0x5A5A7A);
        gfx_rect(cx+2, cy+6, sz-4, 1, 0x4A4A6A);
        gfx_fill_round_rect(cx+4, cy+2, 8, 4, 2, 0xFF6644);
        gfx_fill_round_rect(cx+13, cy+2, 8, 4, 2, 0x44CC44);
        gfx_fill_round_rect(cx+22, cy+2, 8, 4, 2, 0x4488FF);
        gfx_print(cx+5, cy+sz/2-4, 0x44FF66, "$");
        gfx_print(cx+15, cy+sz/2-4, 0x8899CC, "~");
        gfx_print(cx+5, cy+sz/2+10, 0x66AAFF, ">_");
        break;
    case 1: // Calculator: realistic calculator with display
        gfx_fill_round_rect(cx, cy, sz, sz, sz/5, 0x1A1A2A);
        gfx_round_rect(cx, cy, sz, sz, sz/5, 0x6A6A7A);
        gfx_fill_round_rect(cx+4, cy+4, sz-8, sz/4-2, 3, 0x0A0A1A);
        gfx_print(cx+sz-20, cy+8, 0x66FF66, "42");
        int bx = cx+4, by = cy+sz/4+4, bw = (sz-12)/4, bh = (sz*3/4-8)/5;
        int bv[] = {7,8,9,'/',4,5,6,'x',1,2,3,'-',0,'.','=','+'};
        for(int i=0;i<16;i++){
            int br = i/4, bc = i%4;
            int x=bx+bc*(bw+1), y=by+br*(bh+1);
            gfx_fill_round_rect(x,y,bw,bh,3,0x2A2A3A);
            gfx_round_rect(x,y,bw,bh,3,0x4A4A5A);
            char _b[2]={bv[i],0};
            gfx_print(x+bw/2-3, y+bh/2-5, 0xCCCCDD, _b);
        }
        break;
    case 2: // Files: folder with doc peeking out
        gfx_fill_round_rect(cx+4, cy+sz/2-6, sz-8, sz/2+6, 6, 0x7799BB);
        gfx_fill_round_rect(cx+4, cy+sz/2-16, sz*2/5, 16, 5, 0x7799BB);
        gfx_fill_round_rect(cx+2, cy+sz/2-12, sz-4, sz/2+12, 6, 0xCCEEFF);
        gfx_round_rect(cx+2, cy+sz/2-12, sz-4, sz/2+12, 6, 0x5599CC);
        gfx_fill_round_rect(cx+sz/2-4, cy+2, sz/2-6, sz/3+4, 4, 0xFFFFFF);
        gfx_round_rect(cx+sz/2-4, cy+2, sz/2-6, sz/3+4, 4, 0x5588AA);
        gfx_rect(cx+sz/2+2, cy+8, sz/2-14, 1, 0x5588AA);
        gfx_rect(cx+sz/2+2, cy+12, sz/2-18, 1, 0x5588AA);
        gfx_fill_round_rect(cx+sz/2+2, cy+6, 6, 6, 3, 0xFFCC44);
        break;
    case 3: // Player: play button with sound wave
        gfx_fill_round_rect(cx-3, cy-3, sz+6, sz+6, sz/2+3, 0x111111);
        gfx_fill_round_rect(cx, cy, sz, sz, sz/2, 0xFFFFFF);
        fill_tri(cx+sz/2-8, cy+10, cx+sz/2-8, cy+sz-10, cx+sz/2+14, cy+sz/2, 0xDD3366);
        fill_tri(cx+sz/2-6, cy+12, cx+sz/2-6, cy+sz-12, cx+sz/2+10, cy+sz/2, 0xFF5588);
        // Sound waves
        gfx_round_rect(cx+sz/2+16, cy+sz/2-10, 6, 20, 3, 0xFF6688);
        gfx_round_rect(cx+sz/2+24, cy+sz/2-14, 6, 28, 3, 0xFF4466);
        gfx_round_rect(cx+sz/2+32, cy+sz/2-18, 6, 36, 3, 0xFF2244);
        break;
    case 4: // Controls: gear with better teeth
        gfx_fill_round_rect(cx, cy, sz, sz, sz/5, 0x3A3A4A);
        gfx_round_rect(cx, cy, sz, sz, sz/5, 0x8A8A9A);
        { int gcx=cx+sz/2, gcy=cy+sz/2;
          gfx_fill_round_rect(gcx-12, gcy-12, 24, 24, 12, 0xAAAABB);
          gfx_fill_round_rect(gcx-8, gcy-8, 16, 16, 8, 0x8A8A9A);
          gfx_fill_round_rect(gcx-14, gcy-4, 28, 8, 4, 0xAAAABB);
          gfx_fill_round_rect(gcx-4, gcy-14, 8, 28, 4, 0xAAAABB);
          gfx_fill_round_rect(gcx-12, gcy-3, 24, 6, 3, 0xAAAABB);
          gfx_fill_round_rect(gcx-3, gcy-12, 6, 24, 3, 0xAAAABB);
          gfx_fill_round_rect(gcx-6, gcy-6, 12, 12, 6, 0x3A3A4A);
          gfx_fill_round_rect(gcx-2, gcy-2, 4, 4, 2, 0x6A6A7A); }
        break;
    case 5: // Calendar: realistic calendar with date
        gfx_fill_round_rect(cx, cy, sz, sz, sz/6, 0xE06644);
        gfx_round_rect(cx, cy, sz, sz, sz/6, 0xFF8866);
        gfx_fill_round_rect(cx+2, cy+4, sz-4, sz/3+2, 3, 0xFFAA88);
        char _d[]="JUL";
        gfx_print(cx+sz/2-10, cy+8, 0xFFFFFF, _d);
        gfx_fill_round_rect(cx+4, cy+sz/3+2, sz-8, sz*2/3-4, 3, 0xFFFFFF);
        gfx_rect(cx+4, cy+sz/3+sz/6+2, sz-8, 1, 0xDDDDDD);
        gfx_rect(cx+sz/3+4, cy+sz/3+2, 1, sz*2/3-4, 0xDDDDDD);
        gfx_rect(cx+sz*2/3+4, cy+sz/3+2, 1, sz*2/3-4, 0xDDDDDD);
        char _n[]="10";
        gfx_print(cx+sz/2-6, cy+sz/3+sz/6+2, 0x222222, _n);
        break;
    case 6: // Store: shopping bag with storefront
        gfx_fill_round_rect(cx, cy, sz, sz, sz/5, 0x4488FF);
        gfx_round_rect(cx, cy, sz, sz, sz/5, 0x77BBFF);
        fill_tri(cx+8, cy+8, cx+sz-6, cy+sz/2, cx+8, cy+sz-8, 0xFFFFFF);
        fill_tri(cx+sz/3+2, cy+14, cx+sz-14, cy+sz/2, cx+sz/3+2, cy+sz-14, 0xCCEEFF);
        gfx_fill_round_rect(cx+3, cy+3, 6, 6, 3, 0xFFDD44);
        gfx_fill_round_rect(cx+sz-9, cy+3, 6, 6, 3, 0x44DD88);
        break;
    case 7: // Xeneva: refined wing logo on dark bg
        gfx_fill_round_rect(cx, cy, sz, sz, sz/5, 0x1A1A2A);
        gfx_round_rect(cx, cy, sz, sz, sz/5, 0x5A5A6A);
        { int mx=cx+sz/2, my=cy+sz/2;
          fill_tri(mx-12,my+4, mx-28,my-26, mx+8,my+4, 0x7A5ACD);
          fill_tri(mx-12,my+4, mx-28,my+26, mx+8,my+4, 0x7A5ACD);
          fill_tri(mx+12,my+4, mx+30,my-26, mx-8,my+4, 0x7A5ACD);
          fill_tri(mx+12,my+4, mx+30,my+26, mx-8,my+4, 0x7A5ACD);
          gfx_fill_round_rect(mx-6,my-2,12,8,4,0xFFFFFF);
          gfx_fill_round_rect(mx-3,my+4,6,4,2,0xFFFFFF);
          gfx_print(mx-4,my+12,0x7A5ACD,"X"); }
        break;
    case 8: // Glimpse: overlapping frames with film strip
        gfx_fill_round_rect(cx+10, cy+2, sz-14, sz-6, 5, 0xCC6622);
        gfx_fill_round_rect(cx+2, cy+8, sz-14, sz-6, 5, 0x44BBEE);
        gfx_fill_round_rect(cx+6, cy+5, sz-14, sz-6, 5, 0xEE8844);
        fill_tri(cx+28,cy+24, cx+20,cy+sz-8, cx+36,cy+sz-8, 0x1A1A3A);
        fill_tri(cx+42,cy+20, cx+34,cy+sz-8, cx+50,cy+sz-8, 0x0C0C28);
        gfx_round_rect(cx+10, cy+2, sz-14, sz-6, 5, 0xBB4400);
        gfx_round_rect(cx+2, cy+8, sz-14, sz-6, 5, 0x2288BB);
        gfx_round_rect(cx+6, cy+5, sz-14, sz-6, 5, 0xDD6622);
        // Film dots
        for(int _fd=0;_fd<5;_fd++){
            gfx_fill_round_rect(cx+8+_fd*12,cy+sz-8,4,4,2,0xFFDD44);
        }
        break;
    case 9: // Doom: angry skull face
        gfx_fill_round_rect(cx, cy, sz, sz, sz/5, 0x0C1C0C);
        gfx_round_rect(cx, cy, sz, sz, sz/5, 0x66CC66);
        { int mx=cx+sz/2, my=cy+sz/2-4;
          gfx_fill_round_rect(mx-18,my-12,36,28,6,0x44AA44);
          gfx_fill_round_rect(mx-12,my-16,24,8,4,0x44AA44);
          gfx_fill_round_rect(mx-14,my-18,6,6,3,0xFFFFFF);
          gfx_fill_round_rect(mx+8,my-18,6,6,3,0xFFFFFF);
          gfx_fill_round_rect(mx-12,my-16,4,4,2,0x000000);
          gfx_fill_round_rect(mx+8,my-16,4,4,2,0x000000);
          gfx_fill_round_rect(mx-8,my+4,16,6,3,0x000000);
          gfx_rect(mx-8,my+8,16,1,0xFF4444);
          gfx_fill_round_rect(mx-4,my+10,8,4,2,0xFF4444);
          gfx_fill_round_rect(mx-8,my-6,4,3,1,0xFF4444);
          gfx_fill_round_rect(mx+4,my-6,4,3,1,0xFF4444);
        }
        break;
    }
}

void draw_launch_pad(int scr_w, int scr_h, int sel_x, int sel_y) {
    lw = scr_w; lh = scr_h;

    // Multi-band dimming overlay (simulated blur)
    int _bands[] = {0x020208, 0x030310, 0x040412, 0x030310, 0x020208, 0x040414, 0x02020A};
    int _nbands = sizeof(_bands)/sizeof(_bands[0]);
    int _bh = scr_h / _nbands;
    for (int i = 0; i < _nbands; i++) {
        uint32_t c = _bands[i];
        int y0 = i * _bh;
        int y1 = (i == _nbands-1) ? scr_h : (i+1) * _bh;
        // Light edge feathering
        gfx_rect(0, y0, scr_w, 1, (c & 0xF0F0F0) >> 4);
        gfx_rect(0, y1-1, scr_w, 1, (c & 0xF0F0F0) >> 4);
        for (int y = y0+1; y < y1-1; y++)
            gfx_rect(0, y, scr_w, 1, c);
    }
    // Stronger vignette (darker corners)
    for (int i = 0; i < 60; i++) {
        int a = (60-i) * 3;
        if (a > 48) a = 48;
        uint32_t c = (a<<16)|(a<<8)|a;
        gfx_rect(i, 0, 1, scr_h, c);
        gfx_rect(scr_w-1-i, 0, 1, scr_h, c);
    }
    for (int i = 0; i < 30; i++) {
        int a = (30-i) * 4;
        if (a > 36) a = 36;
        uint32_t c = (a<<16)|(a<<8)|a;
        gfx_rect(0, i, scr_w, 1, c);
        gfx_rect(0, scr_h-1-i, scr_w, 1, c);
    }

    // Top bar with glass-like line
    gfx_rect(0, 0, scr_w, 1, 0x0E0E28);
    gfx_rect(0, 30, scr_w, 1, 0x0A0A20);
    gfx_rect(0, 0, scr_w, 1, 0x141438);

    // Title
    gfx_print(10, 8, 0x5A7AAA, "Launchpad");

    // Search bar (macOS-style, pill with glass)
    int sb_w = 280, sb_h = 26;
    int sb_x = (scr_w - sb_w) / 2;
    gfx_fill_round_rect(sb_x, 2, sb_w, sb_h, sb_h/2, 0x0C0C28);
    gfx_round_rect(sb_x, 2, sb_w, sb_h, sb_h/2, 0x3A5A8A);
    gfx_round_rect(sb_x+1, 3, sb_w-2, sb_h-2, sb_h/2-1, 0x5A7AAA);
    // Glass highlight
    gfx_rect(sb_x+12, 4, sb_w-24, 1, 0x1A1A3A);
    gfx_print(sb_x+14, 7, 0x4A7A9A, "Search Apps..");
    // Search icon
    gfx_round_rect(sb_x+sb_w-22, 5, 16, 16, 8, 0x4A6A8A);
    gfx_rect(sb_x+sb_w-16, 14, 6, 6, 0x4A6A8A);

    // Power button
    gfx_fill_round_rect(scr_w-40, 3, 24, 24, 12, 0x331111);
    gfx_round_rect(scr_w-40, 3, 24, 24, 12, 0xFF4444);
    gfx_rect(scr_w-30, 8, 4, 10, 0xFF6666);
    gfx_fill_round_rect(scr_w-33, 20, 10, 4, 2, 0xFF6666);

    // Restart button
    gfx_fill_round_rect(scr_w-70, 3, 24, 24, 12, 0x0E1E3E);
    gfx_round_rect(scr_w-70, 3, 24, 24, 12, 0x4488FF);
    gfx_fill_round_rect(scr_w-66, 8, 16, 14, 7, 0x4488FF);
    gfx_rect(scr_w-62, 6, 8, 6, 0x0E1E3E);
    gfx_fill_round_rect(scr_w-62, 18, 8, 4, 2, 0x4488FF);

    // Grid of apps
    int pad_x = 70, pad_y = 55;
    int g_w = scr_w - pad_x * 2;
    int g_h = scr_h - pad_y - 50;
    lpg_cw = g_w / LPC;
    lpg_ch = g_h / LPR;
    int isz = 80;

    for (int r = 0; r < LPR; r++) {
        for (int c = 0; c < LPC; c++) {
            int idx = r * LPC + c;
            if (idx >= LPI) continue;

            int cx = pad_x + c * lpg_cw + (lpg_cw - isz) / 2;
            int cy = pad_y + r * lpg_ch + 8;

            lpg_x[idx] = cx;
            lpg_y[idx] = cy;

            // Selection glow (expanded, multi-ring)
            if (idx == sel_y * LPC + sel_x) {
                gfx_fill_round_rect(cx-10, cy-8, isz+20, lpg_ch-4, 12, 0x162242);
                gfx_round_rect(cx-10, cy-8, isz+20, lpg_ch-4, 12, 0x3366AA);
                gfx_round_rect(cx-7, cy-5, isz+14, lpg_ch-10, 9, 0x5599FF);
                gfx_round_rect(cx-4, cy-2, isz+8, lpg_ch-16, 6, 0x77BBFF);
            }

            // Icon shadow
            gfx_fill_round_rect(cx+4, cy+5, isz, isz, isz/5, 0x000000);
            gfx_fill_round_rect(cx+2, cy+2, isz, isz, isz/5, 0x080808);
            lp_draw_icon(idx, cx, cy, isz);

            // Label centered below icon
            int ln = 0;
            while (lpg_names[idx][ln]) ln++;
            int lx = pad_x + c * lpg_cw + (lpg_cw - ln * 8) / 2;
            uint32_t lc = (idx == sel_y * LPC + sel_x) ? 0xFFFFFF : 0x8899BB;
            gfx_print(lx-1, cy + isz + 8, 0x000000, lpg_names[idx]);
            gfx_print(lx, cy + isz + 9, lc, lpg_names[idx]);
        }
    }

    // Nav arrows (subtle, modern)
    int ax = scr_w - 32, ay = scr_h / 2 - 32;
    for (int a = 0; a < 2; a++) {
        int ay2 = ay + a * 44;
        gfx_round_rect(ax, ay2, 24, 24, 12, 0x1A2A3A);
        gfx_round_rect(ax+1, ay2+1, 22, 22, 11, 0x3A5A7A);
        int cx_a = ax + 12, cy_a = ay2 + (a == 0 ? 8 : 16);
        uint32_t ac = 0x5A7A9A;
        if (a == 0) {
            fill_tri(cx_a, cy_a-2, cx_a-7, cy_a+6, cx_a+7, cy_a+6, ac);
        } else {
            fill_tri(cx_a, cy_a+2, cx_a-7, cy_a-6, cx_a+7, cy_a-6, ac);
        }
    }

    // Hint
    gfx_print(scr_w/2-160, scr_h-20, 0x2A3A5A,
        "[Arrow keys]  [Enter] open  [Esc] close");
    gfx_print(scr_w/2-160, scr_h-28, 0x4A5A7A,
        "Press  L  anytime  to  reopen");
}
