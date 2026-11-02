// Vectors and matrices. Right-handed, y up, -z forward at yaw 0, metres, radians.
// Matrices are column-major: m[column * 4 + row], as GLSL expects them.
#ifndef TP_MATH_H
#define TP_MATH_H

#include <math.h>

#define TP_PI 3.14159265358979f

struct v3 { float x, y, z; };
struct m4 { float m[16]; };

static inline v3 v3_make(float x, float y, float z) { v3 r = {x, y, z}; return r; }
static inline v3 v3_add(v3 a, v3 b) { return v3_make(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline v3 v3_sub(v3 a, v3 b) { return v3_make(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline v3 v3_scale(v3 a, float s) { return v3_make(a.x * s, a.y * s, a.z * s); }
static inline v3 v3_lerp(v3 a, v3 b, float t) { return v3_add(a, v3_scale(v3_sub(b, a), t)); }
static inline float v3_dot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline float v3_length(v3 a) { return sqrtf(v3_dot(a, a)); }
static inline v3 v3_cross(v3 a, v3 b) {
    return v3_make(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline v3 v3_normalize(v3 a) {
    float length = v3_length(a);
    return length > 0.0f ? v3_scale(a, 1.0f / length) : a;
}

static inline m4 m4_identity(void) {
    m4 r = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
    return r;
}
static inline m4 m4_mul(m4 a, m4 b) {
    m4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = sum;
        }
    return r;
}
static inline m4 m4_translate_scale(v3 position, v3 scale) {
    m4 r = m4_identity();
    r.m[0] = scale.x;
    r.m[5] = scale.y;
    r.m[10] = scale.z;
    r.m[12] = position.x;
    r.m[13] = position.y;
    r.m[14] = position.z;
    return r;
}
// Reversed depth with an infinite far plane: depth is 1 at the near plane and
// falls to 0 at infinity. Both backends set up zero-to-one clip space with y up,
// so this one matrix serves both, and a deck rail and a planet share a depth buffer.
static inline m4 m4_perspective(float fov_y, float aspect, float near_plane) {
    float f = 1.0f / tanf(fov_y * 0.5f);
    m4 r = {{0}};
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[11] = -1.0f;
    r.m[14] = near_plane;
    return r;
}
static inline v3 look_direction(float yaw, float pitch) {
    return v3_make(-sinf(yaw) * cosf(pitch), sinf(pitch), -cosf(yaw) * cosf(pitch));
}
static inline m4 m4_look_to(v3 eye, v3 forward) {
    v3 side = v3_normalize(v3_cross(forward, v3_make(0, 1, 0)));
    v3 up = v3_cross(side, forward);
    m4 r = m4_identity();
    r.m[0] = side.x;  r.m[4] = side.y;  r.m[8] = side.z;
    r.m[1] = up.x;    r.m[5] = up.y;    r.m[9] = up.z;
    r.m[2] = -forward.x; r.m[6] = -forward.y; r.m[10] = -forward.z;
    r.m[12] = -v3_dot(side, eye);
    r.m[13] = -v3_dot(up, eye);
    r.m[14] = v3_dot(forward, eye);
    return r;
}

#endif
