// GLEW include order for the OpenGL x_* backend: Windows uses the bundled
// GLEW (with wglew for the context), Linux the system GLEW (glxew there).
#ifndef XGL_GL_H
#define XGL_GL_H

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "glew.h"
#include "wglew.h"
#else
#include <GL/glew.h>
#endif

#endif
