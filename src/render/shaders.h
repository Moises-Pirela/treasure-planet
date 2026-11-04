// The shaders, as the build embedded them: every file of src/render/shaders as
// text (what OpenGL compiles at runtime, with the #includes expanded here) and,
// per program, the SPIR-V (what Vulkan loads), with the hash of the sources it
// was built from.
#ifndef TP_RENDER_SHADERS_H
#define TP_RENDER_SHADERS_H

#include <stddef.h>
#include <stdint.h>

struct r_shader_file {
    const char *name;            // relative to src/render/shaders: "materials/planet.frag"
    const unsigned char *data;   // the text, terminated
    size_t size;
};
struct r_spirv_blob {
    const char *name;            // the program: "mesh.vert", "materials/planet.frag:lit"
    const uint32_t *words;
    size_t bytes;
};

const r_shader_file *r_shader_files(uint32_t *count);
const r_shader_file *r_shader_file_find(const char *name);

// A GLSL program for OpenGL: the version line, then `defines` (lines of
// "#define X"), then the file with every #include "..." replaced by that file,
// each included once. Returns the length, or 0 when a file is missing or the
// buffer is too small.
size_t r_shader_assemble(const char *name, const char *defines, char *out, size_t capacity);

const r_spirv_blob *r_spirv_find(const char *name);
bool r_spirv_fresh(void);    // built from these very sources

#endif
