// render/renderer.h -- 遮罩绘制（单 pass，渲染线程内耗时目标 ≤2ms）
#pragma once

#include <graphics/graphics.h>
#include <obs-module.h>

#include <string>

#include "core/config.h"
#include "core/types.h"

namespace guard {

class Renderer {
public:
	~Renderer() { destroy(); }

	bool init(std::string &err);
	void destroy();

	// 在渲染线程调用；内部完成 process_filter_begin/end
	void draw(obs_source_t *context, uint32_t w, uint32_t h, const Config &cfg,
		  const MaskFrame &mask, const RoiRect &roi, int64_t now, bool test_mode);

private:
	gs_effect_t *effect_ = nullptr;
	gs_effect_t *fallback_ = nullptr;

	gs_eparam_t *p_texel = nullptr;
	gs_eparam_t *p_pad = nullptr;
	gs_eparam_t *p_feather = nullptr;
	gs_eparam_t *p_color = nullptr;
	gs_eparam_t *p_opacity = nullptr;
	gs_eparam_t *p_style = nullptr;
	gs_eparam_t *p_blur = nullptr;
	gs_eparam_t *p_block = nullptr;
	gs_eparam_t *p_count = nullptr;
	gs_eparam_t *p_sec = nullptr;
	gs_eparam_t *p_roi = nullptr;
	gs_eparam_t *p_roi_on = nullptr;
	gs_eparam_t *p_box_on = nullptr;
	gs_eparam_t *p_conf_on = nullptr;
	gs_eparam_t *p_m[MaskFrame::kMax] = {};
	gs_eparam_t *p_f[MaskFrame::kMax] = {};
	gs_eparam_t *p_s[MaskFrame::kMax] = {};

	void set_mask(int i, float x, float y, float w, float h, float fade, float score);
	void clear_masks();
};

} // namespace guard
