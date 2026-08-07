#include "render3d.h"
#include "gfx.h"
#include <stdint.h>

#define FAR 20.0f
#define NEAR 0.3f

static float *zbuf = 0;
static int zbw = 0, zbh = 0;
static volatile uint32_t *fb = 0;
static int fbw = 0, fbh = 0;
static int vp_x = 0, vp_y = 0, vp_w = 0, vp_h = 0;

#define STAB 256
static int16_t sin_tab[STAB];
static int sin_init = 0;
static int _tick = 0;

#define SIN(x) (sin_tab[((int)((x)*(STAB/6.2831853f))&(STAB-1))]/32767.0f)
#define COS(x) (sin_tab[((int)(((x)+1.5707963f)*(STAB/6.2831853f))&(STAB-1))]/32767.0f)

static void init_trig(void) {
    if (sin_init) return;
    sin_init = 1;
    for (int i = 0; i < STAB; i++) {
        float a = 6.2831853f * i / STAB;
        float x = a;
        float x2 = x*x, x3 = x2*x, x5 = x3*x2;
        float s = x - x3/6.0f + x5/120.0f;
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        sin_tab[i] = (int16_t)(s * 32767.0f);
    }
}

void r3d_init(int w, int h) {
    fbw = w; fbh = h;
    init_trig();
    zbw = w / 2; zbh = h / 2;
    static float zb_mem[207*448];
    zbuf = zb_mem;
}

void r3d_set_viewport(int x, int y, int w, int h) {
    vp_x = x; vp_y = y; vp_w = w; vp_h = h;
}

void r3d_tick(void) { _tick++; }

void r3d_begin_frame(void) {
    fb = (volatile uint32_t*)gfx_get_fb_addr();
    int total = zbw * zbh;
    for (int i = 0; i < total; i++) zbuf[i] = FAR;
}

static void proj(int *sx, int *sy, float *sz, vec3f *v, camera_t *cam) {
    float dx = v->x - cam->pos.x;
    float dy = v->y - cam->pos.y;
    float dz = v->z - cam->pos.z;

    float cy = COS(cam->yaw), sy_v = SIN(cam->yaw);
    float cp = COS(cam->pitch), sp = SIN(cam->pitch);

    float x = dx*cy - dz*sy_v;
    float z = dx*sy_v + dz*cy;
    float y = dy*cp - z*sp;
    z = dy*sp + z*cp;

    if (z < 0.3f) { *sx = -9999; return; }
    float sc = (float)fbw * 0.7f / z;
    *sx = (int)(x * sc + fbw/2);
    *sy = (int)(-y * sc + fbh/2);
    *sz = z;
}

static void tri(vec3f *v0, vec3f *v1, vec3f *v2, uint32_t col, camera_t *cam) {
    int x0, y0, x1, y1, x2, y2;
    float z0, z1, z2;
    proj(&x0, &y0, &z0, v0, cam);
    proj(&x1, &y1, &z1, v1, cam);
    proj(&x2, &y2, &z2, v2, cam);
    if (x0<-5000 || x1<-5000 || x2<-5000) return;

    int ax = x1-x0, ay = y1-y0;
    int bx = x2-x0, by = y2-y0;
    if (ax*by - ay*bx < 0) return;

    int minx = x0, maxx = x0;
    if (x1 < minx) minx=x1; if (x2 < minx) minx=x2;
    if (x1 > maxx) maxx=x1; if (x2 > maxx) maxx=x2;
    int miny = y0, maxy = y0;
    if (y1 < miny) miny=y1; if (y2 < miny) miny=y2;
    if (y1 > maxy) maxy=y1; if (y2 > maxy) maxy=y2;

    int clip_x0 = vp_w ? vp_x : 0;
    int clip_y0 = vp_h ? vp_y : 0;
    int clip_x1 = vp_w ? vp_x + vp_w : fbw;
    int clip_y1 = vp_h ? vp_y + vp_h : fbh;
    if (maxx < clip_x0 || minx >= clip_x1 || maxy < clip_y0 || miny >= clip_y1) return;
    if (minx < clip_x0) minx=clip_x0; if (maxx >= clip_x1) maxx=clip_x1-1;
    if (miny < clip_y0) miny=clip_y0; if (maxy >= clip_y1) maxy=clip_y1-1;

    int c1 = x0*(y1-y2) + x1*(y2-y0) + x2*(y0-y1);
    if (c1 == 0) return;
    float c1f = 1.0f / c1;

    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            int w0 = (y1 - y2) * x + (x2 - x1) * y + x1*y2 - x2*y1;
            int w1 = (y2 - y0) * x + (x0 - x2) * y + x2*y0 - x0*y2;
            int w2 = (y0 - y1) * x + (x1 - x0) * y + x0*y1 - x1*y0;
            if (w0 >= 0 && w1 >= 0 && w2 >= 0) {
                float z = w0 * c1f * z0 + w1 * c1f * z1 + w2 * c1f * z2;
                int zix = (x>>1) + (y>>1)*zbw;
                if (z < zbuf[zix] && z > NEAR) {
                    zbuf[zix] = z;
                    float att = 1.0f - z/FAR;
                    if (att < 0.2f) att = 0.2f;
                    if (att > 1.0f) att = 1.0f;
                    int r = (int)(((col>>16)&0xFF) * att);
                    int g = (int)(((col>>8)&0xFF) * att);
                    int bv = (int)((col&0xFF) * att);
                    fb[y*fbw+x] = (r<<16)|(g<<8)|bv;
                }
            }
        }
    }
}

static void draw_face(vec3f *a, vec3f *b, vec3f *c, vec3f *d, uint32_t col, camera_t *cam) {
    tri(a, b, c, col, cam);
    tri(a, c, d, col, cam);
}

static void draw_box(float cx, float cy, float cz, float sw, float sh, float sd,
                     uint32_t col_top, uint32_t col_bottom, uint32_t col_side, camera_t *cam) {
    float hsw = sw/2, hsh = sh/2, hsd = sd/2;
    float x0 = cx - hsw, x1 = cx + hsw;
    float y0 = cy - hsh, y1 = cy + hsh;
    float z0 = cz - hsd, z1 = cz + hsd;
    vec3f v[8];
    v[0] = (vec3f){x0, y0, z0}; v[1] = (vec3f){x1, y0, z0};
    v[2] = (vec3f){x1, y1, z0}; v[3] = (vec3f){x0, y1, z0};
    v[4] = (vec3f){x0, y0, z1}; v[5] = (vec3f){x1, y0, z1};
    v[6] = (vec3f){x1, y1, z1}; v[7] = (vec3f){x0, y1, z1};

    int dr = ((col_side>>16)&0xFF)*7/10; if(dr>255)dr=255;
    int dg = ((col_side>>8)&0xFF)*7/10; if(dg>255)dg=255;
    int db = (col_side&0xFF)*7/10; if(db>255)db=255;
    uint32_t col_dark = (dr<<16)|(dg<<8)|db;

    int lr = ((col_side>>16)&0xFF)*10/10;
    int lg = ((col_side>>8)&0xFF)*10/10;
    int lb = (col_side&0xFF)*10/10;
    uint32_t col_light = (lr<<16)|(lg<<8)|lb;

    draw_face(&v[3], &v[2], &v[6], &v[7], col_top, cam);
    draw_face(&v[0], &v[1], &v[5], &v[4], col_bottom, cam);
    draw_face(&v[0], &v[3], &v[7], &v[4], col_dark, cam);
    draw_face(&v[1], &v[2], &v[6], &v[5], col_light, cam);
    draw_face(&v[4], &v[5], &v[6], &v[7], col_side, cam);
    draw_face(&v[0], &v[1], &v[2], &v[3], col_dark, cam);
}

void r3d_draw(camera_t *cam) {
    int vx = vp_x, vy = vp_y, vw = vp_w ? vp_w : fbw, vh = vp_h ? vp_h : fbh;

    r3d_begin_frame();

    for (int y = vy; y < vy+vh; y++) {
        float t = (float)(y - vy) / vh;
        int r = (int)(20 + t * 15);
        int g = (int)(15 + t * 12);
        int b = (int)(25 + t * 20);
        if (r>255)r=255; if(g>255)g=255; if(b>255)b=255;
        uint32_t bg = (r<<16)|(g<<8)|b;
        for (int x = vx; x < vx+vw; x++) fb[y*fbw+x] = bg;
    }

    float room_w = 8.0f, room_h = 3.0f, room_d = 6.0f;
    float hw = room_w/2, hh = room_h/2, hd = room_d/2;

    draw_box(0, 0.015f, 0, room_w, 0.03f, room_d, 0x000000, 0x000000, 0x8B6B3D, cam);
    draw_box(0, room_h, 0, room_w, 0.05f, room_d, 0x000000, 0x000000, 0xF0F0E8, cam);
    draw_box(0, room_h/2, hd-0.02f, room_w, room_h, 0.04f, 0xE8D5B7, 0xE8D5B7, 0xE8D5B7, cam);
    draw_box(0, room_h/2, -(hd-0.02f), room_w, room_h, 0.04f, 0xDCC8A8, 0xDCC8A8, 0xDCC8A8, cam);
    draw_box(-(hw-0.02f), room_h/2, 0, 0.04f, room_h, room_d, 0xD0BFA0, 0xD0BFA0, 0xD0BFA0, cam);
    draw_box(hw-0.02f, room_h/2, 0, 0.04f, room_h, room_d, 0xD0BFA0, 0xD0BFA0, 0xD0BFA0, cam);

    float wx = 0.0f, wy = 1.5f, wz = hd - 0.05f;
    float ww = 1.8f, wh = 1.2f;
    vec3f wv[4] = {
        {wx-ww/2, wy-wh/2, wz}, {wx+ww/2, wy-wh/2, wz},
        {wx+ww/2, wy+wh/2, wz}, {wx-ww/2, wy+wh/2, wz}
    };
    draw_face(&wv[0], &wv[1], &wv[2], &wv[3], 0x87CEEB, cam);

    draw_box(wx, wy, wz+0.05f, ww+0.2f, 0.06f, 0.04f, 0x5C3A1E, 0x5C3A1E, 0x5C3A1E, cam);
    draw_box(wx, wy-wh/2-0.03f, wz+0.05f, ww+0.2f, 0.06f, 0.04f, 0x5C3A1E, 0x5C3A1E, 0x5C3A1E, cam);
    draw_box(wx-ww/2-0.1f, wy, wz+0.05f, 0.06f, wh+0.06f, 0.04f, 0x5C3A1E, 0x5C3A1E, 0x5C3A1E, cam);
    draw_box(wx+ww/2+0.1f, wy, wz+0.05f, 0.06f, wh+0.06f, 0.04f, 0x5C3A1E, 0x5C3A1E, 0x5C3A1E, cam);
    draw_box(wx, wy, wz+0.07f, ww+0.3f, 0.04f, 0.03f, 0x4A2E14, 0x4A2E14, 0x4A2E14, cam);

    float dx = -(hw - 0.06f), dy = 1.1f, dz = 0.0f;
    float dw = 0.06f, dh = 2.0f, dd = 0.8f;
    draw_box(dx, dy, dz, dw, dh, dd, 0x6B3A1F, 0x6B3A1F, 0x6B3A1F, cam);
    draw_box(dx-0.01f, 0.6f, dz-0.35f, 0.04f, 0.3f, 0.1f, 0x4A2E14, 0x4A2E14, 0x4A2E14, cam);
    draw_box(dx-0.01f, 1.6f, dz-0.35f, 0.04f, 0.3f, 0.1f, 0x4A2E14, 0x4A2E14, 0x4A2E14, cam);

    float bfx = 2.0f, bfy = 0.2f, bfz = 1.8f;
    draw_box(bfx, bfy, bfz, 2.0f, 0.4f, 2.2f, 0x5C3A1E, 0x3A2010, 0x5C3A1E, cam);
    draw_box(bfx, 0.6f, bfz, 1.8f, 0.3f, 2.0f, 0xF5F5F0, 0xE0E0D8, 0xF0F0E8, cam);
    draw_box(bfx-0.5f, 0.85f, bfz+0.6f, 0.5f, 0.12f, 0.4f, 0xFFFFFF, 0xF0F0F0, 0xFFFFFF, cam);
    draw_box(bfx+0.4f, 0.85f, bfz-0.1f, 0.5f, 0.08f, 1.4f, 0x4488CC, 0x336699, 0x4477BB, cam);

    float dkx = -1.5f, dky = 0.5f, dkz = 1.2f;
    draw_box(dkx, dky, dkz, 1.6f, 0.06f, 0.9f, 0x8B6914, 0x6B4E0F, 0x8B6914, cam);
    draw_box(dkx-0.7f, 0.2f, dkz-0.35f, 0.08f, 0.6f, 0.08f, 0x5C3A1E, 0x3A2010, 0x5C3A1E, cam);
    draw_box(dkx+0.7f, 0.2f, dkz-0.35f, 0.08f, 0.6f, 0.08f, 0x5C3A1E, 0x3A2010, 0x5C3A1E, cam);
    draw_box(dkx-0.7f, 0.2f, dkz+0.35f, 0.08f, 0.6f, 0.08f, 0x5C3A1E, 0x3A2010, 0x5C3A1E, cam);
    draw_box(dkx+0.7f, 0.2f, dkz+0.35f, 0.08f, 0.6f, 0.08f, 0x5C3A1E, 0x3A2010, 0x5C3A1E, cam);

    draw_box(dkx+0.2f, dky+0.6f, dkz-0.2f, 0.3f, 0.5f, 0.3f, 0x888888, 0x555555, 0x777777, cam);

    draw_box(dkx+0.05f, dky+0.45f, dkz-0.05f, 0.12f, 0.3f, 0.12f, 0xFFFF88, 0xCCAA44, 0xEECC66, cam);

    float sfx = -(hw - 0.1f), sfy = 0.9f, sfz = -1.4f;
    draw_box(sfx, sfy, sfz, 0.1f, 1.8f, 0.8f, 0x6B3A1F, 0x3A2010, 0x6B3A1F, cam);
    draw_box(sfx, sfy-0.9f, sfz-0.1f, 0.08f, 0.08f, 1.0f, 0x5C3A1E, 0x3A2010, 0x5C3A1E, cam);
    draw_box(sfx, sfy+0.9f, sfz-0.1f, 0.08f, 0.08f, 1.0f, 0x5C3A1E, 0x3A2010, 0x5C3A1E, cam);
    draw_box(sfx+0.05f, sfy+0.1f, sfz+0.5f, 0.04f, 0.08f, 0.15f, 0xFF4444, 0xCC3333, 0xFF4444, cam);
    draw_box(sfx+0.05f, sfy-0.2f, sfz+0.5f, 0.04f, 0.08f, 0.15f, 0x44FF44, 0x33CC33, 0x44FF44, cam);
    draw_box(sfx+0.05f, sfy-0.5f, sfz+0.5f, 0.04f, 0.08f, 0.15f, 0x4488FF, 0x3366CC, 0x4488FF, cam);
    draw_box(sfx+0.05f, sfy+0.1f, sfz-0.3f, 0.04f, 0.08f, 0.15f, 0xFFAA44, 0xCC8833, 0xFFAA44, cam);
    draw_box(sfx+0.05f, sfy-0.2f, sfz-0.3f, 0.04f, 0.08f, 0.15f, 0xDD44AA, 0xAA3388, 0xDD44AA, cam);
    draw_box(sfx+0.05f, sfy-0.5f, sfz-0.3f, 0.04f, 0.08f, 0.15f, 0x44DD88, 0x33AA66, 0x44DD88, cam);

    float rgx = 0.0f, rgy = 0.025f, rgz = -0.3f;
    draw_box(rgx, rgy, rgz, 2.5f, 0.05f, 2.0f, 0x8B2500, 0x000000, 0x8B2500, cam);
    draw_box(rgx, rgy+0.02f, rgz, 2.0f, 0.02f, 1.5f, 0xAA3000, 0x000000, 0xAA3000, cam);

    float pstx = hw - 0.06f, psty = 1.5f, pstz = -0.3f;
    draw_box(pstx, psty, pstz, 0.04f, 0.6f, 0.4f, 0xFF6644, 0xCC4422, 0xFF6644, cam);
    draw_box(pstx, psty-0.35f, pstz, 0.04f, 0.04f, 0.44f, 0x5C3A1E, 0x3A2010, 0x5C3A1E, cam);
    draw_box(pstx, psty+0.35f, pstz, 0.04f, 0.04f, 0.44f, 0x5C3A1E, 0x3A2010, 0x5C3A1E, cam);

    float chx = dkx + 0.8f, chy = 0.2f, chz = dkz + 0.5f;
    draw_box(chx, chy, chz, 0.5f, 0.4f, 0.5f, 0x6B3A1F, 0x3A2010, 0x6B3A1F, cam);
    draw_box(chx, chy+0.5f, chz, 0.5f, 0.15f, 0.5f, 0x8B6914, 0x6B4E0F, 0x8B6914, cam);
    draw_box(chx, 0.4f, chz, 0.5f, 0.08f, 0.5f, 0x444444, 0x333333, 0x444444, cam);

    draw_box(-hw+0.3f, 0.3f, 1.0f, 0.4f, 0.1f, 0.3f, 0x888888, 0x555555, 0x777777, cam);
    draw_box(-hw+0.5f, 0.3f, 1.2f, 0.3f, 0.1f, 0.3f, 0x888888, 0x555555, 0x777777, cam);
}

void r3d_move_camera(camera_t *cam, float fwd, float right, float up) {
    float cy = COS(cam->yaw), sy = SIN(cam->yaw);
    cam->pos.x += right*cy - fwd*sy;
    cam->pos.z += right*sy + fwd*cy;
    cam->pos.y += up;
    if (cam->pos.y < 0.5f) cam->pos.y = 0.5f;
    if (cam->pos.y > 2.8f) cam->pos.y = 2.8f;
}

void r3d_rotate_camera(camera_t *cam, float dyaw, float dpitch) {
    cam->yaw += dyaw;
    cam->pitch += dpitch;
    if (cam->pitch > 1.5f) cam->pitch = 1.5f;
    if (cam->pitch < -1.5f) cam->pitch = -1.5f;
}
