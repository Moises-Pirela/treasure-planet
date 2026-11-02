// OpenGL 4.5 core, direct state access. Draws into its own framebuffer, because the
// window's default one cannot be asked for a floating-point depth buffer, and reversed
// depth needs one to be worth anything. The result is blitted to the window.
#include "backend.h"
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
    X(PFNGLCHECKNAMEDFRAMEBUFFERSTATUSPROC, glCheckNamedFramebufferStatus) \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLBLITNAMEDFRAMEBUFFERPROC, glBlitNamedFramebuffer) \
    X(PFNGLCLEARNAMEDFRAMEBUFFERFVPROC, glClearNamedFramebufferfv) \
    X(PFNGLCLIPCONTROLPROC, glClipControl) \
    X(PFNGLDEBUGMESSAGECALLBACKPROC, glDebugMessageCallback) \
    X(PFNGLVIEWPORTPROC, glViewport) \
    X(PFNGLENABLEPROC, glEnable) \
    X(PFNGLDEPTHFUNCPROC, glDepthFunc) \
    X(PFNGLCULLFACEPROC, glCullFace) \
    X(PFNGLFRONTFACEPROC, glFrontFace) \
    X(PFNGLDRAWELEMENTSPROC, glDrawElements) \
    X(PFNGLREADPIXELSPROC, glReadPixels) \
    X(PFNGLPIXELSTOREIPROC, glPixelStorei)

#define DECLARE(type, name) static type name;
GL_FUNCTIONS(DECLARE)
#undef DECLARE

static const char MESH_GLSL[] =
#include "mesh.glsl.inl"
    ;

struct gl_mesh {
    GLuint vertex_array, vertex_buffer, index_buffer;
    GLsizei index_count;
};

#define GL_MAX_MESHES 256

struct gl_state {
    plat_window *window;
    GLuint program, frame_uniforms;
    GLuint framebuffer, color_target, depth_target;
    int width, height;
    gl_mesh meshes[GL_MAX_MESHES];    // slot 0 stays empty: mesh 0 means none
};

#ifndef NDEBUG
static void APIENTRY gl_debug(GLenum, GLenum type, GLuint, GLenum, GLsizei, const GLchar *message, const void *) {
    if (type == GL_DEBUG_TYPE_ERROR) fprintf(stderr, "opengl: %s\n", message);
}
#endif

static GLuint compile_stage(GLenum stage, const char *defines) {
    const char *sources[] = {"#version 450 core\n", defines, MESH_GLSL};
    GLuint shader = glCreateShader(stage);
    glShaderSource(shader, 3, sources, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "opengl: %s shader: %s\n", defines, log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint create_program(void) {
    GLuint vertex = compile_stage(GL_VERTEX_SHADER, "#define VERTEX\n");
    GLuint fragment = compile_stage(GL_FRAGMENT_SHADER, "#define FRAGMENT\n");
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
    if (!s->framebuffer) return;
    glDeleteFramebuffers(1, &s->framebuffer);
    glDeleteRenderbuffers(1, &s->color_target);
    glDeleteRenderbuffers(1, &s->depth_target);
    s->framebuffer = s->color_target = s->depth_target = 0;
}

static void resize_targets(gl_state *s, int width, int height) {
    if (s->framebuffer && s->width == width && s->height == height) return;
    destroy_targets(s);
    s->width = width;
    s->height = height;
    glCreateRenderbuffers(1, &s->color_target);
    glCreateRenderbuffers(1, &s->depth_target);
    glNamedRenderbufferStorage(s->color_target, GL_RGBA8, width, height);
    glNamedRenderbufferStorage(s->depth_target, GL_DEPTH_COMPONENT32F, width, height);
    glCreateFramebuffers(1, &s->framebuffer);
    glNamedFramebufferRenderbuffer(s->framebuffer, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, s->color_target);
    glNamedFramebufferRenderbuffer(s->framebuffer, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s->depth_target);
    if (glCheckNamedFramebufferStatus(s->framebuffer, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        fprintf(stderr, "opengl: framebuffer incomplete\n");
}

static void gl_destroy(void *state);

static void *gl_create(plat_window *window) {
    if (!plat_gl_create(window)) return NULL;
#define LOAD(type, name) name = (type)plat_gl_proc(#name); \
    if (!name) { fprintf(stderr, "opengl: missing %s\n", #name); plat_gl_destroy(window); return NULL; }
    GL_FUNCTIONS(LOAD)
#undef LOAD

    gl_state *s = (gl_state *)calloc(1, sizeof(gl_state));
    s->window = window;
#ifndef NDEBUG
    glEnable(GL_DEBUG_OUTPUT);
    glDebugMessageCallback(gl_debug, NULL);
#endif
    s->program = create_program();
    if (!s->program) {
        gl_destroy(s);
        return NULL;
    }
    glCreateBuffers(1, &s->frame_uniforms);
    glNamedBufferStorage(s->frame_uniforms, sizeof(r_frame_uniforms), NULL, GL_DYNAMIC_STORAGE_BIT);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, s->frame_uniforms);

    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GREATER);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glUseProgram(s->program);
    return s;
}

static void gl_mesh_destroy(void *state, r_mesh mesh) {
    gl_state *s = (gl_state *)state;
    if (!mesh || mesh >= GL_MAX_MESHES || !s->meshes[mesh].vertex_array) return;
    gl_mesh *m = &s->meshes[mesh];
    glDeleteVertexArrays(1, &m->vertex_array);
    glDeleteBuffers(1, &m->vertex_buffer);
    glDeleteBuffers(1, &m->index_buffer);
    memset(m, 0, sizeof(*m));
}

static void gl_destroy(void *state) {
    gl_state *s = (gl_state *)state;
    for (r_mesh i = 1; i < GL_MAX_MESHES; ++i) gl_mesh_destroy(s, i);
    destroy_targets(s);
    if (s->frame_uniforms) glDeleteBuffers(1, &s->frame_uniforms);
    if (s->program) glDeleteProgram(s->program);
    plat_gl_destroy(s->window);
    free(s);
}

static r_mesh gl_mesh_create(void *state, const r_vertex *vertices, uint32_t vertex_count,
                             const uint32_t *indices, uint32_t index_count) {
    gl_state *s = (gl_state *)state;
    r_mesh slot = 1;
    while (slot < GL_MAX_MESHES && s->meshes[slot].vertex_array) ++slot;
    if (slot == GL_MAX_MESHES) return 0;
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

static void gl_submit(void *state, const r_frame *frame, int width, int height) {
    gl_state *s = (gl_state *)state;
    resize_targets(s, width, height);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s->framebuffer);
    glViewport(0, 0, width, height);
    const GLfloat color[4] = {frame->clear[0], frame->clear[1], frame->clear[2], 1.0f};
    const GLfloat depth = 0.0f;
    glClearNamedFramebufferfv(s->framebuffer, GL_COLOR, 0, color);
    glClearNamedFramebufferfv(s->framebuffer, GL_DEPTH, 0, &depth);
    glNamedBufferSubData(s->frame_uniforms, 0, sizeof(frame->uniforms), &frame->uniforms);

    for (size_t i = 0; i < frame->draw_count; ++i) {
        const r_draw *draw = &frame->draws[i];
        if (!draw->mesh || draw->mesh >= GL_MAX_MESHES) continue;
        const gl_mesh *m = &s->meshes[draw->mesh];
        if (!m->vertex_array) continue;
        glProgramUniformMatrix4fv(s->program, 0, 1, GL_FALSE, draw->model.m);
        glProgramUniform4fv(s->program, 4, 1, draw->color);
        glBindVertexArray(m->vertex_array);
        glDrawElements(GL_TRIANGLES, m->index_count, GL_UNSIGNED_INT, NULL);
    }

    if (frame->capture_rgba) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s->framebuffer);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, frame->capture_rgba);
        // GL's first row is the bottom one.
        size_t stride = (size_t)width * 4;
        uint8_t *row = (uint8_t *)malloc(stride);
        for (int y = 0; y < height / 2; ++y) {
            uint8_t *top = frame->capture_rgba + (size_t)y * stride;
            uint8_t *bottom = frame->capture_rgba + (size_t)(height - 1 - y) * stride;
            memcpy(row, top, stride);
            memcpy(top, bottom, stride);
            memcpy(bottom, row, stride);
        }
        free(row);
    }

    glBlitNamedFramebuffer(s->framebuffer, 0, 0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    plat_gl_swap(s->window);
}

const r_backend R_BACKEND_GL_API = {
    "OpenGL 4.5", gl_create, gl_destroy, gl_mesh_create, gl_mesh_destroy, gl_submit,
};
