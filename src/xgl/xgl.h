// Public hooks of the OpenGL x_* backend used by the Win32 GUI (ultrahle.c).
#ifndef XGL_H
#define XGL_H

#ifdef __cplusplus
extern "C" {
#endif

// Creates the (hidden) game display window. Call once from the GUI thread,
// after the main window exists. x_open() shows it, x_close() hides it.
void  xgl_createwindow(void *hinstance,void *owner);

// The game display window (HWND), or NULL before xgl_createwindow().
void *xgl_window(void);

#ifdef __cplusplus
}
#endif

#endif
