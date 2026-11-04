// The post steps' vertex stage: one triangle over the target.
#define POST_STEP
#include "common.glsl"

layout(location = 0) out vec2 v_uv;
void main() {
    vec2 ndc = vec2(VERTEX_INDEX == 1 ? 3.0 : -1.0, VERTEX_INDEX == 2 ? 3.0 : -1.0);
    v_uv = ndc * 0.5 + 0.5;
#ifdef VULKAN
    v_uv.y = 1.0 - v_uv.y;    // targets are rasterised y up, but Vulkan's row 0 is the top
#endif
    gl_Position = vec4(ndc, 0.0, 1.0);
}
