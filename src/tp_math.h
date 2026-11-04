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
// A point through a matrix, as a position (w = 1), without the divide.
static inline v3 m4_transform_point(const m4 *m, v3 p) {
    return v3_make(m->m[0] * p.x + m->m[4] * p.y + m->m[8] * p.z + m->m[12],
                   m->m[1] * p.x + m->m[5] * p.y + m->m[9] * p.z + m->m[13],
                   m->m[2] * p.x + m->m[6] * p.y + m->m[10] * p.z + m->m[14]);
}
// Orthographic, reversed like m4_perspective: depth 1 at the eye's plane, 0 at
// `depth` in front of it. x and y map [-half_width, half_width] and
// [-half_height, half_height] to [-1, 1].
static inline m4 m4_ortho(float half_width, float half_height, float depth) {
    m4 r = m4_identity();
    r.m[0] = 1.0f / half_width;
    r.m[5] = 1.0f / half_height;
    r.m[10] = 1.0f / depth;      // view space looks down -z, so z/depth runs 0..-1
    r.m[14] = 1.0f;
    return r;
}
static inline v3 look_direction(float yaw, float pitch) {
    return v3_make(-sinf(yaw) * cosf(pitch), sinf(pitch), -cosf(yaw) * cosf(pitch));
}
// `up` is a hint: the frame's up is rebuilt perpendicular to `forward`.
static inline m4 m4_look_to_up(v3 eye, v3 forward, v3 up_hint) {
    v3 side = v3_normalize(v3_cross(forward, up_hint));
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
static inline m4 m4_look_to(v3 eye, v3 forward) { return m4_look_to_up(eye, forward, v3_make(0, 1, 0)); }
// The inverse of look_direction: the yaw and pitch that look along `direction` (unit).
static inline void look_angles(v3 direction, float *yaw, float *pitch) {
    float y = direction.y > 1.0f ? 1.0f : direction.y < -1.0f ? -1.0f : direction.y;
    *pitch = asinf(y);
    *yaw = atan2f(-direction.x, -direction.z);
}

// Rotations, as unit quaternions. A rotation takes a body's own frame to the world:
// quat_rotate(q, v) is the body vector v in world axes.
struct quat { float x, y, z, w; };

static inline quat quat_make(float x, float y, float z, float w) { quat q = {x, y, z, w}; return q; }
static inline quat quat_identity(void) { return quat_make(0, 0, 0, 1); }
static inline quat quat_mul(quat a, quat b) {
    return quat_make(a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                     a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                     a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                     a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);
}
static inline quat quat_conjugate(quat q) { return quat_make(-q.x, -q.y, -q.z, q.w); }
static inline float quat_dot(quat a, quat b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
// A zero quaternion (a field never set) normalises to the identity.
static inline quat quat_normalize(quat q) {
    float length = sqrtf(quat_dot(q, q));
    if (length <= 0.0f) return quat_identity();
    float s = 1.0f / length;
    return quat_make(q.x * s, q.y * s, q.z * s, q.w * s);
}
static inline quat quat_from_axis_angle(v3 axis, float angle) {
    float s = sinf(angle * 0.5f);
    return quat_make(axis.x * s, axis.y * s, axis.z * s, cosf(angle * 0.5f));
}
// Heading about +y, in look_direction's sense: 0 faces -z.
static inline quat quat_from_yaw(float yaw) { return quat_from_axis_angle(v3_make(0, 1, 0), yaw); }
static inline v3 quat_rotate(quat q, v3 v) {
    v3 u = v3_make(q.x, q.y, q.z);
    v3 t = v3_scale(v3_cross(u, v), 2.0f);
    return v3_add(v3_add(v, v3_scale(t, q.w)), v3_cross(u, t));
}
static inline v3 quat_unrotate(quat q, v3 v) { return quat_rotate(quat_conjugate(q), v); }
// The orientation after turning at `spin` (rad/s, in the body's own axes) for `dt`.
static inline quat quat_integrate(quat q, v3 spin, float dt) {
    float rate = v3_length(spin);
    if (rate * dt < 1e-9f) return q;
    return quat_normalize(quat_mul(q, quat_from_axis_angle(v3_scale(spin, 1.0f / rate), rate * dt)));
}
// Between two rotations the short way round; normalised linear interpolation, which
// is what a tick's worth of turning needs.
static inline quat quat_nlerp(quat a, quat b, float t) {
    if (quat_dot(a, b) < 0.0f) b = quat_make(-b.x, -b.y, -b.z, -b.w);
    return quat_normalize(quat_make(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                                    a.w + (b.w - a.w) * t));
}
// Position, rotation and then scale: a box of half extents `scale` in a body's frame.
static inline m4 m4_pose(v3 position, quat q, v3 scale) {
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z, wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    m4 r = m4_identity();
    r.m[0] = (1.0f - 2.0f * (yy + zz)) * scale.x;
    r.m[1] = 2.0f * (xy + wz) * scale.x;
    r.m[2] = 2.0f * (xz - wy) * scale.x;
    r.m[4] = 2.0f * (xy - wz) * scale.y;
    r.m[5] = (1.0f - 2.0f * (xx + zz)) * scale.y;
    r.m[6] = 2.0f * (yz + wx) * scale.y;
    r.m[8] = 2.0f * (xz + wy) * scale.z;
    r.m[9] = 2.0f * (yz - wx) * scale.z;
    r.m[10] = (1.0f - 2.0f * (xx + yy)) * scale.z;
    r.m[12] = position.x;
    r.m[13] = position.y;
    r.m[14] = position.z;
    return r;
}

#endif
