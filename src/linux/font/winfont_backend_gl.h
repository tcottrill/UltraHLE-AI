// Adapted from SpriteTest sys_font/winfont_backend_gl.h; see README.md provenance.
// OpenGL 3.3 core backend. All resource calls require the owning context current.
#pragma once

#include <GL/glew.h>
#include "winfont.h"

namespace winfont
{
// Configuration is reserved for source compatibility. Shaders are embedded;
// batches use ordinary GL_STREAM_DRAW storage, with no capacity configuration.
struct GLFontBackendCreateInfo {};

class GLFontBackend : public IFontBackend
{
public:
    GLFontBackend() = default;
    ~GLFontBackend() override;
    GLFontBackend(const GLFontBackend&) = delete;
    GLFontBackend& operator=(const GLFontBackend&) = delete;

    bool Init(const GLFontBackendCreateInfo* ci = nullptr);
    void Shutdown();
    void OnFrameBegin() {} // Driver-managed replacement makes a frame reset unnecessary.

    AtlasId CreateAtlas(const uint8_t* pixels, int width, int height) override;
    void DestroyAtlas(AtlasId atlas) override;
    void BindAtlas(AtlasId atlas) override;
    void BeginPass(const Mat4& mvp, AtlasId atlas) override;
    void DrawGeometry(const FontVertex* vertices, size_t count) override;
    void EndPass() override;
    void SetScissor(int x, int y, int w, int h) override;
    void DisableScissor() override;
    void SetFramebufferScale(float sx, float sy) override;
    void SetViewportOrigin(int x, int y);

private:
    bool CreatePipeline();
    GLuint m_prog = 0, m_vao = 0, m_vbo = 0, m_currentAtlas = 0;
    GLint m_uMVP = -1, m_uTex = -1;
    bool m_passOpen = false;
    float m_fbScaleX = 1, m_fbScaleY = 1;
    int m_viewportX = 0, m_viewportY = 0;

    struct SavedState {
        GLint program = 0, vao = 0, arrayBuffer = 0, activeTexture = 0;
        GLint texture0 = 0, sampler0 = 0;
        GLint equationRGB = 0, equationAlpha = 0;
        GLint srcRGB = 0, dstRGB = 0, srcAlpha = 0, dstAlpha = 0;
        GLboolean depth = GL_FALSE, cull = GL_FALSE, blend = GL_FALSE, scissor = GL_FALSE;
        GLint scissorBox[4]{}, viewport[4]{};
    } m_saved;
};

} // namespace winfont
