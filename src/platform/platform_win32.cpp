#include "platform.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct plat_window {
    HWND hwnd;
    HDC gl_dc;
    HGLRC gl_context;
    plat_input input;
    bool closed;
};

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    plat_window *window = (plat_window *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (window) switch (message) {
    case WM_CLOSE:
        window->closed = true;
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (wparam < 256) window->input.keys[wparam] = true;
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (wparam < 256) window->input.keys[wparam] = false;
        break;
    case WM_LBUTTONDOWN:
        window->input.clicked = true;
        break;
    case WM_KILLFOCUS:
        memset(window->input.keys, 0, sizeof(window->input.keys));
        plat_window_capture_mouse(window, false);
        break;
    case WM_INPUT: {
        RAWINPUT raw;
        UINT size = sizeof(raw);
        if (GetRawInputData((HRAWINPUT)lparam, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1
            && raw.header.dwType == RIM_TYPEMOUSE && window->input.mouse_captured) {
            window->input.mouse_dx += (float)raw.data.mouse.lLastX;
            window->input.mouse_dy += (float)raw.data.mouse.lLastY;
        }
        break;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

plat_window *plat_window_create(const char *title, int width, int height) {
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_OWNDC;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    window_class.lpszClassName = L"TreasurePlanet";
    RegisterClassExW(&window_class);

    RECT rect = {0, 0, width, height};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExA(0, "TreasurePlanet", title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, instance, NULL);
    if (!hwnd) return NULL;

    plat_window *window = (plat_window *)calloc(1, sizeof(plat_window));
    window->hwnd = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)window);

    RAWINPUTDEVICE mouse = {};
    mouse.usUsagePage = 0x01;   // generic desktop
    mouse.usUsage = 0x02;       // mouse
    mouse.hwndTarget = hwnd;
    RegisterRawInputDevices(&mouse, 1, sizeof(mouse));

    ShowWindow(hwnd, SW_SHOW);
    return window;
}

void plat_window_destroy(plat_window *window) {
    if (!window) return;
    plat_window_capture_mouse(window, false);
    DestroyWindow(window->hwnd);
    free(window);
}

bool plat_window_poll(plat_window *window) {
    window->input.mouse_dx = window->input.mouse_dy = 0.0f;
    window->input.clicked = false;
    MSG message;
    while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (IsIconic(window->hwnd)) Sleep(10);    // minimised: nothing presents, so nothing paces the loop
    if (window->input.mouse_captured) {
        // Keep the invisible cursor from leaving: the window can move or resize.
        RECT rect;
        GetClientRect(window->hwnd, &rect);
        MapWindowPoints(window->hwnd, NULL, (POINT *)&rect, 2);
        ClipCursor(&rect);
    }
    return !window->closed;
}

void plat_window_size(const plat_window *window, int *width, int *height) {
    RECT rect;
    GetClientRect(window->hwnd, &rect);
    *width = rect.right - rect.left;
    *height = rect.bottom - rect.top;
}

void plat_window_set_title(plat_window *window, const char *title) { SetWindowTextA(window->hwnd, title); }
const plat_input *plat_window_input(const plat_window *window) { return &window->input; }

void plat_window_capture_mouse(plat_window *window, bool capture) {
    if (window->input.mouse_captured == capture) return;
    if (capture && GetForegroundWindow() != window->hwnd) return;    // never trap a mouse in use elsewhere
    window->input.mouse_captured = capture;
    ShowCursor(capture ? FALSE : TRUE);
    if (!capture) ClipCursor(NULL);
}

void *plat_window_native(const plat_window *window) { return window->hwnd; }
void *plat_instance_native(void) { return GetModuleHandleW(NULL); }

double plat_seconds(void) {
    static LARGE_INTEGER frequency;
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)frequency.QuadPart;
}

// ---------------------------------------------------------------------------
// OpenGL
// ---------------------------------------------------------------------------
typedef HGLRC(WINAPI *create_context_attribs_fn)(HDC, HGLRC, const int *);
typedef BOOL(WINAPI *swap_interval_fn)(int);

enum {
    WGL_CONTEXT_MAJOR_VERSION_ARB = 0x2091,
    WGL_CONTEXT_MINOR_VERSION_ARB = 0x2092,
    WGL_CONTEXT_FLAGS_ARB = 0x2094,
    WGL_CONTEXT_DEBUG_BIT_ARB = 0x0001,
    WGL_CONTEXT_PROFILE_MASK_ARB = 0x9126,
    WGL_CONTEXT_CORE_PROFILE_BIT_ARB = 0x0001,
};

bool plat_gl_create(plat_window *window) {
    window->gl_dc = GetDC(window->hwnd);
    PIXELFORMATDESCRIPTOR format = {};
    format.nSize = sizeof(format);
    format.nVersion = 1;
    format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    format.iPixelType = PFD_TYPE_RGBA;
    format.cColorBits = 32;
    int index = ChoosePixelFormat(window->gl_dc, &format);
    if (!index || !SetPixelFormat(window->gl_dc, index, &format)) return false;

    // A legacy context exists only to fetch the function that creates the real one.
    HGLRC legacy = wglCreateContext(window->gl_dc);
    if (!legacy || !wglMakeCurrent(window->gl_dc, legacy)) return false;
    create_context_attribs_fn create = (create_context_attribs_fn)(void *)wglGetProcAddress("wglCreateContextAttribsARB");
    swap_interval_fn swap_interval = (swap_interval_fn)(void *)wglGetProcAddress("wglSwapIntervalEXT");
    int attributes[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
        WGL_CONTEXT_MINOR_VERSION_ARB, 5,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
#ifndef NDEBUG
        WGL_CONTEXT_FLAGS_ARB, WGL_CONTEXT_DEBUG_BIT_ARB,
#endif
        0,
    };
    window->gl_context = create ? create(window->gl_dc, NULL, attributes) : NULL;
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(legacy);
    if (!window->gl_context || !wglMakeCurrent(window->gl_dc, window->gl_context)) return false;
    if (swap_interval) swap_interval(1);
    return true;
}

void plat_gl_destroy(plat_window *window) {
    wglMakeCurrent(NULL, NULL);
    if (window->gl_context) wglDeleteContext(window->gl_context);
    if (window->gl_dc) ReleaseDC(window->hwnd, window->gl_dc);
    window->gl_context = NULL;
    window->gl_dc = NULL;
}

void *plat_gl_proc(const char *name) {
    void *proc = (void *)wglGetProcAddress(name);
    if (proc) return proc;
    // OpenGL 1.1 functions come from the DLL itself.
    static HMODULE opengl32 = LoadLibraryW(L"opengl32.dll");
    return (void *)GetProcAddress(opengl32, name);
}

void plat_gl_swap(plat_window *window) { SwapBuffers(window->gl_dc); }
