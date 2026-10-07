// Small, renderer-independent text policy shared by picker measurement/drawing.
#pragma once
#include <string>

namespace picker_text {

inline std::string normalize(const char* text)
{
    std::string out;
    if (!text) return out;
    const auto* p = reinterpret_cast<const unsigned char*>(text);
    while (*p) {
        const unsigned char c = *p++;
        if ((c >= 32 && c <= 126) || c == '\n') { out += char(c); continue; }
        if (c == '\t') { out += "    "; continue; }
        // Render one '?' per valid Unicode scalar. Invalid bytes each get a
        // replacement; never read beyond the first string terminator.
        int extra = c >= 0xc2 && c <= 0xdf ? 1 :
                    c >= 0xe0 && c <= 0xef ? 2 :
                    c >= 0xf0 && c <= 0xf4 ? 3 : 0;
        bool valid = extra != 0;
        for (int i = 0; valid && i < extra; ++i) {
            const unsigned char b = p[i];
            valid = b >= 0x80 && b <= 0xbf;
            if (i == 0) {
                if (c == 0xe0 && b < 0xa0) valid = false; // overlong
                if (c == 0xed && b >= 0xa0) valid = false; // surrogate
                if (c == 0xf0 && b < 0x90) valid = false; // overlong
                if (c == 0xf4 && b >= 0x90) valid = false; // > U+10ffff
            }
        }
        if (valid) p += extra;
        out += '?';
    }
    return out;
}

template<class Measure>
std::string ellipsize(std::string text, float max_width, Measure measure)
{
    if (max_width <= 0 || measure(text) <= max_width) return text;
    if (measure("...") > max_width) return {};
    while (!text.empty() && measure(text + "...") > max_width) text.pop_back();
    return text + "...";
}
} // namespace picker_text
