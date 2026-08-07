#ifndef RENDER3D_H
#define RENDER3D_H

#include <stdint.h>

typedef struct { float x, y, z; } vec3f;

typedef struct {
    vec3f pos;
    float yaw, pitch;
} camera_t;

void r3d_init(int w, int h);
void r3d_set_viewport(int x, int y, int w, int h);
void r3d_begin_frame(void);
void r3d_draw(camera_t *cam);
void r3d_tick(void);
void r3d_move_camera(camera_t *cam, float forward, float right, float up);
void r3d_rotate_camera(camera_t *cam, float dyaw, float dpitch);

#endif
