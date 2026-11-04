// The sky: one triangle over the screen, at the far plane, drawn where nothing else was.
#include "common.glsl"

layout(location = 0) out vec2 v_ndc;
void main() {
    v_ndc = vec2(VERTEX_INDEX == 1 ? 3.0 : -1.0, VERTEX_INDEX == 2 ? 3.0 : -1.0);
    gl_Position = vec4(v_ndc, 0.0, 1.0);
}
