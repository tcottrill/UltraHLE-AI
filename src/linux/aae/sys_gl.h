// The GLX half of AAE's system/graphics/sys_gl.h: an OpenGL core context on
// the X11 window LinuxWindow creates (sys_gl_glx.cpp).
#pragma once

#include <GL/glew.h>
#include "sys_log.h"

// LinuxWindow::Create() calls this with its Display*/Window. Typed
// void*/unsigned long so this header stays free of Xlib (Bool, None, ...).
void sys_gl_set_x11_target(void* display, unsigned long window);

// Call BEFORE creating the X window, and create it with the returned visual.
// Returns an XVisualInfo* the caller must XFree(), or nullptr on failure.
void* sys_gl_choose_x11_visual(void* display, int screen, int enableMultisample);

// Creates the context and makes it current on the CALLING thread.
bool InitOpenGLContext(bool forceLegacyGL2, bool enableMultisample = false, bool useCoreProfile = false);
void DeleteGLContext();
bool sys_gl_context_current();   // UltraHLE: the context is current on this thread
void* sys_gl_new_context();      // UltraHLE: a second context (ROM picker), current here
bool sys_gl_bind(void* ctx);     // UltraHLE: make that one current on this thread

void glchain_set_vsync(bool enabled);
void glchain_swap_buffers();
