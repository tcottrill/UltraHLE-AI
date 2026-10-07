// Adapted from SpriteTest sys_font/winfont_backend_gl.cpp; see README.md provenance.
// GL 3.3 core: binding-based setup and one storage replacement per batch.
#include "winfont_backend_gl.h"
#include "winfont_common_glsl.h"
#include "../aae/sys_log.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace winfont
{
namespace
{
constexpr const char* vertexSource = R"glsl(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
out vec2 vUV;
out vec4 vColor;
uniform mat4 uMVP;
void main() {
    vUV = aUV;
    vColor = aColor;
    gl_Position = uMVP * vec4(aPos, 0.0, 1.0);
}
)glsl";
constexpr const char* fragmentPrelude = R"glsl(#version 330 core
in vec2 vUV;
in vec4 vColor;
uniform sampler2D uTex;
out vec4 outColor;
)glsl";
constexpr const char* fragmentMain = R"glsl(
void main() { outColor = winfont_shade(uTex, vUV, vColor); }
)glsl";

GLuint compile(GLenum type, const char* source, const char* tag)
{
    GLuint shader = glCreateShader(type);
    if (!shader) return 0;
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint length; glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        std::vector<char> log(static_cast<size_t>(std::max(length, 1)), '\0');
        glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), nullptr, log.data());
        LOG_ERROR("winfont(GL): %s shader compile failed: %s", tag, log.data());
        glDeleteShader(shader); return 0;
    }
    return shader;
}
void enable(GLenum cap, GLboolean value)
{
    if (value) glEnable(cap); else glDisable(cap);
}
}

bool GLFontBackend::CreatePipeline()
{
    std::string fragment = std::string(fragmentPrelude) + kShadeBodyGLSL + fragmentMain;
    GLuint vs = compile(GL_VERTEX_SHADER, vertexSource, "vertex");
    if (!vs) return false;
    GLuint fs = compile(GL_FRAGMENT_SHADER, fragment.c_str(), "fragment");
    if (!fs) { glDeleteShader(vs); return false; }
    m_prog = glCreateProgram();
    if (m_prog) {
        glAttachShader(m_prog, vs); glAttachShader(m_prog, fs); glLinkProgram(m_prog);
    }
    glDeleteShader(vs); glDeleteShader(fs);
    if (!m_prog) return false;
    GLint ok; glGetProgramiv(m_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint length; glGetProgramiv(m_prog, GL_INFO_LOG_LENGTH, &length);
        std::vector<char> log(static_cast<size_t>(std::max(length, 1)), '\0');
        glGetProgramInfoLog(m_prog, static_cast<GLsizei>(log.size()), nullptr, log.data());
        LOG_ERROR("winfont(GL): shader link failed: %s", log.data());
        return false;
    }
    m_uMVP = glGetUniformLocation(m_prog, "uMVP");
    m_uTex = glGetUniformLocation(m_prog, "uTex");
    GLint vao, buffer;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
    glGenVertexArrays(1, &m_vao); glGenBuffers(1, &m_vbo);
    if (m_vao && m_vbo) {
        glBindVertexArray(m_vao); glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,sizeof(FontVertex),
                              reinterpret_cast<const void*>(offsetof(FontVertex,x)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,sizeof(FontVertex),
                              reinterpret_cast<const void*>(offsetof(FontVertex,u)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2,4,GL_FLOAT,GL_FALSE,sizeof(FontVertex),
                              reinterpret_cast<const void*>(offsetof(FontVertex,r)));
    }
    glBindVertexArray(static_cast<GLuint>(vao));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(buffer));
    return m_vao && m_vbo && m_uMVP >= 0 && m_uTex >= 0;
}

GLFontBackend::~GLFontBackend()
{
    // Destructors cannot assume that the owning GL context is current.
    if (m_prog || m_vao || m_vbo)
        LOG_ERROR("winfont(GL): live objects at destruction; call Shutdown in the owning context");
}
bool GLFontBackend::Init(const GLFontBackendCreateInfo*)
{
    if (m_prog && m_vao && m_vbo) return true;
    if (!GLEW_VERSION_3_3) {
        LOG_ERROR("winfont(GL): initialized GLEW and OpenGL 3.3 are required"); return false;
    }
    if (!CreatePipeline()) { Shutdown(); return false; }
    return true;
}
void GLFontBackend::Shutdown()
{
    EndPass();
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_prog) glDeleteProgram(m_prog);
    m_vbo = m_vao = m_prog = m_currentAtlas = 0;
    m_uMVP = m_uTex = -1;
    // Font owns its atlas handles and must destroy them before backend shutdown.
}

IFontBackend::AtlasId GLFontBackend::CreateAtlas(const uint8_t* pixels, int width, int height)
{
    if (!pixels || width <= 0 || height <= 0) return 0;
    GLint maximum; glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum);
    if (width > maximum || height > maximum) return 0;
    GLint active, texture, unpackBuffer;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
    const GLenum keys[] = { GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH,
        GL_UNPACK_SKIP_PIXELS, GL_UNPACK_SKIP_ROWS, GL_UNPACK_IMAGE_HEIGHT,
        GL_UNPACK_SKIP_IMAGES, GL_UNPACK_SWAP_BYTES, GL_UNPACK_LSB_FIRST };
    GLint unpack[8];
    for (size_t i=0; i<8; ++i) glGetIntegerv(keys[i], &unpack[i]);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    for (size_t i=0; i<8; ++i) glPixelStorei(keys[i], i == 0 ? 1 : 0);
    GLuint atlas; glGenTextures(1, &atlas);
    GLint uploadedWidth = 0, uploadedHeight = 0;
    if (atlas) {
        glBindTexture(GL_TEXTURE_2D, atlas);
        glTexImage2D(GL_TEXTURE_2D,0,GL_R8,width,height,0,GL_RED,GL_UNSIGNED_BYTE,pixels);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&uploadedWidth);
        glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_HEIGHT,&uploadedHeight);
    }
    for (size_t i=0; i<8; ++i) glPixelStorei(keys[i], unpack[i]);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(unpackBuffer));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture));
    glActiveTexture(static_cast<GLenum>(active));
    if (uploadedWidth != width || uploadedHeight != height) {
        if (atlas) glDeleteTextures(1,&atlas);
        LOG_ERROR("winfont(GL): atlas allocation failed (%dx%d)",width,height); return 0;
    }
    return atlas;
}
void GLFontBackend::DestroyAtlas(AtlasId atlas)
{
    GLuint texture = static_cast<GLuint>(atlas);
    if (texture) glDeleteTextures(1, &texture);
    if (texture == m_currentAtlas) m_currentAtlas = 0;
}
void GLFontBackend::BindAtlas(AtlasId atlas)
{
    m_currentAtlas = static_cast<GLuint>(atlas);
    if (m_passOpen) {
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_currentAtlas);
    }
}

void GLFontBackend::BeginPass(const Mat4& mvp, AtlasId atlas)
{
    if (!m_prog || !m_vao || !m_vbo) return;
    // Sequential passes are supported; nested passes are not part of the core API.
    if (m_passOpen) EndPass();
    auto& s = m_saved;
    glGetIntegerv(GL_CURRENT_PROGRAM,&s.program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&s.vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&s.arrayBuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE,&s.activeTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D,&s.texture0);
    glGetIntegerv(GL_SAMPLER_BINDING,&s.sampler0);
    s.depth=glIsEnabled(GL_DEPTH_TEST); s.cull=glIsEnabled(GL_CULL_FACE);
    s.blend=glIsEnabled(GL_BLEND); s.scissor=glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_BLEND_EQUATION_RGB,&s.equationRGB);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA,&s.equationAlpha);
    glGetIntegerv(GL_BLEND_SRC_RGB,&s.srcRGB); glGetIntegerv(GL_BLEND_DST_RGB,&s.dstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA,&s.srcAlpha); glGetIntegerv(GL_BLEND_DST_ALPHA,&s.dstAlpha);
    glGetIntegerv(GL_SCISSOR_BOX,s.scissorBox); glGetIntegerv(GL_VIEWPORT,s.viewport);
    glUseProgram(m_prog); glUniformMatrix4fv(m_uMVP,1,GL_FALSE,mvp.m);
    glUniform1i(m_uTex,0); glBindVertexArray(m_vao); glBindBuffer(GL_ARRAY_BUFFER,m_vbo);
    glBindSampler(0,0); glBindTexture(GL_TEXTURE_2D,static_cast<GLuint>(atlas));
    m_currentAtlas=static_cast<GLuint>(atlas);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD,GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_SCISSOR_TEST);
    m_passOpen=true;
}
void GLFontBackend::DrawGeometry(const FontVertex* vertices, size_t count)
{
    if (!m_passOpen || !vertices || !count) return;
    if (count > static_cast<size_t>(std::numeric_limits<GLsizei>::max()) ||
        count > static_cast<size_t>(std::numeric_limits<GLsizeiptr>::max())/sizeof(FontVertex)) {
        LOG_ERROR("winfont(GL): geometry batch too large"); return;
    }
    glBindBuffer(GL_ARRAY_BUFFER,m_vbo);
    // BufferData replaces the storage; previous queued draws retain their data.
    glBufferData(GL_ARRAY_BUFFER,static_cast<GLsizeiptr>(count*sizeof(FontVertex)),vertices,GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES,0,static_cast<GLsizei>(count));
}
void GLFontBackend::EndPass()
{
    if (!m_passOpen) return;
    const auto& s=m_saved;
    glUseProgram(static_cast<GLuint>(s.program)); glBindVertexArray(static_cast<GLuint>(s.vao));
    glBindBuffer(GL_ARRAY_BUFFER,static_cast<GLuint>(s.arrayBuffer));
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,static_cast<GLuint>(s.texture0));
    glBindSampler(0,static_cast<GLuint>(s.sampler0)); glActiveTexture(static_cast<GLenum>(s.activeTexture));
    glBlendEquationSeparate(s.equationRGB,s.equationAlpha);
    glBlendFuncSeparate(s.srcRGB,s.dstRGB,s.srcAlpha,s.dstAlpha);
    enable(GL_DEPTH_TEST,s.depth); enable(GL_CULL_FACE,s.cull); enable(GL_BLEND,s.blend);
    glScissor(s.scissorBox[0],s.scissorBox[1],s.scissorBox[2],s.scissorBox[3]);
    enable(GL_SCISSOR_TEST,s.scissor);
    m_passOpen=false;
}
void GLFontBackend::SetScissor(int x, int y, int w, int h)
{
    if (!m_passOpen) return;
    const double vx=m_saved.viewport[0], vy=m_saved.viewport[1];
    const double rightBound=vx+m_saved.viewport[2], topBound=vy+m_saved.viewport[3];
    auto clamp=[](double v,double lo,double hi){return std::max(lo,std::min(v,hi));};
    double left=clamp(m_viewportX+std::floor(double(x)*m_fbScaleX),vx,rightBound);
    double bottom=clamp(m_viewportY+std::floor(double(y)*m_fbScaleY),vy,topBound);
    double right=clamp(m_viewportX+std::ceil((double(x)+std::max(w,0))*m_fbScaleX),vx,rightBound);
    double top=clamp(m_viewportY+std::ceil((double(y)+std::max(h,0))*m_fbScaleY),vy,topBound);
    if (w<=0 || h<=0 || m_fbScaleX<=0 || m_fbScaleY<=0) right=left,top=bottom;
    glEnable(GL_SCISSOR_TEST);
    glScissor(static_cast<GLint>(left),static_cast<GLint>(bottom),
              static_cast<GLsizei>(std::max(0.0,right-left)),
              static_cast<GLsizei>(std::max(0.0,top-bottom)));
}
void GLFontBackend::DisableScissor() { if (m_passOpen) glDisable(GL_SCISSOR_TEST); }
void GLFontBackend::SetFramebufferScale(float sx,float sy)
{
    m_fbScaleX=std::isfinite(sx) && sx>0 ? sx : 0;
    m_fbScaleY=std::isfinite(sy) && sy>0 ? sy : 0;
}
void GLFontBackend::SetViewportOrigin(int x,int y) { m_viewportX=x; m_viewportY=y; }
} // namespace winfont
