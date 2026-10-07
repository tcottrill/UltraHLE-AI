# Linux winfont port

This directory reuses SpriteTest's renderer-independent ASCII font core and
stb_truetype atlas rasterizer. UltraHLE supplies a separate OpenGL 3.3 backend
and explicitly owns each backend/font in the picker context. This port changes
font rendering only; picker redesign and Windows font changes are outside scope.

## Copied source provenance

Copied on 2026-10-05 from
`C:\SourceEngineAlpha\SpriteTestVulcan28_vulkan_working_copy` without editing the
donor. SHA-256 values below describe the original donor bytes, before adaptation.

| Destination | Donor path | Original SHA-256 |
| --- | --- | --- |
| `winfont.h` | `sys_font/winfont.h` | `23e0b233950d34d92febc1c96ef901cd306c8062388d616ce1c03fb5a8be405d` |
| `stb_truetype.h` | `sys_general/stb_truetype.h` | `ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab` |

The donor core's existing descriptive comments are retained; its file has no
embedded copyright/license declaration. `stb_truetype.h` is an unchanged v1.26
copy by Sean Barrett / RAD Game Tools and retains its complete MIT/public-domain
license alternatives and all notices. No donor engine math implementation was
copied: `winfont_types.h` defines only the small local RGBA aggregate.

## Core API and adaptations

`winfont::Font`, `IFontBackend`, `Mat4` (16 column-major floats), and `FontVertex`
(eight floats: x/y/u/v/r/g/b/a) retain the donor interfaces. Colors are explicitly
`winfont::Vec4 { float r, g, b, a; }`. `Destroy`, `DrawShadowScaled`, and
`DrawOutlineScaled` keep their donor names. Logging uses UltraHLE's
`../aae/sys_log.h` adapter.

The core keeps the donor's atlas packing/growth, supersampling, oversampling,
kerning, measurements, wrapping, alignment, shadows, outlines and geometry.
The unused engine input/rect/math headers, legacy engine-matrix overload,
global default backend/font and C-style wrappers are removed. Define
`WINFONT_IMPLEMENTATION` only in `winfont_impl.cpp` to compile stb exactly once.

Fonts require an injected caller-owned backend before initialization. Destroying
or reinitializing a font preserves that backend for retry; destroy the font
before its backend or owning GL context. Initialization rejects empty buffers,
nonpositive/nonfinite heights and atlas dimensions outside 1..4096. Atlas growth
is capped at 4096 per dimension and failed atlas creation clears partial CPU
state. `InitFromFile` requires an explicit path. Missing/empty files return false.
Font files must be trusted: stb_truetype explicitly does not validate arbitrary
malformed fonts, and this port uses the bundled font rather than ROM data.

Clip changes always submit pending geometry under the previous clip. Backend
scissors receive y-up canvas coordinates; the backend applies viewport scale
and origin. `Font::End` flushes and finishes with `EndPass`, so scissor cleanup
cannot overwrite restored caller state.

## Bundled asset

`assets/fonts/ui.ttf` is DejaVu Sans Book 2.37 from Ubuntu's
`fonts-dejavu-core` 2.37-8build1, upstream
<https://dejavu-fonts.github.io/>. SHA-256:
`b4c632e3cdf9acc7f28758fb5a323c8524d7fc6660d46904d9b6cbe2809c419c`.
Its redistribution notices are in `assets/fonts/LICENSE.txt`, copied exactly
from `/usr/share/doc/fonts-dejavu-core/copyright` (SHA-256
`63d3ba759d12804c5b31a9d5940d855c1820d1f5999e6b0872eb1c7ff045fbc9`).
Flatpak staging and installed-asset validation are required by the user and are
tracked by the build/assets owner and coordinating report. The user also
authorized the separate `op_memaddr` Linux build fix; the coordinator owns that
change, not this font core.

## Core verification

On 2026-10-05, GCC 15.2.0 under Ubuntu WSL compiled the core with C++17,
`-Wall -Wextra -pedantic` and passed `tests/linux/winfont_core_test.cpp` using the
bundled TTF. The test records actual nonzero atlas coverage, empty/null widths,
scaled advances, six finite glyph vertices, multiline positions, kerning,
wrapping, clip flushing/intersection, final restoration order, atlas destruction,
missing/empty files, invalid sizes, rejected-atlas cleanup and retry.

Two ordinary header users plus `winfont_impl.cpp` also link successfully with
that compiler. Clang is unavailable in the supplied WSL environment. CPU tests
verify layout and lifetime contracts; they do not establish GL rendering,
Steam Deck behavior or Flatpak installation. Those checks belong to the backend
and integration reports.
