// -----------------------------------------------------------------------------
// The GLX half of AAE's system/graphics/sys_gl.cpp (AAE - Another Arcade
// Emulator, Copyright (C) 2025-2026 Tim Cottrill, GNU GPL v3 or later):
// framebuffer config, core profile context, vsync and swap on the X11 window
// LinuxWindow creates. Unchanged except: the capability report is a log line,
// and sys_gl_context_current() was added (UltraHLE renders on its emulation
// thread, which makes the context current there).
// -----------------------------------------------------------------------------

#include <GL/glew.h>
#include <GL/glx.h>
#include "sys_log.h"
#include "sys_gl.h"

// The X11 display/window LinuxWindow hands us via sys_gl_set_x11_target().
static Display*    gDpy = nullptr;
static GLXDrawable gWin = 0;
static GLXContext  gCtx = nullptr;
static GLXFBConfig gFbc = nullptr;   // chosen by sys_gl_choose_x11_visual()

// -----------------------------------------------------------------------------
// sys_gl_choose_x11_visual
//
// MUST be called BEFORE the X window is created, and the returned visual MUST
// be the one XCreateWindow uses (with a colormap made from it).
//
// GLX requires the drawable's visual to be compatible with the context's
// framebuffer config. Creating the window with CopyFromParent and *then*
// picking an FBConfig independently is the classic way to get a window that
// makes a context, reports no error, swaps happily - and displays nothing.
// -----------------------------------------------------------------------------
void* sys_gl_choose_x11_visual(void* display, int screen, int enableMultisample)
{
	Display* dpy = static_cast<Display*>(display);
	if (!dpy) return nullptr;

	int fbAttribs[] = {
		GLX_X_RENDERABLE,  True,
		GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
		GLX_RENDER_TYPE,   GLX_RGBA_BIT,
		GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
		GLX_RED_SIZE,      8,
		GLX_GREEN_SIZE,    8,
		GLX_BLUE_SIZE,     8,
		GLX_ALPHA_SIZE,    8,
		GLX_DEPTH_SIZE,    24,
		GLX_STENCIL_SIZE,  8,
		GLX_DOUBLEBUFFER,  True,
		GLX_SAMPLE_BUFFERS, enableMultisample ? 1 : 0,
		GLX_SAMPLES,        enableMultisample ? 4 : 0,
		None
	};

	int fbCount = 0;
	GLXFBConfig* fbc = glXChooseFBConfig(dpy, screen, fbAttribs, &fbCount);
	if ((!fbc || fbCount == 0) && enableMultisample) {
		// MSAA is optional - retry without rather than failing outright.
		LOG_INFO("GLX: no multisample framebuffer config, retrying without MSAA");
		fbAttribs[24] = 0;   // GLX_SAMPLE_BUFFERS value
		fbAttribs[26] = 0;   // GLX_SAMPLES value
		fbc = glXChooseFBConfig(dpy, screen, fbAttribs, &fbCount);
	}
	if (!fbc || fbCount == 0) {
		LOG_ERROR("GLX: no suitable framebuffer config found");
		return nullptr;
	}

	gFbc = fbc[0];
	XVisualInfo* vi = glXGetVisualFromFBConfig(dpy, gFbc);
	XFree(fbc);

	if (!vi) {
		LOG_ERROR("GLX: glXGetVisualFromFBConfig returned no visual");
		return nullptr;
	}

	LOG_INFO("GLX: chose visual 0x%lx depth %d for the window",
	         (unsigned long)vi->visualid, vi->depth);
	return vi;
}

void sys_gl_set_x11_target(void* display, unsigned long window)
{
	gDpy = static_cast<Display*>(display);
	gWin = static_cast<GLXDrawable>(window);
}

// -----------------------------------------------------------------------------
// InitOpenGLContext (GLX)
//
// LinuxWindow must have called sys_gl_set_x11_target() first.
//
// The one thing that MUST NOT be simplified: glXCreateContextAttribsARB is an
// EXTENSION entry point and has to be resolved through glXGetProcAddressARB.
// Calling glXCreateContext() instead compiles and runs, but silently yields a
// legacy compatibility context - and the renderer's "#version 330 core"
// shaders then fail at draw time, far away from the real cause.
// -----------------------------------------------------------------------------
bool InitOpenGLContext(bool forceLegacyGL2, bool enableMultisample, bool useCoreProfile)
{
	if (!gDpy || !gWin) {
		LOG_ERROR("InitOpenGLContext: no X11 target - LinuxWindow must call "
		          "sys_gl_set_x11_target() before this");
		return false;
	}

	if (!gFbc) {
		LOG_ERROR("InitOpenGLContext: no framebuffer config - "
		          "sys_gl_choose_x11_visual() must run before the window is created");
		return false;
	}
	GLXFBConfig chosen = gFbc;

	typedef GLXContext (*PFNGLXCREATECONTEXTATTRIBSARB)(Display*, GLXFBConfig, GLXContext, Bool, const int*);
	PFNGLXCREATECONTEXTATTRIBSARB createContextAttribs =
		(PFNGLXCREATECONTEXTATTRIBSARB)glXGetProcAddressARB(
			(const GLubyte*)"glXCreateContextAttribsARB");

	if (!createContextAttribs) {
		LOG_ERROR("GLX: glXCreateContextAttribsARB unavailable - cannot create a "
		          "core profile context, and the 330 core shaders require one");
		return false;
	}

	// Primary target, then 3.3, then legacy.
	const int wantMajor = forceLegacyGL2 ? 2 : 4;
	const int wantMinor = forceLegacyGL2 ? 1 : 2;

	int attribs[] = {
		GLX_CONTEXT_MAJOR_VERSION_ARB, wantMajor,
		GLX_CONTEXT_MINOR_VERSION_ARB, wantMinor,
		GLX_CONTEXT_PROFILE_MASK_ARB,
			useCoreProfile ? GLX_CONTEXT_CORE_PROFILE_BIT_ARB
			               : GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB,
		None
	};

	gCtx = createContextAttribs(gDpy, chosen, nullptr, True, attribs);

	if (!gCtx && !forceLegacyGL2) {
		LOG_INFO("GLX: %d.%d context refused, falling back to 3.3", wantMajor, wantMinor);
		attribs[1] = 3;
		attribs[3] = 3;
		gCtx = createContextAttribs(gDpy, chosen, nullptr, True, attribs);
	}
	if (!gCtx) {
		LOG_ERROR("GLX: could not create an OpenGL context");
		return false;
	}

	if (!glXMakeCurrent(gDpy, gWin, gCtx)) {
		LOG_ERROR("GLX: glXMakeCurrent failed");
		glXDestroyContext(gDpy, gCtx);
		gCtx = nullptr;
		return false;
	}

	glewExperimental = GL_TRUE;   // required for core profiles
	GLenum err = glewInit();
	if (err != GLEW_OK) {
		LOG_ERROR("GLEW init failed: %s", glewGetErrorString(err));
		return false;
	}
	// A core profile makes glewInit leave a spurious GL_INVALID_ENUM behind.
	glGetError();

	if (enableMultisample)
		glEnable(GL_MULTISAMPLE);

	LOG_INFO("GLX: context %s / %s / %s", (const char*)glGetString(GL_VENDOR),
	         (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION));
	return true;
}

void DeleteGLContext()
{
	if (gDpy && gCtx) {
		glXMakeCurrent(gDpy, 0, nullptr);
		glXDestroyContext(gDpy, gCtx);
		gCtx = nullptr;
	}
}

bool sys_gl_context_current()
{
	return gCtx && glXGetCurrentContext() == gCtx;
}

// UltraHLE: a second, separate core context on the same window, for the
// ROM picker on the main thread (the emulator's own context belongs to the
// emulation thread). Made current on the calling thread; GLEW is loaded
// for it. Returns nullptr on failure.
void* sys_gl_new_context()
{
	if (!gDpy || !gWin || !gFbc) return nullptr;

	typedef GLXContext (*PFNGLXCREATECONTEXTATTRIBSARB)(Display*, GLXFBConfig, GLXContext, Bool, const int*);
	PFNGLXCREATECONTEXTATTRIBSARB createContextAttribs =
		(PFNGLXCREATECONTEXTATTRIBSARB)glXGetProcAddressARB(
			(const GLubyte*)"glXCreateContextAttribsARB");
	if (!createContextAttribs) return nullptr;

	int attribs[] = {
		GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
		GLX_CONTEXT_MINOR_VERSION_ARB, 3,
		GLX_CONTEXT_PROFILE_MASK_ARB,  GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
		None
	};
	GLXContext ctx = createContextAttribs(gDpy, gFbc, nullptr, True, attribs);
	if (!ctx) {
		LOG_ERROR("GLX: could not create the picker's context");
		return nullptr;
	}
	if (!glXMakeCurrent(gDpy, gWin, ctx)) {
		glXDestroyContext(gDpy, ctx);
		return nullptr;
	}
	glewExperimental = GL_TRUE;
	if (glewInit() != GLEW_OK) {
		LOG_ERROR("GLEW init failed for the picker's context");
		glXMakeCurrent(gDpy, 0, nullptr);
		glXDestroyContext(gDpy, ctx);
		return nullptr;
	}
	glGetError();
	return ctx;
}

// UltraHLE: makes a context from sys_gl_new_context current on this thread
bool sys_gl_bind(void* ctx)
{
	return gDpy && gWin && glXMakeCurrent(gDpy, gWin, (GLXContext)ctx);
}

void sys_gl_destroy_context(void* context)
{
	GLXContext ctx = static_cast<GLXContext>(context);
	if (!gDpy || !ctx || ctx == gCtx) return;
	if (glXGetCurrentContext() == ctx)
		glXMakeCurrent(gDpy, 0, nullptr);
	glXDestroyContext(gDpy, ctx);
}

void glchain_set_vsync(bool enabled)
{
	if (!gDpy || !gWin) return;

	typedef void (*PFNGLXSWAPINTERVALEXT)(Display*, GLXDrawable, int);
	static PFNGLXSWAPINTERVALEXT swapIntervalEXT =
		(PFNGLXSWAPINTERVALEXT)glXGetProcAddressARB((const GLubyte*)"glXSwapIntervalEXT");

	if (swapIntervalEXT) {
		swapIntervalEXT(gDpy, gWin, enabled ? 1 : 0);
		return;
	}

	// MESA and SGI fallbacks, in that order.
	typedef int (*PFNGLXSWAPINTERVALINT)(int);
	static PFNGLXSWAPINTERVALINT swapIntervalMESA =
		(PFNGLXSWAPINTERVALINT)glXGetProcAddressARB((const GLubyte*)"glXSwapIntervalMESA");
	if (swapIntervalMESA) { swapIntervalMESA(enabled ? 1 : 0); return; }

	static PFNGLXSWAPINTERVALINT swapIntervalSGI =
		(PFNGLXSWAPINTERVALINT)glXGetProcAddressARB((const GLubyte*)"glXSwapIntervalSGI");
	if (swapIntervalSGI) { swapIntervalSGI(enabled ? 1 : 0); return; }

	// Say so rather than silently doing nothing - under a compositor vsync is
	// often forced on regardless, and that is worth knowing when timing looks odd.
	LOG_INFO("SetvSync: no GLX swap-control extension; vsync is compositor-controlled");
}

void glchain_swap_buffers()
{
	// The first three swaps are logged and no more.
	static int s_swapCount = 0;
	if (s_swapCount < 3) {
		++s_swapCount;
		LOG_INFO("GLSwapBuffers #%d (dpy=%p win=%lu)", s_swapCount,
		         (void*)gDpy, (unsigned long)gWin);
	}

	if (gDpy && gWin)
		glXSwapBuffers(gDpy, gWin);
	else
		LOG_ERROR("GLSwapBuffers called with no X11 target");
}
