// Adapted from SpriteTest sys_font/winfont_common_glsl.h; see README.md provenance.
#pragma once

namespace winfont
{
// Linear atlas coverage with straight-alpha output. The caller owns sRGB state.
inline constexpr const char* kShadeBodyGLSL = R"glsl(
vec4 winfont_shade(sampler2D tex, vec2 uv, vec4 color)
{
    float coverage = texture(tex, uv).r;
    return vec4(color.rgb, color.a * coverage);
}
)glsl";
}
