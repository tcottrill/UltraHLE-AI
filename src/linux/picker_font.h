// Linux/SteamOS font adapter. All calls run on the main thread with the
// picker context current. Coordinates retain the picker's 1024x768 y-up canvas.
#pragma once

struct PickerFontColor { float r, g, b, a; };
// A null/empty path resolves fonts/ui.ttf beneath ULTRAHLE_ASSETS, or the
// actual executable's assets directory (independent of CWD/ULTRAHLE_HOME).
bool picker_font_init(const char* ttf_path = nullptr);
void picker_font_shutdown();
void picker_font_begin(int vx, int vy, int vw, int vh);
float picker_font_width(const char* text, float canvas_height);
void picker_font_draw(float x, float baseline_y, float canvas_height,
                      PickerFontColor color, const char* text,
                      float max_width = 0.0f);
void picker_font_end();
