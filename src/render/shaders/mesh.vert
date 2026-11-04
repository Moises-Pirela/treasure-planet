// The lit pass's vertex stage, for every material: the mesh in the world.
#include "common.glsl"

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec3 v_surface;   // object space, in metres
layout(location = 2) out vec3 v_world;

void main() {
    vec3 scale = vec3(length(u_model[0].xyz), length(u_model[1].xyz), length(u_model[2].xyz));
    v_normal = mat3(u_model) * (a_normal / (scale * scale));
    v_surface = a_position * scale;
    v_world = (u_model * vec4(a_position, 1.0)).xyz;
    gl_Position = u_view_proj * vec4(v_world, 1.0);
}
