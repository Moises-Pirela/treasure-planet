// What the client needs from the operating system. Only Windows is implemented.
#ifndef TP_PLATFORM_H
#define TP_PLATFORM_H

struct plat_window;

struct plat_input {
    bool keys[256];              // indexed by virtual key code: 'W', 0x20 for space...
    float mouse_dx, mouse_dy;    // counts since the last poll, while captured
    bool mouse_captured;
    bool clicked;                // the left button went down since the last poll
};

plat_window *plat_window_create(const char *title, int width, int height);
void plat_window_destroy(plat_window *window);
bool plat_window_poll(plat_window *window);      // false once the window was closed
void plat_window_size(const plat_window *window, int *width, int *height);
void plat_window_set_title(plat_window *window, const char *title);
const plat_input *plat_window_input(const plat_window *window);
void plat_window_capture_mouse(plat_window *window, bool capture);

void *plat_window_native(const plat_window *window);
void *plat_instance_native(void);

double plat_seconds(void);

// OpenGL 4.5 core on the window. Everything is process-wide: one window, one context.
bool plat_gl_create(plat_window *window);
void plat_gl_destroy(plat_window *window);
void *plat_gl_proc(const char *name);
void plat_gl_swap(plat_window *window);

#endif
