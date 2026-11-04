// What the client needs from the operating system. Backed by GLFW on Windows, Linux
// and macOS. Nothing here names an API type, so neither GLFW nor Vulkan leak upward.
#ifndef TP_PLATFORM_H
#define TP_PLATFORM_H

#include <stdint.h>

struct plat_window;

// Letters and digits are their uppercase ASCII codes: keys['W'].
#define PLAT_KEY_SPACE 32
#define PLAT_KEY_ESCAPE 256
#define PLAT_KEY_F1 290
#define PLAT_KEY_F2 291
#define PLAT_KEY_F3 292
#define PLAT_KEY_F4 293
#define PLAT_KEY_LEFT_CONTROL 341
#define PLAT_KEY_COUNT 512

struct plat_input {
    bool keys[PLAT_KEY_COUNT];   // held now
    bool pressed[PLAT_KEY_COUNT];    // went down since the last poll, however briefly
    float mouse_dx, mouse_dy;    // counts since the last poll, while captured
    bool mouse_captured;
    bool clicked;                // the left button went down since the last poll
};

// The window starts hidden, so that a renderer needing to replace it (see plat_gl_create)
// does not make it flash: show it once a renderer is running.
plat_window *plat_window_create(const char *title, int width, int height);
void plat_window_destroy(plat_window *window);
void plat_window_show(plat_window *window);
// Whether a present waits for the display, for a renderer to read when it starts. On
// by default; off is for measuring what a frame really costs.
void plat_set_vsync(bool on);
bool plat_vsync(void);
bool plat_window_poll(plat_window *window);      // false once the window was closed
void plat_window_size(const plat_window *window, int *width, int *height);    // in pixels
void plat_window_set_title(plat_window *window, const char *title);
const plat_input *plat_window_input(const plat_window *window);
void plat_window_capture_mouse(plat_window *window, bool capture);

double plat_seconds(void);

// Vulkan. volk is loaded by plat_window_create, because GLFW has to be told where
// Vulkan is before it starts. Instance and surface are a VkInstance and a VkSurfaceKHR *.
bool plat_vk_available(void);
const char **plat_vk_instance_extensions(uint32_t *count);
bool plat_vk_create_surface(plat_window *window, void *instance, void *surface);

// OpenGL 4.5 core on the window. A window cannot have both an OpenGL context and a
// Vulkan surface, so these replace the underlying window: destroy any surface first.
// Fails on macOS, whose OpenGL stops at 4.1.
bool plat_gl_create(plat_window *window);
void plat_gl_destroy(plat_window *window);
void *plat_gl_proc(const char *name);
void plat_gl_swap(plat_window *window);

#endif
