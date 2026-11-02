// The one mesh shader, used by both backends. There is no #version line: the GL
// backend prepends it at runtime, glslc gets -std=450core. Exactly one of VERTEX
// or FRAGMENT is defined. VULKAN is defined by the compiler for the SPIR-V build.
// After editing, run tools/compile_shaders.ps1 (needs glslc from the Vulkan SDK).

#ifdef VULKAN
layout(set = 0, binding = 0, std140) uniform Frame {
#else
layout(binding = 0, std140) uniform Frame {
#endif
    mat4 u_view_proj;
    vec4 u_sun_dir;    // xyz: direction towards the sun
    vec4 u_sun_color;
    vec4 u_ambient;
};

#ifdef VULKAN
layout(push_constant) uniform Draw {
    mat4 u_model;
    vec4 u_color;      // rgb: albedo, a: 1 lit, 0 unlit
};
#else
layout(location = 0) uniform mat4 u_model;
layout(location = 4) uniform vec4 u_color;
#endif

#ifdef VERTEX
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec3 v_surface;   // object space, in metres

void main() {
    vec3 scale = vec3(length(u_model[0].xyz), length(u_model[1].xyz), length(u_model[2].xyz));
    v_normal = mat3(u_model) * (a_normal / (scale * scale));
    v_surface = a_position * scale;
    gl_Position = u_view_proj * u_model * vec4(a_position, 1.0);
}
#endif

#ifdef FRAGMENT
layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec3 v_surface;
layout(location = 0) out vec4 o_color;

void main() {
    vec3 color = u_color.rgb;
    if (u_color.a > 0.5) {
        // A metre checker, so walking and flying read as motion without textures.
        vec3 cell = floor(v_surface + 0.001);
        float checker = mod(cell.x + cell.y + cell.z, 2.0);
        float diffuse = max(dot(normalize(v_normal), u_sun_dir.xyz), 0.0);
        color *= (0.92 + 0.08 * checker) * (u_ambient.rgb + u_sun_color.rgb * diffuse);
    }
    o_color = vec4(pow(color, vec3(1.0 / 2.2)), 1.0);
}
#endif
