#include "picker_font.h"
#include "picker_text.h"
#include "font/winfont_backend_gl.h"
#include "aae/sys_log.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>
#include <limits.h>

// Each logical text height is baked at its size in real screen pixels and drawn
// in screen pixels at scale 1. A single atlas baked once and shrunk by the GPU
// (bilinear, no mipmaps) was fine on a 2160-line 4K screen but broke up at
// 1080p and on the Deck's 800 lines. Atlases are rebuilt when the picker's
// viewport height changes (resize, fullscreen, gamescope), never per frame.

namespace {
struct PickerFonts {
    winfont::GLFontBackend backend;
    std::vector<uint8_t> ttf;
    std::map<int, std::unique_ptr<winfont::Font>> sizes; // key: canvas height*100
    winfont::Font* active = nullptr;
    bool drawing = false;
    bool bakefailed = false;
    int vw = 0, vh = 0;      // viewport the atlases were baked for
    float sx = 0, sy = 0;    // screen pixels per canvas pixel
    winfont::Mat4 projection{};
};
// Allocation/init is explicit; no GL work occurs during static destruction.
std::unique_ptr<PickerFonts> fonts;

std::string font_path()
{
    const char* assets = std::getenv("ULTRAHLE_ASSETS");
    if (assets && *assets) return std::string(assets) + "/fonts/ui.ttf";
    char exe[PATH_MAX + 1];
    const ssize_t n = readlink("/proc/self/exe", exe, PATH_MAX);
    if (n <= 0 || n >= PATH_MAX) return {};
    exe[n] = 0;
    std::string path(exe);
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos) return {};
    return path.substr(0, slash) + "/assets/fonts/ui.ttf";
}

bool read_file(const std::string& path, std::vector<uint8_t>& out)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t buf[65536];
    size_t n;
    out.clear();
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return !out.empty();
}

bool bake(winfont::Font& font, float pixels)
{
    font.SetBackend(&fonts->backend);
    font.yDown = true;
    font.enableKerning = true;
    // Baked at the drawn size: no supersampling to shrink away. Horizontal
    // oversampling keeps subpixel glyph placement smooth; vertical stays 1 so
    // stems sit on whole rows under the snapped baseline.
    font.supersample = 1;
    font.oversampleH = 2;
    font.oversampleV = 1;
    return font.InitFromTTF(fonts->ttf.data(), fonts->ttf.size(), pixels);
}

void drop_sizes()
{
    if (fonts->active) fonts->active->End();
    fonts->active = nullptr;
    for (auto& f : fonts->sizes) f.second->Destroy();
    fonts->sizes.clear();
}

// The font for a canvas height at the current screen scale, baked on first use.
winfont::Font* sized_font(float canvas_height)
{
    if (!fonts || fonts->sy <= 0 || !std::isfinite(canvas_height) || canvas_height <= 0)
        return nullptr;
    const int key = int(std::lround(canvas_height * 100.0f));
    auto it = fonts->sizes.find(key);
    if (it != fonts->sizes.end()) return it->second.get();
    auto font = std::make_unique<winfont::Font>();
    const float pixels = std::max(1.0f, canvas_height * fonts->sy);
    if (!bake(*font, pixels)) {
        if (!fonts->bakefailed)
            LOG_ERROR("picker font: cannot bake %.1f px atlas", pixels);
        fonts->bakefailed = true;
        return nullptr;
    }
    font->SetScreenSize(fonts->vw, fonts->vh);
    winfont::Font* f = font.get();
    fonts->sizes.emplace(key, std::move(font));
    return f;
}
}

bool picker_font_init(const char* ttf_path)
{
    picker_font_shutdown();
    const std::string path = ttf_path && *ttf_path ? ttf_path : font_path();
    if (path.empty()) {
        LOG_ERROR("picker font: cannot resolve bundled font path; using vector fallback");
        return false;
    }
    fonts = std::make_unique<PickerFonts>();
    if (!read_file(path, fonts->ttf)) {
        LOG_ERROR("picker font: cannot load %s; using vector fallback", path.c_str());
        picker_font_shutdown();
        return false;
    }
    if (!fonts->backend.Init()) {
        picker_font_shutdown();
        LOG_ERROR("picker font: GL initialization failed; using vector fallback");
        return false;
    }
    // Prove the file bakes before committing to it over the vector font.
    winfont::Font probe;
    if (!bake(probe, 26.0f)) {
        LOG_ERROR("picker font: cannot load %s; using vector fallback", path.c_str());
        probe.Destroy();
        picker_font_shutdown();
        return false;
    }
    probe.Destroy();
    LOG_INFO("picker font: loaded %s (OpenGL 3.3 winfont)", path.c_str());
    return true;
}

void picker_font_shutdown()
{
    if (!fonts) return;
    picker_font_end();
    drop_sizes();
    fonts->backend.Shutdown();
    fonts.reset();
}

void picker_font_begin(int vx, int vy, int vw, int vh)
{
    if (!fonts) return;
    picker_font_end();
    if (vw <= 0 || vh <= 0) { fonts->sx = fonts->sy = 0; return; }
    if (vw != fonts->vw || vh != fonts->vh) {
        drop_sizes();
        fonts->vw = vw;
        fonts->vh = vh;
        fonts->bakefailed = false;
        LOG_INFO("picker font: baking for a %dx%d viewport", vw, vh);
    }
    fonts->sx = float(vw) / 1024.0f;
    fonts->sy = float(vh) / 768.0f;
    // Column-major projection from top-left viewport pixels into NDC.
    fonts->projection = {{
        2.0f/float(vw), 0, 0, 0,
        0, -2.0f/float(vh), 0, 0,
        0, 0, -1, 0,
        -1, 1, 0, 1
    }};
    fonts->backend.OnFrameBegin();
    fonts->backend.SetViewportOrigin(vx, vy);
    fonts->backend.SetFramebufferScale(1.0f, 1.0f);
    fonts->drawing = true;
}

float picker_font_width(const char* text, float canvas_height)
{
    winfont::Font* font = sized_font(canvas_height);
    if (!font) return 0;
    const std::string normalized = picker_text::normalize(text);
    return font->MeasureTextWidthScaled(normalized.c_str(), 1.0f) / fonts->sx;
}

void picker_font_draw(float x, float baseline_y, float canvas_height,
                      PickerFontColor color, const char* text, float max_width)
{
    if (!fonts || !fonts->drawing || !std::isfinite(x) || !std::isfinite(baseline_y) ||
        !std::isfinite(max_width)) return;
    winfont::Font* font = sized_font(canvas_height);
    if (!font) return;
    const float sx = fonts->sx, sy = fonts->sy;
    const auto measure = [&](const std::string& s) {
        return font->MeasureTextWidthScaled(s.c_str(), 1.0f);
    };
    const std::string normalized =
        picker_text::ellipsize(picker_text::normalize(text), max_width * sx, measure);
    if (normalized.empty()) return;
    if (fonts->active != font) {
        if (fonts->active) fonts->active->End();
        font->Begin(fonts->projection);
        fonts->active = font;
    }
    const float px = x * sx;
    const float py = (768.0f - baseline_y) * sy;   // baseline, top-down screen pixels
    // The core expects top-left clip coordinates and flips them for GL.
    // Keep glyph overhangs (and multiline strings) within the title column.
    if (max_width > 0)
        font->PushClipRectPixels(int(std::floor(px)), 0, int(std::ceil(max_width * sx)), fonts->vh);
    font->DrawTextScaled(px, py - font->GetBaselineOffset(1.0f),
                         {color.r, color.g, color.b, color.a}, 1.0f, normalized.c_str());
    if (max_width > 0) font->PopClipRect();
}

void picker_font_end()
{
    if (!fonts) return;
    if (fonts->active) fonts->active->End();
    fonts->active = nullptr;
    fonts->drawing = false;
}
