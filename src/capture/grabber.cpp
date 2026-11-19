#include "capture/grabber.h"

#include <graphics/vec4.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace guard {

void Grabber::release()
{
	if (tr_) {
		gs_texrender_destroy(tr_);
		tr_ = nullptr;
		tr_w_ = tr_h_ = 0;
	}
	if (stage_) {
		gs_stagesurface_destroy(stage_);
		stage_ = nullptr;
		st_w_ = st_h_ = 0;
	}
}

bool Grabber::ensure_texrender(uint32_t w, uint32_t h)
{
	if (tr_ && tr_w_ == w && tr_h_ == h)
		return true;
	if (tr_) {
		gs_texrender_destroy(tr_);
		tr_ = nullptr;
	}
	tr_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (!tr_)
		return false;
	tr_w_ = w;
	tr_h_ = h;
	return true;
}

bool Grabber::ensure_stage(uint32_t w, uint32_t h)
{
	if (stage_ && st_w_ == w && st_h_ == h)
		return true;
	if (stage_) {
		gs_stagesurface_destroy(stage_);
		stage_ = nullptr;
	}
	stage_ = gs_stagesurface_create(w, h, GS_RGBA);
	if (!stage_)
		return false;
	st_w_ = w;
	st_h_ = h;
	return true;
}

bool Grabber::grab(obs_source_t *target, uint32_t src_w, uint32_t src_h, const RoiRect &roi,
		   int det_w, int det_h, FrameView &out)
{
	if (!target || !src_w || !src_h || det_w <= 0 || det_h <= 0)
		return false;
	if (!roi.valid())
		return false;
	if (!ensure_texrender(src_w, src_h))
		return false;

	// --- 1) 输入源 → texrender（GPU only）---
	gs_texrender_reset(tr_);
	if (!gs_texrender_begin(tr_, src_w, src_h))
		return false;
	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	obs_source_video_render(target);
	gs_texrender_end(tr_);

	gs_texture_t *tex = gs_texrender_get_texture(tr_);
	if (!tex)
		return false;

	// --- 2) 回读 ---
	if (!ensure_stage(src_w, src_h))
		return false;
	gs_stage_texture(stage_, tex);

	uint8_t *data = nullptr;
	uint32_t linesize = 0;
	if (!gs_stagesurface_map(stage_, &data, &linesize) || !data)
		return false;

	// --- 3) ROI 裁剪 + 双线性缩放 + letterbox ---
	int sw = (int)src_w, sh = (int)src_h;
	int rx = (int)std::lround(roi.x * sw);
	int ry = (int)std::lround(roi.y * sh);
	int rw = (int)std::lround(roi.w * sw);
	int rh = (int)std::lround(roi.h * sh);
	rx = std::clamp(rx, 0, sw - 1);
	ry = std::clamp(ry, 0, sh - 1);
	rw = std::clamp(rw, 1, sw - rx);
	rh = std::clamp(rh, 1, sh - ry);

	out.width = det_w;
	out.height = det_h;
	out.rgb.assign((size_t)det_w * (size_t)det_h * 3u, 0);
	out.roi = roi;

	// 等比缩放，居中留黑边（保持目标长宽比，避免压制召回）
	const float scale = std::min((float)det_w / (float)rw, (float)det_h / (float)rh);
	const int cw = std::max(1, (int)std::lround((float)rw * scale));
	const int ch = std::max(1, (int)std::lround((float)rh * scale));
	const int ox = (det_w - cw) / 2;
	const int oy = (det_h - ch) / 2;

	out.pad_x = (float)ox / (float)det_w;
	out.pad_y = (float)oy / (float)det_h;

	uint8_t *dst_base = out.rgb.data();
	for (int y = 0; y < ch; ++y) {
		float sy = (float)ry + (float)(y + 1) / scale - 0.5f;
		int y0 = (int)std::floor(sy);
		float fy = sy - (float)y0;
		int y1 = std::min(y0 + 1, sh - 1);
		y0 = std::clamp(y0, 0, sh - 1);

		const uint8_t *row0 = data + (size_t)y0 * linesize;
		const uint8_t *row1 = data + (size_t)y1 * linesize;
		uint8_t *dst = dst_base + ((size_t)(y + oy) * (size_t)det_w + (size_t)ox) * 3u;

		for (int x = 0; x < cw; ++x) {
			float sx = (float)rx + (float)(x + 1) / scale - 0.5f;
			int x0 = (int)std::floor(sx);
			float fx = sx - (float)x0;
			int x1 = std::min(x0 + 1, sw - 1);
			x0 = std::clamp(x0, 0, sw - 1);

			const uint8_t *p00 = row0 + (size_t)x0 * 4u;
			const uint8_t *p10 = row0 + (size_t)x1 * 4u;
			const uint8_t *p01 = row1 + (size_t)x0 * 4u;
			const uint8_t *p11 = row1 + (size_t)x1 * 4u;

			for (int c = 0; c < 3; ++c) {
				float top = (float)p00[c] * (1.0f - fx) + (float)p10[c] * fx;
				float bot = (float)p01[c] * (1.0f - fx) + (float)p11[c] * fx;
				float v = top * (1.0f - fy) + bot * fy;
				dst[x * 3 + c] = (uint8_t)(v + 0.5f);
			}
		}
	}

	gs_stagesurface_unmap(stage_);
	return true;
}

} // namespace guard
