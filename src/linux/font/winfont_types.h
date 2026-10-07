#pragma once

namespace winfont {
// The font API only needs an RGBA color. No donor math/input dependencies.
struct Vec4 {
    float r, g, b, a;
};
} // namespace winfont
