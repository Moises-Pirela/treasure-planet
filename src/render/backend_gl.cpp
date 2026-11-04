// OpenGL 4.5 core, direct state access. Draws into its own targets: the scene is
// floating point, with a floating-point depth texture that reversed depth needs
// (the depth target of the table, which the post steps sample unfiltered); the
// window target is the window's own framebuffer. The shadow map is a depth
// texture array with one layer per cascade, each its own framebuffer. Every
// other target of backend.h's table is a texture with a framebuffer, and a post
// step is one program, its inputs on texture units, one triangle. The passes are
// those of backend.h, in that order.
#include "backend.h"
#include "shaders.h"
#include <GL/glcorearb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GL_FUNCTIONS(X) \
    X(PFNGLCREATESHADERPROC, glCreateShader) \
    X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) \
    X(PFNGLDELETESHADERPROC, glDeleteShader) \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram) \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) \
    X(PFNGLPROGRAMUNIFORMMATRIX4FVPROC, glProgramUniformMatrix4fv) \
    X(PFNGLPROGRAMUNIFORM4FVPROC, glProgramUniform4fv) \
    X(PFNGLPROGRAMUNIFORM1UIPROC, glProgramUniform1ui) \
    X(PFNGLCREATEBUFFERSPROC, glCreateBuffers) \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers) \
    X(PFNGLNAMEDBUFFERSTORAGEPROC, glNamedBufferStorage) \
    X(PFNGLNAMEDBUFFERSUBDATAPROC, glNamedBufferSubData) \
    X(PFNGLBINDBUFFERBASEPROC, glBindBufferBase) \
    X(PFNGLCREATEVERTEXARRAYSPROC, glCreateVertexArrays) \
    X(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays) \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
    X(PFNGLVERTEXARRAYVERTEXBUFFERPROC, glVertexArrayVertexBuffer) \
    X(PFNGLVERTEXARRAYELEMENTBUFFERPROC, glVertexArrayElementBuffer) \
    X(PFNGLVERTEXARRAYATTRIBFORMATPROC, glVertexArrayAttribFormat) \
    X(PFNGLVERTEXARRAYATTRIBBINDINGPROC, glVertexArrayAttribBinding) \
    X(PFNGLENABLEVERTEXARRAYATTRIBPROC, glEnableVertexArrayAttrib) \
    X(PFNGLCREATEFRAMEBUFFERSPROC, glCreateFramebuffers) \
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers) \
    X(PFNGLCREATERENDERBUFFERSPROC, glCreateRenderbuffers) \
    X(PFNGLDELETERENDERBUFFERSPROC, glDeleteRenderbuffers) \
    X(PFNGLNAMEDRENDERBUFFERSTORAGEPROC, glNamedRenderbufferStorage) \
    X(PFNGLNAMEDFRAMEBUFFERRENDERBUFFERPROC, glNamedFramebufferRenderbuffer) \
    X(PFNGLNAMEDFRAMEBUFFERTEXTURELAYERPROC, glNamedFramebufferTextureLayer) \
    X(PFNGLNAMEDFRAMEBUFFERDRAWBUFFERPROC, glNamedFramebufferDrawBuffer) \
    X(PFNGLNAMEDFRAMEBUFFERREADBUFFERPROC, glNamedFramebufferReadBuffer) \
    X(PFNGLCHECKNAMEDFRAMEBUFFERSTATUSPROC, glCheckNamedFramebufferStatus) \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLCLEARNAMEDFRAMEBUFFERFVPROC, glClearNamedFramebufferfv) \
    X(PFNGLCREATETEXTURESPROC, glCreateTextures) \
    X(PFNGLDELETETEXTURESPROC, glDeleteTextures) \
    X(PFNGLTEXTURESTORAGE2DPROC, glTextureStorage2D) \
    X(PFNGLTEXTURESTORAGE3DPROC, glTextureStorage3D) \
    X(PFNGLNAMEDFRAMEBUFFERTEXTUREPROC, glNamedFramebufferTexture) \
    X(PFNGLTEXTUREPARAMETERIPROC, glTextureParameteri) \
    X(PFNGLTEXTUREPARAMETERFVPROC, glTextureParameterfv) \
    X(PFNGLBINDTEXTUREUNITPROC, glBindTextureUnit) \
    X(PFNGLCLIPCONTROLPROC, glClipControl) \
    X(PFNGLDEBUGMESSAGECALLBACKPROC, glDebugMessageCallback) \
    X(PFNGLVIEWPORTPROC, glViewport) \
    X(PFNGLENABLEPROC, glEnable) \
    X(PFNGLDISABLEPROC, glDisable) \
    X(PFNGLDEPTHFUNCPROC, glDepthFunc) \
    X(PFNGLDEPTHMASKPROC, glDepthMask) \
    X(PFNGLCULLFACEPROC, glCullFace) \
    X(PFNGLFRONTFACEPROC, glFrontFace) \
    X(PFNGLDRAWELEMENTSPROC, glDrawElements) \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays) \
    X(PFNGLREADPIXELSPROC, glReadPixels) \
    X(PFNGLPIXELSTOREIPROC, glPixelStorei) \
    X(PFNGLCREATEQUERIESPROC, glCreateQueries) \
    X(PFNGLDELETEQUERIESPROC, glDeleteQueries) \
    X(PFNGLBEGINQUERYPROC, glBeginQuery) \
    X(PFNGLENDQUERYPROC, glEndQuery) \
    X(PFNGLGETQUERYOBJECTIVPROC, glGetQueryObjectiv) \
    X(PFNGLGETQUERYOBJECTUI64VPROC, glGetQueryObjectui64v)

#define DECLARE(type, name) static type name;
GL_FUNCTIONS(DECLARE)
#undef DECLARE

// The Draw and Step uniforms' locations, as common.glsl declares them.
#define GL_LOCATION_MODEL 0
#define GL_LOCATION_COLOR 4
#define GL_LOCATION_FLAGS 5
#define GL_LOCATION_CASCADE 6
#define GL_LOCATION_MATERIAL 7
#define GL_LOCATION_LIGHTS 8
#define GL_LOCATION_TEXEL 0
#define GL_LOCATION_PARAMS 1

static const char *const POST_PROGRAM_FILES[R_POST_PROGRAM_COUNT] = {
    "post_downsample.frag", "post_upsample.frag", "post_flare.frag", "post_composite.frag", "post_fxaa.frag",
    "post_outline.frag", "post_haze.frag",
};
#define GL_SOURCE_CAPACITY (256 * 1024)   // a program's assembled source

struct gl_mesh {
    GLuint vertex_array, vertex_buffer, index_buffer;
    GLsizei index_count;
};

struct gl_target {
    GLuint texture, framebuffer;
    int width, height;
};

// The timers: one frame's passes each get a GL_TIME_ELAPSED query, in a ring of
// frames so that reading a frame's results never waits for the GPU.
#define GL_TIMED_PASSES (3 + R_MAX_POST_STEPS)   // shadow, main, sky, then each post step
#define GL_TIMER_FRAMES 3
struct gl_timed_frame {
    GLuint queries[GL_TIMED_PASSES];
    int pass[GL_TIMED_PASSES];                   // r_pass of each query, -1 when unused
    bool written;
};

struct gl_state {
    plat_window *window;
    r_shader_set shaders;
    GLuint lit_programs[R_MAX_SHADERS];              // the lit pass, per material shader
    GLuint cutout_programs[R_MAX_SHADERS];           // the shadow pass of a cutout material shader; 0 for the rest
    GLuint shadow_program, sky_program;              // the shadow pass of everything else; the sky
    GLuint post_programs[R_POST_PROGRAM_COUNT];
    char *source;                                    // scratch for assembling programs
    GLuint frame_uniforms, light_uniforms, post_uniforms, material_uniforms;
    gl_target targets[R_TARGET_COUNT];               // backend.h's table, at the window's size
    GLuint depth_target;                             // the scene's depth, a texture: the depth target's
    int width, height;
    GLuint shadow_map;                               // a depth array, one layer per cascade
    GLuint shadow_framebuffers[R_SHADOW_CASCADES];
    uint32_t shadow_resolution;                      // what the map was made at
    GLuint empty_vertex_array;                       // the sky and the post steps make their triangle in the shader
    gl_mesh meshes[R_MAX_MESHES];                    // slot 0 stays empty: mesh 0 means none
    gl_timed_frame timed[GL_TIMER_FRAMES];
    uint32_t timed_frame;                            // the ring slot this frame writes
    r_timings timings;                               // the last frame read back
};

// Reads a timed frame's queries, if they are in, into the timings.
static void timers_collect(gl_state *s, gl_timed_frame *t) {
    if (!t->written) return;
    r_timings result = {};
    for (int i = 0; i < GL_TIMED_PASSES; ++i) {
        if (t->pass[i] < 0) continue;
        GLint available = 0;
        glGetQueryObjectiv(t->queries[i], GL_QUERY_RESULT_AVAILABLE, &available);
        if (!available) return;    // too soon: keep the last timings
        GLuint64 nanoseconds = 0;
        glGetQueryObjectui64v(t->queries[i], GL_QUERY_RESULT, &nanoseconds);
        result.ms[t->pass[i]] += (float)((double)nanoseconds * 1e-6);
    }
    for (int p = 0; p < R_PASS_COUNT; ++p) result.total_ms += result.ms[p];
    result.valid = true;
    s->timings = result;
}

static void timer_begin(gl_state *s, int slot, int pass) {
    gl_timed_frame *t = &s->timed[s->timed_frame];
    t->pass[slot] = pass;
    glBeginQuery(GL_TIME_ELAPSED, t->queries[slot]);
}
static void timer_end(void) { glEndQuery(GL_TIME_ELAPSED); }

static r_timings gl_timings(void *state) { return ((gl_state *)state)->timings; }

#ifndef NDEBUG
static void APIENTRY gl_debug(GLenum, GLenum type, GLuint, GLenum, GLsizei, const GLchar *message, const void *) {
    if (type == GL_DEBUG_TYPE_ERROR) fprintf(stderr, "opengl: %s\n", message);
}
#endif

// One stage from one shader file, its #includes expanded, with `defines` first.
static GLuint compile_stage(gl_state *s, GLenum stage, const char *file, const char *defines) {
    if (!r_shader_assemble(file, defines, s->source, GL_SOURCE_CAPACITY)) return 0;
    const char *sources[] = {s->source};
    GLuint shader = glCreateShader(stage);
    glShaderSource(shader, 1, sources, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "opengl: %s (%s): %s\n", file, defines, log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint create_program(gl_state *s, const char *vertex_file, const char *fragment_file, const char *fragment_defines) {
    GLuint vertex = compile_stage(s, GL_VERTEX_SHADER, vertex_file, "");
    GLuint fragment = compile_stage(s, GL_FRAGMENT_SHADER, fragment_file, fragment_defines);
    if (!vertex || !fragment) return 0;
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        fprintf(stderr, "opengl: link: %s\n", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

static void destroy_targets(gl_state *s) {
    s->targets[R_TARGET_DEPTH].texture = 0;    // the depth texture's, deleted once below
    for (int t = 0; t < R_TARGET_COUNT; ++t) {
        gl_target *target = &s->targets[t];
        if (target->framebuffer) glDeleteFramebuffers(1, &target->framebuffer);
        if (target->texture) glDeleteTextures(1, &target->texture);
        memset(target, 0, sizeof(*target));
    }
    if (s->depth_target) glDeleteTextures(1, &s->depth_target);
    s->depth_target = 0;
}

static void resize_targets(gl_state *s, int width, int height) {
    if (s->targets[R_TARGET_SCENE].framebuffer && s->width == width && s->height == height) return;
    destroy_targets(s);
    s->width = width;
    s->height = height;
    // The depth: the scene's buffer, and the depth target the steps read, unfiltered.
    glCreateTextures(GL_TEXTURE_2D, 1, &s->depth_target);
    glTextureStorage2D(s->depth_target, 1, GL_DEPTH_COMPONENT32F, width, height);
    glTextureParameteri(s->depth_target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(s->depth_target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTextureParameteri(s->depth_target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(s->depth_target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    for (int t = 0; t < R_TARGET_COUNT; ++t) {
        r_target_desc desc = r_target_info(t);
        gl_target *target = &s->targets[t];
        target->width = (width >> desc.shift) > 1 ? width >> desc.shift : 1;
        target->height = (height >> desc.shift) > 1 ? height >> desc.shift : 1;
        if (t == R_TARGET_WINDOW) continue;    // framebuffer 0: the window's own
        if (desc.depth) {
            target->texture = s->depth_target;
            continue;
        }
        glCreateTextures(GL_TEXTURE_2D, 1, &target->texture);
        glTextureStorage2D(target->texture, 1, desc.hdr ? GL_RGBA16F : GL_RGBA8, target->width, target->height);
        glTextureParameteri(target->texture, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(target->texture, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(target->texture, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(target->texture, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glCreateFramebuffers(1, &target->framebuffer);
        glNamedFramebufferTexture(target->framebuffer, GL_COLOR_ATTACHMENT0, target->texture, 0);
        if (t == R_TARGET_SCENE) glNamedFramebufferTexture(target->framebuffer, GL_DEPTH_ATTACHMENT, s->depth_target, 0);
        if (glCheckNamedFramebufferStatus(target->framebuffer, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            fprintf(stderr, "opengl: framebuffer %d incomplete\n", t);
    }
}

static void destroy_shadow_map(gl_state *s) {
    for (int c = 0; c < R_SHADOW_CASCADES; ++c)
        if (s->shadow_framebuffers[c]) glDeleteFramebuffers(1, &s->shadow_framebuffers[c]);
    if (s->shadow_map) glDeleteTextures(1, &s->shadow_map);
    memset(s->shadow_framebuffers, 0, sizeof(s->shadow_framebuffers));
    s->shadow_map = 0;
    s->shadow_resolution = 0;
}

static bool create_shadow_map(gl_state *s, uint32_t resolution) {
    destroy_shadow_map(s);
    s->shadow_resolution = resolution;
    glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &s->shadow_map);
    glTextureStorage3D(s->shadow_map, 1, GL_DEPTH_COMPONENT32F, (GLsizei)resolution, (GLsizei)resolution, R_SHADOW_CASCADES);
    glTextureParameteri(s->shadow_map, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(s->shadow_map, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    // Outside the map the depth reads as 0, the far plane: lit.
    glTextureParameteri(s->shadow_map, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTextureParameteri(s->shadow_map, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    const GLfloat border[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    glTextureParameterfv(s->shadow_map, GL_TEXTURE_BORDER_COLOR, border);
    // Reversed depth: lit where the reference is at least as near as what is stored.
    glTextureParameteri(s->shadow_map, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTextureParameteri(s->shadow_map, GL_TEXTURE_COMPARE_FUNC, GL_GEQUAL);
    for (int c = 0; c < R_SHADOW_CASCADES; ++c) {
        glCreateFramebuffers(1, &s->shadow_framebuffers[c]);
        glNamedFramebufferTextureLayer(s->shadow_framebuffers[c], GL_DEPTH_ATTACHMENT, s->shadow_map, 0, c);
        glNamedFramebufferDrawBuffer(s->shadow_framebuffers[c], GL_NONE);
        glNamedFramebufferReadBuffer(s->shadow_framebuffers[c], GL_NONE);
        if (glCheckNamedFramebufferStatus(s->shadow_framebuffers[c], GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "opengl: shadow framebuffer incomplete\n");
            return false;
        }
    }
    glBindTextureUnit(R_BIND_SHADOW_MAP, s->shadow_map);
    return true;
}

static void gl_destroy(void *state);

static void *gl_create(plat_window *window, const r_shader_set *shaders) {
    if (!plat_gl_create(window)) return NULL;
#define LOAD(type, name) name = (type)plat_gl_proc(#name); \
    if (!name) { fprintf(stderr, "opengl: missing %s\n", #name); plat_gl_destroy(window); return NULL; }
    GL_FUNCTIONS(LOAD)
#undef LOAD

    gl_state *s = (gl_state *)calloc(1, sizeof(gl_state));
    s->window = window;
    s->shaders = *shaders;
    s->source = (char *)malloc(GL_SOURCE_CAPACITY);
#ifndef NDEBUG
    glEnable(GL_DEBUG_OUTPUT);
    glDebugMessageCallback(gl_debug, NULL);
#endif
    s->shadow_program = create_program(s, "shadow.vert", "shadow.frag", "");
    s->sky_program = create_program(s, "sky.vert", "sky.frag", "");
    bool ok = s->shadow_program && s->sky_program;
    for (uint32_t i = 0; i < shaders->count; ++i) {
        const r_shader_info *info = &shaders->items[i];
        s->lit_programs[i] = create_program(s, "mesh.vert", info->file, "#define LIT_PASS\n");
        ok = ok && s->lit_programs[i];
        if (info->cutout) {
            s->cutout_programs[i] = create_program(s, "shadow.vert", info->file, "#define SHADOW_PASS\n");
            ok = ok && s->cutout_programs[i];
        }
    }
    for (int i = 0; i < R_POST_PROGRAM_COUNT; ++i) {
        s->post_programs[i] = create_program(s, "post.vert", POST_PROGRAM_FILES[i], "");
        ok = ok && s->post_programs[i];
    }
    if (!ok || !create_shadow_map(s, R_SHADOW_RESOLUTION_MIN)) {
        gl_destroy(s);
        return NULL;
    }
    glCreateBuffers(1, &s->frame_uniforms);
    glNamedBufferStorage(s->frame_uniforms, sizeof(r_gpu_frame), NULL, GL_DYNAMIC_STORAGE_BIT);
    glBindBufferBase(GL_UNIFORM_BUFFER, R_BIND_FRAME, s->frame_uniforms);
    glCreateBuffers(1, &s->light_uniforms);
    glNamedBufferStorage(s->light_uniforms, sizeof(r_gpu_lights), NULL, GL_DYNAMIC_STORAGE_BIT);
    glBindBufferBase(GL_UNIFORM_BUFFER, R_BIND_LIGHTS, s->light_uniforms);
    glCreateBuffers(1, &s->post_uniforms);
    glNamedBufferStorage(s->post_uniforms, sizeof(r_gpu_post), NULL, GL_DYNAMIC_STORAGE_BIT);
    glBindBufferBase(GL_UNIFORM_BUFFER, R_BIND_POST, s->post_uniforms);
    glCreateBuffers(1, &s->material_uniforms);
    glNamedBufferStorage(s->material_uniforms, sizeof(r_gpu_materials), NULL, GL_DYNAMIC_STORAGE_BIT);
    glBindBufferBase(GL_UNIFORM_BUFFER, R_BIND_MATERIALS, s->material_uniforms);
    glCreateVertexArrays(1, &s->empty_vertex_array);
    for (int f = 0; f < GL_TIMER_FRAMES; ++f) glCreateQueries(GL_TIME_ELAPSED, GL_TIMED_PASSES, s->timed[f].queries);

    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GREATER);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    return s;
}

static void gl_mesh_destroy(void *state, r_mesh mesh) {
    gl_state *s = (gl_state *)state;
    if (!mesh || mesh >= R_MAX_MESHES || !s->meshes[mesh].vertex_array) return;
    gl_mesh *m = &s->meshes[mesh];
    glDeleteVertexArrays(1, &m->vertex_array);
    glDeleteBuffers(1, &m->vertex_buffer);
    glDeleteBuffers(1, &m->index_buffer);
    memset(m, 0, sizeof(*m));
}

static void gl_destroy(void *state) {
    gl_state *s = (gl_state *)state;
    for (r_mesh i = 1; i < R_MAX_MESHES; ++i) gl_mesh_destroy(s, i);
    destroy_targets(s);
    destroy_shadow_map(s);
    if (s->empty_vertex_array) glDeleteVertexArrays(1, &s->empty_vertex_array);
    for (int f = 0; f < GL_TIMER_FRAMES; ++f)
        if (s->timed[f].queries[0]) glDeleteQueries(GL_TIMED_PASSES, s->timed[f].queries);
    if (s->frame_uniforms) glDeleteBuffers(1, &s->frame_uniforms);
    if (s->light_uniforms) glDeleteBuffers(1, &s->light_uniforms);
    if (s->post_uniforms) glDeleteBuffers(1, &s->post_uniforms);
    if (s->material_uniforms) glDeleteBuffers(1, &s->material_uniforms);
    for (int i = 0; i < R_MAX_SHADERS; ++i) {
        if (s->lit_programs[i]) glDeleteProgram(s->lit_programs[i]);
        if (s->cutout_programs[i]) glDeleteProgram(s->cutout_programs[i]);
    }
    if (s->shadow_program) glDeleteProgram(s->shadow_program);
    if (s->sky_program) glDeleteProgram(s->sky_program);
    free(s->source);
    for (int i = 0; i < R_POST_PROGRAM_COUNT; ++i)
        if (s->post_programs[i]) glDeleteProgram(s->post_programs[i]);
    plat_gl_destroy(s->window);
    free(s);
}

static r_mesh gl_mesh_create(void *state, const r_vertex *vertices, uint32_t vertex_count,
                             const uint32_t *indices, uint32_t index_count) {
    gl_state *s = (gl_state *)state;
    r_mesh slot = 1;
    while (slot < R_MAX_MESHES && s->meshes[slot].vertex_array) ++slot;
    if (slot == R_MAX_MESHES) return 0;
    gl_mesh *m = &s->meshes[slot];
    glCreateBuffers(1, &m->vertex_buffer);
    glCreateBuffers(1, &m->index_buffer);
    glNamedBufferStorage(m->vertex_buffer, vertex_count * sizeof(r_vertex), vertices, 0);
    glNamedBufferStorage(m->index_buffer, index_count * sizeof(uint32_t), indices, 0);
    glCreateVertexArrays(1, &m->vertex_array);
    glVertexArrayVertexBuffer(m->vertex_array, 0, m->vertex_buffer, 0, sizeof(r_vertex));
    glVertexArrayElementBuffer(m->vertex_array, m->index_buffer);
    glEnableVertexArrayAttrib(m->vertex_array, 0);
    glEnableVertexArrayAttrib(m->vertex_array, 1);
    glVertexArrayAttribFormat(m->vertex_array, 0, 3, GL_FLOAT, GL_FALSE, offsetof(r_vertex, position));
    glVertexArrayAttribFormat(m->vertex_array, 1, 3, GL_FLOAT, GL_FALSE, offsetof(r_vertex, normal));
    glVertexArrayAttribBinding(m->vertex_array, 0, 0);
    glVertexArrayAttribBinding(m->vertex_array, 1, 0);
    m->index_count = (GLsizei)index_count;
    return slot;
}

// Draws one pass's range of the packet: the lit pass with each material's own
// program, or a shadow cascade with the plain depth program and, for a cutout
// material, its own. The pass is sorted by shader, so programs change seldom.
static void draw_pass(gl_state *s, const r_packet *packet, r_pass pass, bool lit, uint32_t cascade) {
    bool culling = true;
    GLuint program = 0;
    for (uint32_t i = 0; i < pass.count; ++i) {
        uint32_t index = packet->indices[pass.first + i];
        const r_draw *draw = &packet->draws[index];
        if (draw->mesh >= R_MAX_MESHES) continue;
        const gl_mesh *m = &s->meshes[draw->mesh];
        if (!m->vertex_array) continue;
        uint32_t material = draw->material < R_MAX_MATERIALS ? draw->material : 0;
        uint32_t shader = packet->material_shader[material];
        GLuint wanted = lit ? s->lit_programs[shader] : (packet->material_cutout[material] ? s->cutout_programs[shader] : s->shadow_program);
        if (wanted != program) {
            program = wanted;
            glUseProgram(program);
            if (!lit) glProgramUniform1ui(program, GL_LOCATION_CASCADE, cascade);
        }
        bool two_sided = (draw->flags & R_DRAW_TWO_SIDED) != 0;
        if (two_sided == culling) {
            culling = !two_sided;
            if (culling) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
        }
        glProgramUniformMatrix4fv(program, GL_LOCATION_MODEL, 1, GL_FALSE, draw->model.m);
        if (lit || packet->material_cutout[material]) glProgramUniform1ui(program, GL_LOCATION_MATERIAL, material);
        if (lit) {
            glProgramUniform4fv(program, GL_LOCATION_COLOR, 1, draw->color);
            glProgramUniform1ui(program, GL_LOCATION_FLAGS, draw->flags);
            glProgramUniform1ui(program, GL_LOCATION_LIGHTS, packet->light_masks[index]);
        }
        glBindVertexArray(m->vertex_array);
        glDrawElements(GL_TRIANGLES, m->index_count, GL_UNSIGNED_INT, NULL);
    }
    if (!culling) glEnable(GL_CULL_FACE);
}

static void gl_submit(void *state, const r_packet *packet, int width, int height) {
    gl_state *s = (gl_state *)state;
    resize_targets(s, width, height);
    glNamedBufferSubData(s->frame_uniforms, 0, sizeof(packet->frame), &packet->frame);
    glNamedBufferSubData(s->light_uniforms, 0, sizeof(packet->lights), &packet->lights);
    glNamedBufferSubData(s->post_uniforms, 0, sizeof(packet->post), &packet->post);
    glNamedBufferSubData(s->material_uniforms, 0, sizeof(packet->materials), &packet->materials);
    const GLfloat depth_clear = 0.0f;
    const gl_target *scene = &s->targets[R_TARGET_SCENE];

    // This frame's timers: read the ring slot's old results first.
    gl_timed_frame *timed = &s->timed[s->timed_frame];
    timers_collect(s, timed);
    for (int i = 0; i < GL_TIMED_PASSES; ++i) timed->pass[i] = -1;
    timed->written = true;

    // 1. The shadow map, a cascade per layer, from the sun's point of view.
    if (packet->cascade_count && packet->shadow_resolution != s->shadow_resolution) create_shadow_map(s, packet->shadow_resolution);
    timer_begin(s, 0, R_PASS_SHADOW);
    glCullFace(GL_FRONT);
    glViewport(0, 0, (GLsizei)s->shadow_resolution, (GLsizei)s->shadow_resolution);
    for (uint32_t c = 0; c < packet->cascade_count; ++c) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s->shadow_framebuffers[c]);
        glClearNamedFramebufferfv(s->shadow_framebuffers[c], GL_DEPTH, 0, &depth_clear);
        draw_pass(s, packet, packet->shadow[c], false, c);
    }
    timer_end();

    // 2. The lit pass, into the scene target, its depth attached again (the post
    // steps had it off: see below).
    timer_begin(s, 1, R_PASS_MAIN);
    glNamedFramebufferTexture(scene->framebuffer, GL_DEPTH_ATTACHMENT, s->depth_target, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, scene->framebuffer);
    glViewport(0, 0, width, height);
    const GLfloat color_clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    glClearNamedFramebufferfv(scene->framebuffer, GL_COLOR, 0, color_clear);
    glClearNamedFramebufferfv(scene->framebuffer, GL_DEPTH, 0, &depth_clear);
    glCullFace(GL_BACK);
    draw_pass(s, packet, packet->main, true, 0);
    timer_end();

    // 3. The sky, where nothing was drawn.
    timer_begin(s, 2, R_PASS_SKY);
    glUseProgram(s->sky_program);
    glDepthFunc(GL_GEQUAL);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(s->empty_vertex_array);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glEnable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_GREATER);
    timer_end();

    // 4. The post steps, the last of which writes the window target. The depth
    // comes off the scene target first: a step may write the scene target while
    // reading the depth target, which is that same texture, and a texture both
    // attached and sampled is a feedback loop.
    glNamedFramebufferTexture(scene->framebuffer, GL_DEPTH_ATTACHMENT, 0, 0);
    glDisable(GL_DEPTH_TEST);
    glBindVertexArray(s->empty_vertex_array);
    for (uint32_t i = 0; i < packet->step_count; ++i) {
        const r_post_step *step = &packet->steps[i];
        const gl_target *target = &s->targets[step->target];
        const gl_target *input = &s->targets[step->inputs[0]];
        r_post_push push;
        push.texel[0] = 1.0f / (float)input->width;
        push.texel[1] = 1.0f / (float)input->height;
        push.texel[2] = 1.0f / (float)target->width;
        push.texel[3] = 1.0f / (float)target->height;
        memcpy(push.params, step->params, sizeof(push.params));
        GLuint program = s->post_programs[step->program];
        glProgramUniform4fv(program, GL_LOCATION_TEXEL, 1, push.texel);
        glProgramUniform4fv(program, GL_LOCATION_PARAMS, 1, push.params);
        for (int k = 0; k < 3; ++k) glBindTextureUnit(R_BIND_INPUT + k, s->targets[step->inputs[k]].texture);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target->framebuffer);
        glViewport(0, 0, target->width, target->height);
        glUseProgram(program);
        timer_begin(s, 3 + (int)i, r_post_program_pass(step->program));
        glDrawArrays(GL_TRIANGLES, 0, 3);
        timer_end();
    }
    glEnable(GL_DEPTH_TEST);
    s->timed_frame = (s->timed_frame + 1) % GL_TIMER_FRAMES;

    if (packet->capture_rgba) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, packet->capture_rgba);
        // GL's first row is the bottom one.
        size_t stride = (size_t)width * 4;
        uint8_t *row = (uint8_t *)malloc(stride);
        for (int y = 0; y < height / 2; ++y) {
            uint8_t *top = packet->capture_rgba + (size_t)y * stride;
            uint8_t *bottom = packet->capture_rgba + (size_t)(height - 1 - y) * stride;
            memcpy(row, top, stride);
            memcpy(top, bottom, stride);
            memcpy(bottom, row, stride);
        }
        free(row);
    }

    plat_gl_swap(s->window);
}

const r_backend R_BACKEND_GL_API = {
    "OpenGL 4.5", gl_create, gl_destroy, gl_mesh_create, gl_mesh_destroy, gl_submit, gl_timings,
};
