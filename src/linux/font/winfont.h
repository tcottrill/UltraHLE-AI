#pragma once

// -----------------------------------------------------------------------------
// winfont.h
// Header-only, renderer-agnostic font drawing core.
//
// - Bakes ASCII glyphs (32..126) into an R8 atlas using stb_truetype's
//   pack API (stbtt_PackBegin / stbtt_PackSetOversampling / PackFontRange)
//   with prefiltered oversampling and optional supersampled bake size.
// - Builds CPU-side vertex geometry (FontVertex).
// - Delegates all GPU work to an IFontBackend that the caller injects.
// - Supports printf/vprintf formatting, scaling, kerning, measurement,
//   alignment, wrapping, shadow, outline, pixel snapping, and a scissor /
//   clip-rect stack (delegated to the backend).
//
// Backend contract:
//   See IFontBackend below. An OpenGL implementation lives in
//   winfont_backend_gl.h. A Vulkan implementation can provide the same
//   interface without touching this file.
//
// Usage:
//   winfont::GLFontBackend gl;                    // or VulkanFontBackend
//   winfont::Font font;
//   font.SetBackend(&gl);
//   font.InitFromFile(18.0f, "fonts/ui.ttf");
//   font.SetScreenSize(w, h);
//
//   font.Begin(mvp);
//     font.DrawTextScaled(10, 10, color, 1.0f, "Hello");
//   font.End();
//
// stb_truetype:
//   In EXACTLY one translation unit, define WINFONT_IMPLEMENTATION before
//   including this header (winfont_impl.cpp in UltraHLE).
// -----------------------------------------------------------------------------

#include <cstdint>
#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <cmath>
#include <algorithm>

#include "../aae/sys_log.h"

#ifndef WINFONT_ASSERT
#include <cassert>
#define WINFONT_ASSERT(x) assert(x)
#endif

#if defined(WINFONT_IMPLEMENTATION)
#ifndef STB_TRUETYPE_IMPLEMENTATION
#define STB_TRUETYPE_IMPLEMENTATION
#endif
#endif

#include "stb_truetype.h"
#include "winfont_types.h"

namespace winfont
{
	// -------------------------------------------------------------------------
	// Backend contract
	//
	// Mat4 is an opaque 4x4 column-major float matrix. We don't reuse
	// aae::math::mat4 here on purpose so the backend interface stays
	// decoupled from your math library.
	// -------------------------------------------------------------------------

	struct Mat4
	{
		float m[16];
	};

	struct FontVertex
	{
		float x, y;
		float u, v;
		float r, g, b, a;
	};

	class IFontBackend
	{
	public:
		virtual ~IFontBackend() = default;

		// atlas handle is opaque to the core; backend returns/destroys a handle.
		// 0 = invalid / no atlas.
		using AtlasId = uint64_t;
		virtual AtlasId CreateAtlas(const uint8_t* pixels, int width, int height) = 0;
		virtual void    DestroyAtlas(AtlasId atlas) = 0;
		virtual void    BindAtlas(AtlasId atlas) = 0;   // called from BeginPass context

		// Called once per Font::Begin. Binds pipeline, atlas, sets MVP uniform.
		virtual void BeginPass(const Mat4& mvp, AtlasId atlas) = 0;

		// Submit one batch of triangles. count = number of vertices (multiple of 3).
		virtual void DrawGeometry(const FontVertex* vertices, size_t count) = 0;

		// Called once per Font::End.
		virtual void EndPass() = 0;

		// Scissor rect in canvas pixels (y-up, GL-style: origin bottom-left).
		// Backend applies framebuffer scale and viewport origin.
		virtual void SetScissor(int x, int y, int w, int h) = 0;
		virtual void DisableScissor() = 0;

		// Framebuffer scale. The winfont core lays out and clips everything in
		// "canvas" pixels (whatever screen size the Font was told about). When
		// the canvas is drawn 1:1 into its own framebuffer this is (1,1) and a
		// no-op. When text is drawn straight to a larger framebuffer (the
		// native-resolution UI pass: a 1920x1080 canvas rendered into the real
		// window), the backend multiplies its viewport and scissor by this
		// factor so canvas coords map onto the actual pixels. Default no-op so
		// backends that don't need it (and the RT pass, scale 1) are unaffected.
		virtual void SetFramebufferScale(float /*sx*/, float /*sy*/) {}
	};

	// -------------------------------------------------------------------------
	// Helpers
	// -------------------------------------------------------------------------

	static inline bool ReadFileBytes(const char* path, std::vector<uint8_t>& out)
	{
		out.clear();

		if (!path || !path[0])
			return false;

		std::ifstream f(path, std::ios::binary);
		if (!f)
			return false;

		f.seekg(0, std::ios::end);
		std::streamoff len = f.tellg();
		if (len <= 0)
			return false;

		f.seekg(0, std::ios::beg);

		out.resize((size_t)len);
		f.read((char*)out.data(), len);
		return f.good();
	}

	struct Rect
	{
		float x = 0.0f;
		float y = 0.0f;
		float w = 0.0f;
		float h = 0.0f;

		Rect() = default;
		Rect(float inX, float inY, float inW, float inH)
			: x(inX), y(inY), w(inW), h(inH) {
		}
	};

	struct ClipRect
	{
		int x = 0;
		int y = 0;
		int w = 0;
		int h = 0;

		ClipRect() = default;
		ClipRect(int inX, int inY, int inW, int inH)
			: x(inX), y(inY), w(inW), h(inH) {
		}
	};

	// -------------------------------------------------------------------------
	// Font
	//
	// Owns the stb_truetype baking, CPU-side vertex buffer, and layout logic.
	// Rendering is delegated to an IFontBackend* the caller injects via
	// SetBackend(). Not owned; backend lifetime is the caller's responsibility.
	// -------------------------------------------------------------------------

	struct Font
	{
		static constexpr int kFirst = 32;
		static constexpr int kCount = 95; // 32..126

		enum Align
		{
			ALIGN_LEFT = 0,
			ALIGN_CENTER = 1,
			ALIGN_RIGHT = 2
		};

		enum VAlign
		{
			VALIGN_TOP = 0,
			VALIGN_MIDDLE = 1,
			VALIGN_BOTTOM = 2
		};

		// ---- Backend ---------------------------------------------------------

		IFontBackend* backend = nullptr;

		void SetBackend(IFontBackend* b)
		{
			backend = b;
		}

		// Explicit caller-owned backend; no engine-global renderer selection.
		IFontBackend* GetBackend() const
		{
			return backend;
		}

		// ---- Static utilities ------------------------------------------------

		static int ClampInt(int v, int lo, int hi)
		{
			return (v < lo) ? lo : ((v > hi) ? hi : v);
		}

		static ClipRect IntersectClipRects(const ClipRect& a, const ClipRect& b)
		{
			int x0 = (a.x > b.x) ? a.x : b.x;
			int y0 = (a.y > b.y) ? a.y : b.y;
			int x1 = ((a.x + a.w) < (b.x + b.w)) ? (a.x + a.w) : (b.x + b.w);
			int y1 = ((a.y + a.h) < (b.y + b.h)) ? (a.y + a.h) : (b.y + b.h);

			if (x1 < x0) x1 = x0;
			if (y1 < y0) y1 = y0;

			return ClipRect(x0, y0, x1 - x0, y1 - y0);
		}

		int CountLines(const char* s) const
		{
			if (!s || !s[0])
				return 1;

			int lines = 1;
			for (const char* p = s; *p; ++p)
			{
				if (*p == '\n')
					++lines;
			}
			return lines;
		}

		// ---- Drawing in rects (alignment / wrapping) -------------------------

		void DrawInRectScaled(
			const Rect& rect,
			float scale,
			Vec4 color,
			Align align,
			VAlign vAlign,
			const char* s)
		{
			if (!initialized || !s || scale <= 0.0f)
				return;

			float textW = MeasureTextWidthScaled(s, scale);
			float textH = MeasureTextHeightScaled(s, scale);

			float drawX = rect.x;
			float drawY = rect.y;

			if (align == ALIGN_CENTER)
				drawX = rect.x + (rect.w - textW) * 0.5f;
			else if (align == ALIGN_RIGHT)
				drawX = rect.x + rect.w - textW;

			if (vAlign == VALIGN_MIDDLE)
				drawY = rect.y + (rect.h - textH) * 0.5f;
			else if (vAlign == VALIGN_BOTTOM)
				drawY = rect.y + rect.h - textH;

			DrawTextScaled(drawX, drawY, color, scale, s);
		}

		void DrawWrappedInRectScaled(
			const Rect& rect,
			float scale,
			Vec4 color,
			Align align,
			VAlign vAlign,
			const char* s)
		{
			if (!initialized || !s || scale <= 0.0f)
				return;

			float textH = MeasureWrappedHeightScaled(s, rect.w, scale);
			float drawY = rect.y;

			if (vAlign == VALIGN_MIDDLE)
				drawY = rect.y + (rect.h - textH) * 0.5f;
			else if (vAlign == VALIGN_BOTTOM)
				drawY = rect.y + rect.h - textH;

			if (align == ALIGN_LEFT)
			{
				DrawWrappedScaled(rect.x, drawY, rect.w, scale, color, s);
				return;
			}

			float lineAdvance = GetLineAdvance(scale);
			std::vector<std::string> lines;
			const float spaceWidth = MeasureTextWidthScaled(" ", scale);

			std::string current;
			float currentWidth = 0.0f;
			const char* p = s;

			while (*p)
			{
				if (*p == '\n')
				{
					lines.push_back(current);
					current.clear();
					currentWidth = 0.0f;
					++p;
					continue;
				}

				if (*p == ' ')
				{
					if (!current.empty())
					{
						current += ' ';
						currentWidth += spaceWidth;
					}
					++p;
					continue;
				}

				const char* wordStart = p;
				while (*p && *p != ' ' && *p != '\n')
					++p;

				std::string word(wordStart, (size_t)(p - wordStart));
				float wordWidth = MeasureTextWidthScaled(word.c_str(), scale);

				if (!current.empty() && (currentWidth + wordWidth) > rect.w)
				{
					lines.push_back(current);
					current = word;
					currentWidth = wordWidth;
				}
				else
				{
					current += word;
					currentWidth += wordWidth;
				}
			}

			if (!current.empty())
				lines.push_back(current);

			float y = drawY;
			for (size_t i = 0; i < lines.size(); ++i)
			{
				float lineW = MeasureTextWidthScaled(lines[i].c_str(), scale);
				float x = rect.x;

				if (align == ALIGN_CENTER)
					x = rect.x + (rect.w - lineW) * 0.5f;
				else if (align == ALIGN_RIGHT)
					x = rect.x + rect.w - lineW;

				DrawTextScaled(x, y, color, scale, lines[i].c_str());
				y += (yDown ? lineAdvance : -lineAdvance);
			}
		}

		float MeasureTextHeightScaled(const char* s, float scale) const
		{
			if (!initialized || !s || scale <= 0.0f)
				return 0.0f;

			return (float)CountLines(s) * GetLineAdvance(scale);
		}

		float MeasureTextHeight(const char* s) const
		{
			return MeasureTextHeightScaled(s, drawScale);
		}

		void SetScreenSize(int w, int h)
		{
			screenW = w;
			screenH = h;
		}

		// ---- Clip-rect stack (scissor) ---------------------------------------

		void ApplyCurrentClip()
		{
			IFontBackend* activeBackend = GetBackend();
			if (!activeBackend)
				return;

			// Correct behavior: flush anything already queued under the PREVIOUS
			// scissor state before swapping to the new one. Guarantees that each
			// DrawText* call renders under the clip active at the moment it was
			// issued.
			if (!verts.empty())
			{
				activeBackend->DrawGeometry(verts.data(), verts.size());
				verts.clear();
			}

			if (clipStack.empty())
			{
				activeBackend->DisableScissor();
				return;
			}

			if (screenW <= 0 || screenH <= 0)
			{
				activeBackend->DisableScissor();
				return;
			}

			const ClipRect& r = clipStack.back();

			// Convert top-left UI space to y-up framebuffer space for GL-style
			// scissor. Vulkan backends typically want the same convention or
			// invert internally; the backend is responsible for any further
			// adjustment.
			int sx = r.x;
			int sy = screenH - (r.y + r.h);
			int sw = r.w;
			int sh = r.h;

			if (sw < 0) sw = 0;
			if (sh < 0) sh = 0;

			activeBackend->SetScissor(sx, sy, sw, sh);
		}

		void PushClipRectPixels(int x, int y, int w, int h)
		{
			ClipRect next(x, y, w, h);

			if (!clipStack.empty())
				next = IntersectClipRects(clipStack.back(), next);

			clipStack.push_back(next);
			ApplyCurrentClip();
		}

		void PushClipRect(const Rect& r)
		{
			int x = (int)std::floor(r.x + 0.5f);
			int y = (int)std::floor(r.y + 0.5f);
			int w = (int)std::floor(r.w + 0.5f);
			int h = (int)std::floor(r.h + 0.5f);

			PushClipRectPixels(x, y, w, h);
		}

		void PopClipRect()
		{
			if (!clipStack.empty())
				clipStack.pop_back();

			ApplyCurrentClip();
		}

		void ClearClipRects()
		{
			clipStack.clear();
			ApplyCurrentClip();
		}

		// ---- State -----------------------------------------------------------

		int atlasW = 512;
		int atlasH = 512;

		float pixelHeight = 18.0f;
		float drawScale = 1.0f;

		bool yDown = true;
		bool enableKerning = false;
		bool pixelSnap = true;

		int screenW = 0;
		int screenH = 0;
		std::vector<ClipRect> clipStack;

		// ---- Bake quality knobs (set BEFORE InitFromTTF / InitFromFile) ------
		//
		// supersample: bake the atlas at N x the requested pixel height while
		// keeping every layout metric in the requested (logical) size. The
		// glyph quads then sample a larger raster, so text survives being
		// drawn onto a canvas that is later upscaled (1080p scene RT -> 4K
		// window). 1 = bake exactly at the requested size (legacy behavior).
		//
		// oversampleH / oversampleV: stb_truetype pack-API prefiltered
		// oversampling. Greatly improves bilinear-sampled text at subpixel
		// positions and non-integer scales. 2x2 is the stb-recommended sweet
		// spot; values above 3 burn atlas space for little visible gain.
		int supersample = 2;
		int oversampleH = 2;
		int oversampleV = 2;

		stbtt_packedchar packed[kCount]{};

		stbtt_fontinfo fontInfo{};
		bool fontInfoValid = false;

		int ascent = 0;
		int descent = 0;
		int lineGap = 0;
		float bakedFontScale = 1.0f;
		float bakedLineAdvance = 18.0f;

		std::vector<uint8_t> ttfBytes;
		std::vector<FontVertex> verts;

		// MVP is stored as the backend-facing Mat4, not aae::math::mat4, so the
		// core never has to include the math header in its interface.
		Mat4 currentMVP{};
		IFontBackend::AtlasId atlas = 0;
		bool initialized = false;

		// ---- Destroy ---------------------------------------------------------

		void Destroy()
		{
			IFontBackend* activeBackend = GetBackend();
			if (activeBackend && atlas)
				activeBackend->DestroyAtlas(atlas);
			atlas = 0;

			verts.clear();
			ttfBytes.clear();

			fontInfo = stbtt_fontinfo{};
			fontInfoValid = false;

			ascent = 0;
			descent = 0;
			lineGap = 0;
			bakedFontScale = 1.0f;
			bakedLineAdvance = pixelHeight;

			// The backend is caller-owned and stays attached for reinitialization.
			// Callers must destroy this font before destroying its backend.

			initialized = false;
		}

		// ---- Metrics ---------------------------------------------------------

		static float SnapCoord(float v, bool enable)
		{
			return enable ? floorf(v + 0.5f) : v;
		}

		float GetLineAdvance(float scale) const
		{
			return bakedLineAdvance * scale;
		}

		float GetBaselineOffset(float scale) const
		{
			return (float)ascent * bakedFontScale * scale;
		}

		float GetKerningAdvance(int prevCodepoint, int codepoint, float scale) const
		{
			if (!enableKerning || !fontInfoValid)
				return 0.0f;

			if (prevCodepoint <= 0 || codepoint <= 0)
				return 0.0f;

			int kern = stbtt_GetCodepointKernAdvance(
				const_cast<stbtt_fontinfo*>(&fontInfo),
				prevCodepoint,
				codepoint
			);

			return (float)kern * bakedFontScale * scale;
		}

		// ---- Init ------------------------------------------------------------

		bool InitFromTTF(const uint8_t* ttfData, size_t ttfSize, float sizePixels,
			int inAtlasW = 512, int inAtlasH = 512)
		{
			Destroy();

			IFontBackend* activeBackend = GetBackend();
			if (!activeBackend)
				return false;

			if (!ttfData || ttfSize == 0 || !std::isfinite(sizePixels) || sizePixels <= 0.0f
				|| inAtlasW <= 0 || inAtlasH <= 0 || inAtlasW > 4096 || inAtlasH > 4096)
				return false;

			atlasW = inAtlasW;
			atlasH = inAtlasH;
			pixelHeight = sizePixels;
			drawScale = 1.0f;

			ttfBytes.assign(ttfData, ttfData + ttfSize);

			if (!stbtt_InitFont(&fontInfo, ttfBytes.data(), stbtt_GetFontOffsetForIndex(ttfBytes.data(), 0)))
			{
				Destroy();
				return false;
			}
			fontInfoValid = true;

			// Layout metrics stay in LOGICAL pixels (the size the caller asked
			// for). Only the atlas raster is scaled up by supersample below.
			stbtt_GetFontVMetrics(&fontInfo, &ascent, &descent, &lineGap);
			bakedFontScale = stbtt_ScaleForPixelHeight(&fontInfo, pixelHeight);
			bakedLineAdvance = (float)(ascent - descent + lineGap) * bakedFontScale;

			const int ss = (supersample < 1) ? 1 : supersample;
			const int osH = (oversampleH < 1) ? 1 : oversampleH;
			const int osV = (oversampleV < 1) ? 1 : oversampleV;
			const float bakePixelHeight = sizePixels * (float)ss;

			// Pack-API bake with prefiltered oversampling. Replaces the legacy
			// stbtt_BakeFontBitmap path, which rasterized each glyph exactly
			// once at integer positions with no oversampling -- the main
			// source of jagged text once the canvas gets upscaled.
			//
			// Supersampled + oversampled glyphs may not fit the requested
			// atlas, so grow (double the smaller dimension) and retry, capped
			// at 4096 x 4096, before giving up.
			std::vector<uint8_t> bitmap;
			bool packOk = false;
			int tryW = atlasW;
			int tryH = atlasH;

			for (;;)
			{
				bitmap.assign((size_t)tryW * (size_t)tryH, 0);

				stbtt_pack_context packCtx;
				if (!stbtt_PackBegin(&packCtx, bitmap.data(), tryW, tryH, 0, 1, nullptr))
					break;

				stbtt_PackSetOversampling(&packCtx, (unsigned)osH, (unsigned)osV);

				const int fit = stbtt_PackFontRange(
					&packCtx,
					ttfBytes.data(),
					0,
					bakePixelHeight,
					kFirst,
					kCount,
					packed
				);

				stbtt_PackEnd(&packCtx);

				if (fit)
				{
					packOk = true;
					atlasW = tryW;
					atlasH = tryH;
					break;
				}

				if (tryW >= 4096 && tryH >= 4096)
					break;

				if (tryW <= tryH)
					tryW = std::min(4096, tryW * 2);
				else
					tryH = std::min(4096, tryH * 2);
			}

			if (!packOk)
			{
				LOG_ERROR("winfont: atlas pack failed at %.1fpx "
					"(supersample %d, oversample %dx%d) even at 4096x4096",
					bakePixelHeight, ss, osH, osV);
				Destroy();
				return false;
			}

			// Normalize packed metrics from bake-pixel space back to
			// logical-pixel space so the draw / measure code consumes them
			// unchanged. UV fields (x0..y1, atlas texels) stay as packed.
			if (ss > 1)
			{
				const float inv = 1.0f / (float)ss;
				for (int i = 0; i < kCount; ++i)
				{
					packed[i].xoff     *= inv;
					packed[i].yoff     *= inv;
					packed[i].xoff2    *= inv;
					packed[i].yoff2    *= inv;
					packed[i].xadvance *= inv;
				}
			}

			atlas = activeBackend->CreateAtlas(bitmap.data(), atlasW, atlasH);
			if (!atlas)
			{
				Destroy();
				return false;
			}

			initialized = true;
			return true;
		}

		bool InitFromFile(float sizePixels, const char* ttfPath,
			int inAtlasW = 512, int inAtlasH = 512)
		{
			if (!ttfPath || !ttfPath[0])
				return false;

			std::vector<uint8_t> bytes;
			if (!ReadFileBytes(ttfPath, bytes))
				return false;

			return InitFromTTF(bytes.data(), bytes.size(), sizePixels, inAtlasW, inAtlasH);
		}

		// ---- Frame lifecycle -------------------------------------------------

		// Overload that takes the backend-facing Mat4 directly. Prefer this in
		// new code.
		void Begin(const Mat4& mvp)
		{
			IFontBackend* activeBackend = GetBackend();
			if (!initialized || !activeBackend)
				return;

			currentMVP = mvp;
			verts.clear();

			activeBackend->BeginPass(currentMVP, atlas);
			ApplyCurrentClip();
		}

		void End()
		{
			IFontBackend* activeBackend = GetBackend();
			if (!initialized || !activeBackend)
				return;

			Flush();
			// EndPass restores caller state, including scissor. Nothing may
			// change backend state after that restoration.
			activeBackend->EndPass();
		}

		void Flush()
		{
			IFontBackend* activeBackend = GetBackend();
			if (!initialized || !activeBackend || verts.empty())
				return;

			activeBackend->DrawGeometry(verts.data(), verts.size());
			verts.clear();
		}

		// ---- Geometry building ----------------------------------------------

		void DrawGlyphQuad(
			float x0, float y0,
			float x1, float y1,
			float u0, float v0,
			float u1, float v1,
			Vec4 color)
		{
			if (pixelSnap)
			{
				// Snap the HORIZONTAL origin only; preserve the extent.
				// Independently snapping both corners can stretch or compress
				// each glyph by up to a pixel, which scales the UV region onto
				// a slightly wrong destination rectangle. Bilinear filtering
				// then pulls the atlas's fringe into adjacent pixels -- you
				// see flat glyph tops and the occasional missing bottom row.
				//
				// The VERTICAL position is deliberately NOT snapped here.
				// Each glyph's y0 = baseline + its own top bearing, and
				// bearings differ by a fraction of a pixel between letters.
				// Snapping y0 per glyph rounds neighbours to different rows
				// whenever their bearings straddle a .5 boundary, so a word's
				// x-height tops jitter up and down by a pixel. Instead the
				// caller snaps the shared BASELINE once (see DrawTextScaled),
				// which keeps every glyph on one crisp line while leaving the
				// small per-glyph top offsets uniform. At scale 1.0 the baked
				// bearings are already integral, so text stays pixel-crisp.
				const float dx = x1 - x0;
				x0 = SnapCoord(x0, true);
				x1 = x0 + dx;
			}

			FontVertex a{ x0, y0, u0, v0, color.r, color.g, color.b, color.a };
			FontVertex b{ x1, y0, u1, v0, color.r, color.g, color.b, color.a };
			FontVertex c{ x1, y1, u1, v1, color.r, color.g, color.b, color.a };
			FontVertex d{ x0, y1, u0, v1, color.r, color.g, color.b, color.a };

			verts.push_back(a);
			verts.push_back(b);
			verts.push_back(c);

			verts.push_back(c);
			verts.push_back(d);
			verts.push_back(a);
		}

		void DrawTextScaled(float x, float y, Vec4 color, float scale, const char* s)
		{
			if (!initialized || !s || scale <= 0.0f)
				return;

			float cx = x;
			float cy = y + GetBaselineOffset(scale);
			// Snap the BASELINE to a pixel once, so every glyph on this line
			// sits on the same integer row (see DrawGlyphQuad for why the
			// per-glyph vertical snap was removed). Re-snapped after each
			// newline so multi-line runs don't accumulate sub-pixel drift.
			if (pixelSnap)
				cy = SnapCoord(cy, true);
			int prevCodepoint = 0;

			for (const unsigned char* p = (const unsigned char*)s; *p; ++p)
			{
				int codepoint = (int)(*p);

				if (codepoint == '\n')
				{
					cx = x;
					cy += (yDown ? GetLineAdvance(scale) : -GetLineAdvance(scale));
					if (pixelSnap)
						cy = SnapCoord(cy, true);
					prevCodepoint = 0;
					continue;
				}

				if (codepoint < kFirst || codepoint >= (kFirst + kCount))
				{
					prevCodepoint = 0;
					continue;
				}

				cx += GetKerningAdvance(prevCodepoint, codepoint, scale);

				const stbtt_packedchar& bc = packed[codepoint - kFirst];

				// With the pack API the quad extent comes from xoff2/yoff2
				// (logical pixels), NOT from the atlas rect size: oversampled
				// and supersampled glyphs occupy a LARGER atlas region than
				// the on-screen quad. Deriving width from (x1 - x0) here
				// would draw giant glyphs -- the shrink is the entire point.
				float x0 = cx + bc.xoff  * scale;
				float y0 = cy + bc.yoff  * scale;
				float x1 = cx + bc.xoff2 * scale;
				float y1 = cy + bc.yoff2 * scale;

				float u0 = (float)bc.x0 / (float)atlasW;
				float v0 = (float)bc.y0 / (float)atlasH;
				float u1 = (float)bc.x1 / (float)atlasW;
				float v1 = (float)bc.y1 / (float)atlasH;

				DrawGlyphQuad(x0, y0, x1, y1, u0, v0, u1, v1, color);

				cx += bc.xadvance * scale;
				prevCodepoint = codepoint;
			}
		}

		void DrawText(float x, float y, Vec4 color, const char* s)
		{
			DrawTextScaled(x, y, color, drawScale, s);
		}

		void DrawTextVPrintfScaled(float x, float y, Vec4 color, float scale, const char* fmt, va_list va)
		{
			if (!fmt)
				return;

			char buf[4096];
			buf[0] = 0;

#if defined(_MSC_VER)
			_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, va);
#else
			vsnprintf(buf, sizeof(buf), fmt, va);
#endif

			DrawTextScaled(x, y, color, scale, buf);
		}

		void DrawTextVPrintf(float x, float y, Vec4 color, const char* fmt, va_list va)
		{
			DrawTextVPrintfScaled(x, y, color, drawScale, fmt, va);
		}

		// ---- Measurement -----------------------------------------------------

		float MeasureTextWidthScaled(const char* s, float scale) const
		{
			if (!initialized || !s || scale <= 0.0f)
				return 0.0f;

			float cx = 0.0f;
			float maxWidth = 0.0f;
			int prevCodepoint = 0;

			for (const unsigned char* p = (const unsigned char*)s; *p; ++p)
			{
				int codepoint = (int)(*p);

				if (codepoint == '\n')
				{
					if (cx > maxWidth)
						maxWidth = cx;
					cx = 0.0f;
					prevCodepoint = 0;
					continue;
				}

				if (codepoint < kFirst || codepoint >= (kFirst + kCount))
				{
					prevCodepoint = 0;
					continue;
				}

				cx += GetKerningAdvance(prevCodepoint, codepoint, scale);
				cx += packed[codepoint - kFirst].xadvance * scale;
				prevCodepoint = codepoint;
			}

			if (cx > maxWidth)
				maxWidth = cx;

			return maxWidth;
		}

		float MeasureTextWidth(const char* s) const
		{
			return MeasureTextWidthScaled(s, drawScale);
		}

		float MeasureWrappedHeightScaled(const char* s, float wrapWidth, float scale) const
		{
			if (!initialized || !s || scale <= 0.0f)
				return 0.0f;

			float lineAdvance = GetLineAdvance(scale);

			if (wrapWidth <= 0.0f)
				return MeasureTextHeightScaled(s, scale);

			float currentLineWidth = 0.0f;
			float totalHeight = lineAdvance;
			const float spaceWidth = MeasureTextWidthScaled(" ", scale);

			const char* p = s;

			while (*p)
			{
				if (*p == '\n')
				{
					currentLineWidth = 0.0f;
					totalHeight += lineAdvance;
					++p;
					continue;
				}

				if (*p == ' ')
				{
					if (currentLineWidth > 0.0f)
						currentLineWidth += spaceWidth;
					++p;
					continue;
				}

				const char* wordStart = p;
				while (*p && *p != ' ' && *p != '\n')
					++p;

				std::string word(wordStart, (size_t)(p - wordStart));
				float wordWidth = MeasureTextWidthScaled(word.c_str(), scale);

				if (currentLineWidth > 0.0f && (currentLineWidth + wordWidth) > wrapWidth)
				{
					totalHeight += lineAdvance;
					currentLineWidth = wordWidth;
				}
				else
				{
					currentLineWidth += wordWidth;
				}
			}

			return totalHeight;
		}

		// ---- Aligned / wrapped / shadow / outline ---------------------------

		void DrawAlignedScaled(float x, float y, float scale, Vec4 color, Align align, const char* s)
		{
			if (!s)
				return;

			float w = MeasureTextWidthScaled(s, scale);
			float drawX = x;

			if (align == ALIGN_CENTER)
				drawX = x - (w * 0.5f);
			else if (align == ALIGN_RIGHT)
				drawX = x - w;

			DrawTextScaled(drawX, y, color, scale, s);
		}

		void DrawWrappedScaled(float x, float y, float wrapWidth, float scale, Vec4 color, const char* s)
		{
			if (!initialized || !s || scale <= 0.0f)
				return;

			if (wrapWidth <= 0.0f)
			{
				DrawTextScaled(x, y, color, scale, s);
				return;
			}

			float lineAdvance = GetLineAdvance(scale);
			float cx = x;
			float cy = y;
			const float spaceWidth = MeasureTextWidthScaled(" ", scale);

			const char* p = s;
			bool lineHasText = false;

			while (*p)
			{
				if (*p == '\n')
				{
					cx = x;
					cy += (yDown ? lineAdvance : -lineAdvance);
					lineHasText = false;
					++p;
					continue;
				}

				if (*p == ' ')
				{
					if (lineHasText)
						cx += spaceWidth;
					++p;
					continue;
				}

				const char* wordStart = p;
				while (*p && *p != ' ' && *p != '\n')
					++p;

				std::string word(wordStart, (size_t)(p - wordStart));
				float wordWidth = MeasureTextWidthScaled(word.c_str(), scale);

				if (lineHasText && ((cx - x) + wordWidth) > wrapWidth)
				{
					cx = x;
					cy += (yDown ? lineAdvance : -lineAdvance);
					lineHasText = false;
				}

				DrawTextScaled(cx, cy, color, scale, word.c_str());
				cx += wordWidth;
				lineHasText = true;

				if (*p == ' ')
				{
					cx += spaceWidth;
					++p;
				}
			}
		}

		void DrawShadowScaled(float x, float y, float scale, Vec4 color, Vec4 shadowColor, float shadowDx, float shadowDy, const char* s)
		{
			DrawTextScaled(x + shadowDx, y + shadowDy, shadowColor, scale, s);
			DrawTextScaled(x, y, color, scale, s);
		}

		void DrawOutlineScaled(float x, float y, float scale, Vec4 color, Vec4 outlineColor, float thickness, const char* s)
		{
			DrawTextScaled(x - thickness, y, outlineColor, scale, s);
			DrawTextScaled(x + thickness, y, outlineColor, scale, s);
			DrawTextScaled(x, y - thickness, outlineColor, scale, s);
			DrawTextScaled(x, y + thickness, outlineColor, scale, s);

			DrawTextScaled(x - thickness, y - thickness, outlineColor, scale, s);
			DrawTextScaled(x + thickness, y - thickness, outlineColor, scale, s);
			DrawTextScaled(x - thickness, y + thickness, outlineColor, scale, s);
			DrawTextScaled(x + thickness, y + thickness, outlineColor, scale, s);

			DrawTextScaled(x, y, color, scale, s);
		}
	};

} // namespace winfont
