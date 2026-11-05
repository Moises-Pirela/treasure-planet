#include "platform.h"
#include <volk.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <dlfcn.h>
#endif

#define PLAT_TITLE_CAPACITY 256

struct plat_window {
    GLFWwindow *handle;
    plat_input input;
    char title[PLAT_TITLE_CAPACITY];
    double cursor_x, cursor_y;
};

static bool vulkan_available = false;

static void on_error(int code, const char *message) { fprintf(stderr, "glfw: %s (%d)\n", message, code); }

static void on_key(GLFWwindow *handle, int key, int, int action, int) {
    plat_window *window = (plat_window *)glfwGetWindowUserPointer(handle);
    if (key < 0 || key >= PLAT_KEY_COUNT) return;
    window->input.keys[key] = action != GLFW_RELEASE;
    if (action == GLFW_PRESS) window->input.pressed[key] = true;
}

static void on_mouse_button(GLFWwindow *handle, int button, int action, int) {
    plat_window *window = (plat_window *)glfwGetWindowUserPointer(handle);
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) window->input.clicked = true;
}

static void on_cursor(GLFWwindow *handle, double x, double y) {
    plat_window *window = (plat_window *)glfwGetWindowUserPointer(handle);
    if (window->input.mouse_captured) {
        window->input.mouse_dx += (float)(x - window->cursor_x);
        window->input.mouse_dy += (float)(y - window->cursor_y);
    }
    window->cursor_x = x;
    window->cursor_y = y;
}

static void on_scroll(GLFWwindow *handle, double, double y) {
    plat_window *window = (plat_window *)glfwGetWindowUserPointer(handle);
    window->input.scroll += (float)y;
}

static void on_focus(GLFWwindow *handle, int focused) {
    plat_window *window = (plat_window *)glfwGetWindowUserPointer(handle);
    if (focused) return;
    memset(window->input.keys, 0, sizeof(window->input.keys));
    plat_window_capture_mouse(window, false);
}

// Opens the GLFW window behind a plat_window, hidden, and with a context if asked.
static bool open_handle(plat_window *window, int width, int height, bool opengl) {
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    if (opengl) {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_DEPTH_BITS, 0);     // the GL backend draws into its own framebuffer
        glfwWindowHint(GLFW_STENCIL_BITS, 0);
#ifndef NDEBUG
        glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
#endif
    } else {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    }
    window->handle = glfwCreateWindow(width, height, window->title, NULL, NULL);
    if (!window->handle) return false;
    glfwSetWindowUserPointer(window->handle, window);
    glfwSetKeyCallback(window->handle, on_key);
    glfwSetMouseButtonCallback(window->handle, on_mouse_button);
    glfwSetCursorPosCallback(window->handle, on_cursor);
    glfwSetScrollCallback(window->handle, on_scroll);
    glfwSetWindowFocusCallback(window->handle, on_focus);
    return true;
}

static bool load_vulkan(void) {
    if (volkInitialize() == VK_SUCCESS) return true;
#ifdef __APPLE__
    // Homebrew's directory on Apple Silicon is not on dlopen's search path, and
    // MoltenVK works on its own when the Vulkan loader is not installed.
    static const char *const PATHS[] = {
        "/opt/homebrew/lib/libvulkan.1.dylib",
        "/opt/homebrew/lib/libMoltenVK.dylib",
        "/usr/local/lib/libMoltenVK.dylib",
    };
    for (size_t i = 0; i < sizeof(PATHS) / sizeof(PATHS[0]); ++i) {
        void *module = dlopen(PATHS[i], RTLD_NOW | RTLD_LOCAL);
        if (!module) continue;
        void *entry = dlsym(module, "vkGetInstanceProcAddr");
        if (entry) {
            volkInitializeCustom((PFN_vkGetInstanceProcAddr)entry);
            return true;
        }
    }
#endif
    return false;
}

plat_window *plat_window_create(const char *title, int width, int height) {
    glfwSetErrorCallback(on_error);
    vulkan_available = load_vulkan();
    if (vulkan_available) glfwInitVulkanLoader(vkGetInstanceProcAddr);
    if (!glfwInit()) return NULL;

    plat_window *window = (plat_window *)calloc(1, sizeof(plat_window));
    strncpy(window->title, title, PLAT_TITLE_CAPACITY - 1);
    if (!open_handle(window, width, height, false)) {
        free(window);
        glfwTerminate();
        return NULL;
    }
    return window;
}

void plat_window_destroy(plat_window *window) {
    if (!window) return;
    if (window->handle) glfwDestroyWindow(window->handle);
    free(window);
    glfwTerminate();
}

void plat_window_show(plat_window *window) { glfwShowWindow(window->handle); }

static bool vsync_on = true;
void plat_set_vsync(bool on) { vsync_on = on; }
bool plat_vsync(void) { return vsync_on; }

bool plat_window_poll(plat_window *window) {
    window->input.mouse_dx = window->input.mouse_dy = 0.0f;
    window->input.clicked = false;
    window->input.scroll = 0.0f;
    memset(window->input.pressed, 0, sizeof(window->input.pressed));
    // Minimised: nothing presents, so nothing paces the loop.
    if (glfwGetWindowAttrib(window->handle, GLFW_ICONIFIED)) glfwWaitEventsTimeout(0.01);
    else glfwPollEvents();
    // The pointer, in the window's own units (which a high-density display's framebuffer is not).
    int width = 0, height = 0;
    glfwGetWindowSize(window->handle, &width, &height);
    if (width > 0 && height > 0) {
        window->input.cursor_x = (float)(window->cursor_x / (double)width);
        window->input.cursor_y = (float)(window->cursor_y / (double)height);
    }
    return !glfwWindowShouldClose(window->handle);
}

void plat_window_size(const plat_window *window, int *width, int *height) {
    glfwGetFramebufferSize(window->handle, width, height);
}

void plat_window_set_title(plat_window *window, const char *title) {
    strncpy(window->title, title, PLAT_TITLE_CAPACITY - 1);
    glfwSetWindowTitle(window->handle, window->title);
}

const plat_input *plat_window_input(const plat_window *window) { return &window->input; }

void plat_window_capture_mouse(plat_window *window, bool capture) {
    if (window->input.mouse_captured == capture) return;
    // Never take a mouse that is in use elsewhere.
    if (capture && !glfwGetWindowAttrib(window->handle, GLFW_FOCUSED)) return;
    window->input.mouse_captured = capture;
    glfwSetInputMode(window->handle, GLFW_CURSOR, capture ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    if (glfwRawMouseMotionSupported())
        glfwSetInputMode(window->handle, GLFW_RAW_MOUSE_MOTION, capture ? GLFW_TRUE : GLFW_FALSE);
    glfwGetCursorPos(window->handle, &window->cursor_x, &window->cursor_y);   // no jump on the first move
}

double plat_seconds(void) { return glfwGetTime(); }

// ---------------------------------------------------------------------------
// Vulkan
// ---------------------------------------------------------------------------
bool plat_vk_available(void) { return vulkan_available && glfwVulkanSupported(); }

const char **plat_vk_instance_extensions(uint32_t *count) { return glfwGetRequiredInstanceExtensions(count); }

bool plat_vk_create_surface(plat_window *window, void *instance, void *surface) {
    return glfwCreateWindowSurface((VkInstance)instance, window->handle, NULL, (VkSurfaceKHR *)surface) == VK_SUCCESS;
}

// ---------------------------------------------------------------------------
// OpenGL
// ---------------------------------------------------------------------------
static bool replace_handle(plat_window *window, bool opengl) {
    int width, height;
    glfwGetWindowSize(window->handle, &width, &height);
    glfwDestroyWindow(window->handle);
    window->handle = NULL;
    return open_handle(window, width, height, opengl);
}

bool plat_gl_create(plat_window *window) {
    int width, height;
    glfwGetWindowSize(window->handle, &width, &height);
    if (!replace_handle(window, true)) {
        // Leave a window behind for the other renderer to try.
        open_handle(window, width, height, false);
        return false;
    }
    glfwMakeContextCurrent(window->handle);
    glfwSwapInterval(vsync_on ? 1 : 0);
    return true;
}

void plat_gl_destroy(plat_window *window) {
    glfwMakeContextCurrent(NULL);
    replace_handle(window, false);
}

void *plat_gl_proc(const char *name) { return (void *)glfwGetProcAddress(name); }

void plat_gl_swap(plat_window *window) { glfwSwapBuffers(window->handle); }
