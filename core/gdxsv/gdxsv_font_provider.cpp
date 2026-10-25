// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_font_provider.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_BBOX_H

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "log/Log.h"
#include "oslib/resources.h"

namespace {

struct GdxsvFontGlyph {
	u32 hash;
	u32 code_point;
};

const GdxsvFontGlyph kGlyphs[] = {
#include "gdxsv_font_glyphs.inc"
};

// Embedded resource (core/gdxsv/Fonts).
const char* const kFontFile = "gdxsv/GDXGothic-Medium.otf";
// Output px per game px; a glyph texture is 32x32 game px.
constexpr int kScale = 4;
constexpr int kSize = 32 * kScale;
// GDXGothic's em is the 24 px cell, its baseline 21.12 game px below the cell top (the em box is [-120, 880]).
constexpr float kEm = 24.f;
constexpr float kBaseline = 21.12f;
// A smaller or shorter glyph is scaled about the middle of its cell (x 12, or 6 if half-width), 12 game px below the top.
constexpr float kMiddleY = 12.f;

// DC2's glyph edge as the GPU draws it, measured from the game font: straight luminance and alpha by signed distance
// (game px, + outside) from the 0.5 coverage outline. Before the first row: 1, 1; after the last: 0, 0.
struct GdxsvEdgeRow {
	float distance;
	float luminance;
	float alpha;
};
constexpr GdxsvEdgeRow kGameEdge[] = {
	{-1.0625f, 1.0000f, 1.0000f}, {-0.9375f, 0.9841f, 1.0000f}, {-0.8125f, 0.9695f, 0.9997f}, {-0.6875f, 0.9248f, 1.0000f},
	{-0.5625f, 0.9189f, 0.9974f}, {-0.4375f, 0.8366f, 0.9735f}, {-0.3125f, 0.8027f, 0.9406f}, {-0.1875f, 0.7180f, 0.9136f},
	{-0.0625f, 0.6054f, 0.7995f}, {0.1875f, 0.4156f, 0.5920f},	{0.4375f, 0.2561f, 0.3316f},  {0.5625f, 0.1610f, 0.2314f},
	{0.6875f, 0.1537f, 0.1328f},  {0.8125f, 0.0970f, 0.1151f},	{0.9375f, 0.0637f, 0.0358f},  {1.1875f, 0.0295f, 0.0053f},
	{1.3125f, 0.0123f, 0.0021f},  {1.4375f, 0.0000f, 0.0000f},
};

// Text: the game's edge with its distances scaled by kEdgeScale (< 1 is sharper). The measured alpha is a mean over
// edges whose texels are each opaque grey or clear, so drawn as is the background would show through the rim: instead
// the glyph is opaque out to where that alpha falls to kRimCut and then fades over kRimFade game px, darkening as it
// fades like the game's (its clear texels are black), and no darker than the game's darkest grey texel before that.
// The rim then shows as a darker shade of the text colour, and the glyph is as bold as the game's on a dark background.
constexpr float kEdgeScale = 1.0f;
constexpr float kRimCut = 0.6f;
constexpr float kRimFade = 0.6f * kEdgeScale;
constexpr float kRimFloor = 115 / 255.f;
// Text: 96% of the font's height (the game's on-screen height), the horizontal strokes keeping their weight
// (GDXGothic Medium's stem: 27/256 em).
constexpr float kTextHeight = 0.96f;
constexpr float kMediumStem = 27.f / 256 * kEm;
// BattleName: 90% of the font's size and a dark outline under the white glyph: kNameOutlineWidth game px outside the
// 0.5 outline, its opacity falling to 0 over the outer kNameOutlineFade px, soft like the HUD's own digits. The
// background shows through it, as through the arcade's pilot names; the game tints texels with the text colour.
constexpr float kNameScale = 0.9f;
constexpr float kNameOutlineWidth = 2.0f;
constexpr float kNameOutlineFade = 1.5f;
constexpr float kNameOutlineGrey = 60 / 255.f;
constexpr float kNameOutlineOpacity = 0.6f;
// The game draws each name glyph from its own cell and shows only the cell's top-left 24x24 game px.
constexpr float kNameBox = 24.f;
constexpr size_t kCacheEntries = 256;

const GdxsvFontGlyph* find_glyph(u32 hash) {
	const GdxsvFontGlyph* end = kGlyphs + sizeof(kGlyphs) / sizeof(kGlyphs[0]);
	const GdxsvFontGlyph* it = std::lower_bound(kGlyphs, end, hash, [](const GdxsvFontGlyph& g, u32 h) { return g.hash < h; });
	return it != end && it->hash == hash ? it : nullptr;
}

float edge_at(float d, float GdxsvEdgeRow::*column) {
	constexpr size_t n = sizeof(kGameEdge) / sizeof(kGameEdge[0]);
	if (d <= kGameEdge[0].distance) return kGameEdge[0].*column;
	for (size_t i = 1; i < n; i++)
		if (d < kGameEdge[i].distance) {
			const GdxsvEdgeRow &a = kGameEdge[i - 1], &b = kGameEdge[i];
			return a.*column + (b.*column - a.*column) * (d - a.distance) / (b.distance - a.distance);
		}
	return kGameEdge[n - 1].*column;
}

// Where the measured alpha first falls to a, game px.
float edge_where_alpha(float a) {
	constexpr size_t n = sizeof(kGameEdge) / sizeof(kGameEdge[0]);
	for (size_t i = 1; i < n; i++)
		if (kGameEdge[i].alpha < a) {
			const GdxsvEdgeRow &p = kGameEdge[i - 1], &q = kGameEdge[i];
			return p.distance + (q.distance - p.distance) * (p.alpha - a) / (p.alpha - q.alpha);
		}
	return kGameEdge[n - 1].distance;
}

// Squared distance from each element to the nearest zero of f (0 or a large value), along one row or column of n
// elements `step` apart (Felzenszwalb and Huttenlocher's lower envelope of parabolas).
void distance_1d(double* f, int n, int step, std::vector<double>& d, std::vector<int>& v, std::vector<double>& z) {
	int k = 0;
	v[0] = 0;
	z[0] = -1e20;
	z[1] = 1e20;
	for (int q = 1; q < n; q++) {
		double s;
		while (true) {
			const int p = v[k];
			s = ((f[q * step] + q * q) - (f[p * step] + p * p)) / (2.0 * (q - p));
			if (s > z[k] || k == 0) break;
			k--;
		}
		if (s <= z[k]) s = z[k];
		k++;
		v[k] = q;
		z[k] = s;
		z[k + 1] = 1e20;
	}
	k = 0;
	for (int q = 0; q < n; q++) {
		while (z[k + 1] < q) k++;
		const int p = v[k];
		d[q] = (q - p) * (q - p) + f[p * step];
	}
	for (int q = 0; q < n; q++) f[q * step] = d[q];
}

// Euclidean distance (px, between pixel centres) from each pixel to the nearest pixel where target is true.
std::vector<float> distance_to(const std::vector<bool>& target) {
	std::vector<double> f(kSize * kSize);
	for (int i = 0; i < kSize * kSize; i++) f[i] = target[i] ? 0 : 1e20;
	std::vector<double> d(kSize);
	std::vector<int> v(kSize);
	std::vector<double> z(kSize + 1);
	for (int x = 0; x < kSize; x++) distance_1d(&f[x], kSize, kSize, d, v, z);
	for (int y = 0; y < kSize; y++) distance_1d(&f[y * kSize], kSize, 1, d, v, z);
	std::vector<float> out(kSize * kSize);
	for (int i = 0; i < kSize * kSize; i++) out[i] = (float)std::sqrt(f[i]);
	return out;
}

u8 to_byte(float v) { return (u8)std::lround(std::clamp(v, 0.f, 1.f) * 255); }

}  // namespace

struct GdxsvFontProvider::Impl {
	struct Face {
		std::unique_ptr<u8[]> data;
		FT_Face face = nullptr;
		bool tried = false;
	};

	std::mutex mutex;
	FT_Library library = nullptr;
	Face font;

	// Rendered images by code point and style, most recent first.
	std::list<std::pair<u64, std::vector<u8>>> cache;
	std::unordered_map<u64, decltype(cache)::iterator> cache_index;

	u64 render_count = 0;
	u64 render_us = 0;

	FT_Face Open() {
		Face& f = font;
		if (f.tried) return f.face;
		f.tried = true;
		const auto start = std::chrono::steady_clock::now();
		if (library == nullptr && FT_Init_FreeType(&library) != 0) {
			library = nullptr;
			return nullptr;
		}
		size_t size = 0;
		f.data = resource::load(kFontFile, size);
		if (f.data == nullptr) {
			ERROR_LOG(COMMON, "FontRaster: %s not embedded", kFontFile);
			return nullptr;
		}
		if (FT_New_Memory_Face(library, f.data.get(), (FT_Long)size, 0, &f.face) != 0 ||
			FT_Set_Char_Size(f.face, 0, (FT_F26Dot6)(kEm * kScale * 64), 72, 72) != 0) {
			ERROR_LOG(COMMON, "FontRaster: cannot load %s (%u B)", kFontFile, (unsigned)size);
			if (f.face != nullptr) FT_Done_Face(f.face);
			f.face = nullptr;
			f.data.reset();
			return nullptr;
		}
		const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
		NOTICE_LOG(COMMON, "FontRaster: %s %s loaded in %d us, %u glyph textures", f.face->family_name, f.face->style_name, (int)us,
				   (unsigned)(sizeof(kGlyphs) / sizeof(kGlyphs[0])));
		return f.face;
	}

	void CloseLocked() {
		if (font.face != nullptr) FT_Done_Face(font.face);
		font.face = nullptr;
		font.data.reset();
		font.tried = false;
		if (library != nullptr) FT_Done_FreeType(library);
		library = nullptr;
		cache.clear();
		cache_index.clear();
	}

	// Draws the character in a 32x32 game px texture with the pen at the cell's left edge, like the game's glyphs.
	bool Draw(u32 code_point, GdxsvFontStyle style, std::vector<u8>& image) {
		FT_Face face = Open();
		if (face == nullptr) return false;
		const FT_UInt index = FT_Get_Char_Index(face, code_point);
		if (index == 0 || FT_Load_Glyph(face, index, FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING) != 0 ||
			face->glyph->format != FT_GLYPH_FORMAT_OUTLINE)
			return false;
		const bool name = style == GdxsvFontStyle::BattleName;
		const float f = name ? kNameScale : 1.f;
		const float fy = name ? 1.f : kTextHeight;
		const bool half = face->glyph->advance.x < kEm * kScale * 64 * 3 / 4;
		FT_Outline* outline = &face->glyph->outline;
		FT_Matrix scale = {(FT_Fixed)std::lround(f * 0x10000), 0, 0, (FT_Fixed)std::lround(f * fy * 0x10000)};
		FT_Outline_Transform(outline, &scale);
		// Outline coordinates are output px up from the texture's bottom left.
		const float x = (half ? kEm / 4 : kEm / 2) * (1 - f);
		const float baseline = 32 - kMiddleY - f * fy * (kBaseline - kMiddleY);
		FT_Outline_Translate(outline, std::lround(x * kScale * 64), std::lround(baseline * kScale * 64));
		if (fy != 1.f) {
			const FT_Pos grow = std::lround(kMediumStem * (1 - fy) * kScale * 64);
			FT_Outline_EmboldenXY(outline, 0, grow);
			FT_Outline_Translate(outline, 0, -((grow + 1) / 2));
		}
		if (name) {
			// Move the glyph sideways so that its outline fits in the shown box, or else crosses both sides equally
			FT_BBox box;
			FT_Outline_Get_BBox(outline, &box);
			const FT_Pos outline_width = std::lround(kNameOutlineWidth * kScale * 64);
			const FT_Pos left = box.xMin - outline_width;
			const FT_Pos right = std::lround(kNameBox * kScale * 64) - box.xMax - outline_width;
			const FT_Pos shift = left + right < 0 ? (right - left) / 2 : left < 0 ? -left : right < 0 ? right : 0;
			FT_Outline_Translate(outline, shift, 0);
		}
		std::vector<u8> cov(kSize * kSize, 0);
		FT_Bitmap bitmap = {};
		bitmap.rows = kSize;
		bitmap.width = kSize;
		bitmap.pitch = kSize;
		bitmap.buffer = cov.data();
		bitmap.num_grays = 256;
		bitmap.pixel_mode = FT_PIXEL_MODE_GRAY;
		if (FT_Outline_Get_Bitmap(library, outline, &bitmap) != 0) return false;

		// Signed distance from the 0.5 coverage outline, game px, + outside
		std::vector<bool> inside(kSize * kSize), outside(kSize * kSize);
		for (int i = 0; i < kSize * kSize; i++) {
			inside[i] = cov[i] >= 128;
			outside[i] = !inside[i];
		}
		image.assign(kSize * kSize * 4, 0);
		if (std::find(inside.begin(), inside.end(), true) == inside.end()) return true;
		const std::vector<float> to_outside = distance_to(outside);
		const std::vector<float> to_inside = distance_to(inside);
		const float cut = kEdgeScale * edge_where_alpha(kRimCut);
		for (int i = 0; i < kSize * kSize; i++) {
			const float d = (inside[i] ? 0.5f - to_outside[i] : to_inside[i] - 0.5f) / kScale;
			float lum, alpha;
			if (name) {
				const float a = kNameOutlineOpacity * std::clamp((kNameOutlineWidth - d) / kNameOutlineFade, 0.f, 1.f);
				const float c = cov[i] / 255.f;
				alpha = c + a * (1 - c);
				lum = alpha > 0 ? (c + kNameOutlineGrey * a * (1 - c)) / alpha : 1;
			} else {
				alpha = std::clamp((cut - d) / kRimFade + 0.5f, 0.f, 1.f);
				lum = std::max(edge_at(d / kEdgeScale, &GdxsvEdgeRow::luminance), kRimFloor) * alpha;
			}
			u8* p = &image[i * 4];
			p[0] = p[1] = p[2] = to_byte(lum);
			p[3] = to_byte(alpha);
		}
		return true;
	}
};

GdxsvFontProvider::GdxsvFontProvider() : impl(new Impl()) {}

GdxsvFontProvider::~GdxsvFontProvider() { Close(); }

bool GdxsvFontProvider::HasGlyph(u32 hash) const { return find_glyph(hash) != nullptr; }

u8* GdxsvFontProvider::Render(u32 hash, GdxsvFontStyle style, int& width, int& height) {
	const GdxsvFontGlyph* glyph = find_glyph(hash);
	return glyph != nullptr ? RenderCodePoint(glyph->code_point, style, width, height) : nullptr;
}

bool GdxsvFontProvider::HasCodePoint(u32 code_point) {
	std::lock_guard<std::mutex> lock(impl->mutex);
	FT_Face face = impl->Open();
	return face != nullptr && FT_Get_Char_Index(face, code_point) != 0;
}

u8* GdxsvFontProvider::RenderCodePoint(u32 code_point, GdxsvFontStyle style, int& width, int& height) {
	const u64 key = (u64)style << 32 | code_point;
	std::lock_guard<std::mutex> lock(impl->mutex);
	auto it = impl->cache_index.find(key);
	if (it == impl->cache_index.end()) {
		const auto start = std::chrono::steady_clock::now();
		std::vector<u8> image;
		if (!impl->Draw(code_point, style, image)) return nullptr;
		impl->cache.emplace_front(key, std::move(image));
		impl->cache_index[key] = impl->cache.begin();
		if (impl->cache.size() > kCacheEntries) {
			impl->cache_index.erase(impl->cache.back().first);
			impl->cache.pop_back();
		}
		impl->render_us += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
		if (++impl->render_count % 256 == 1)
			NOTICE_LOG(COMMON, "FontRaster: %u glyphs drawn, %u us each", (unsigned)impl->render_count,
					   (unsigned)(impl->render_us / impl->render_count));
	} else {
		impl->cache.splice(impl->cache.begin(), impl->cache, it->second);
	}
	const std::vector<u8>& image = impl->cache.front().second;
	u8* data = (u8*)std::malloc(image.size());
	if (data == nullptr) return nullptr;
	memcpy(data, image.data(), image.size());
	width = height = kSize;
	return data;
}

void GdxsvFontProvider::Close() {
	std::lock_guard<std::mutex> lock(impl->mutex);
	impl->CloseLocked();
}
