#include "render/renderer.h"

#include "core/constants.h"

#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace guard {
namespace {

// 特效文件缺失时的兜底：纯拷贝，保证画面不会黑
const char *kFallbackEffect = R"(
uniform float4x4 ViewProj;
uniform texture2d image;

sampler_state linear_sampler {
	AddressU = Clamp;
	AddressV = Clamp;
	Filter   = Linear;
};

struct VertIn { float4 pos : POSITION; float2 uv : TEXCOORD0; };
struct VertOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };

VertOut VSDefault(VertIn v_in)
{
	VertOut v_out;
	v_out.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);
	v_out.uv  = v_in.uv;
	return v_out;
}

float4 PSBase(VertIn v_in) : TARGET
{
	return image.Sample(linear_sampler, v_in.uv);
}

technique Draw
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSBase(v_in);
	}
}
)";

} // namespace

bool Renderer::init(std::string &err)
{
	destroy();

	char *path = obs_module_file("effects/guard_mask.effect");
	char *err_str = nullptr;
	if (path) {
		effect_ = gs_effect_create_from_file(path, &err_str);
		bfree(path);
	}
	if (!effect_) {
		err = err_str ? err_str : "无法加载 effects/guard_mask.effect";
		if (err_str)
			bfree(err_str);
		blog(LOG_ERROR, "[obs-guard] %s", err.c_str());
	}

	// 兜底特效始终创建
	err_str = nullptr;
	fallback_ = gs_effect_create(kFallbackEffect, "guard_fallback", &err_str);
	if (!fallback_ && err_str) {
		blog(LOG_ERROR, "[obs-guard] 兜底特效创建失败: %s", err_str);
		bfree(err_str);
	}

	if (!effect_)
		return false;

	p_texel = gs_effect_get_param_by_name(effect_, "texel");
	p_pad = gs_effect_get_param_by_name(effect_, "pad_uv");
	p_feather = gs_effect_get_param_by_name(effect_, "feather_uv");
	p_color = gs_effect_get_param_by_name(effect_, "mask_color");
	p_opacity = gs_effect_get_param_by_name(effect_, "base_opacity");
	p_style = gs_effect_get_param_by_name(effect_, "style");
	p_blur = gs_effect_get_param_by_name(effect_, "blur_uv");
	p_block = gs_effect_get_param_by_name(effect_, "block_uv");
	p_count = gs_effect_get_param_by_name(effect_, "count");
	p_sec = gs_effect_get_param_by_name(effect_, "sec");
	p_roi = gs_effect_get_param_by_name(effect_, "roi_rect");
	p_roi_on = gs_effect_get_param_by_name(effect_, "roi_on");
	p_box_on = gs_effect_get_param_by_name(effect_, "box_on");
	p_conf_on = gs_effect_get_param_by_name(effect_, "conf_on");

	for (int i = 0; i < MaskFrame::kMax; ++i) {
		char name[8];
		snprintf(name, sizeof(name), "m%d", i);
		p_m[i] = gs_effect_get_param_by_name(effect_, name);
		snprintf(name, sizeof(name), "f%d", i);
		p_f[i] = gs_effect_get_param_by_name(effect_, name);
		snprintf(name, sizeof(name), "s%d", i);
		p_s[i] = gs_effect_get_param_by_name(effect_, name);
	}

	// 任一必需参数缺失（特效文件被改坏 / 版本不匹配）就退回兜底，避免空指针
	const bool ok = p_texel && p_pad && p_feather && p_color && p_opacity && p_style &&
			p_blur && p_block && p_count && p_sec && p_roi && p_roi_on &&
			p_box_on && p_conf_on && p_m[0] && p_f[0] && p_s[0];
	if (!ok) {
		err = "guard_mask.effect 缺少必需的 uniform 参数";
		blog(LOG_ERROR, "[obs-guard] %s", err.c_str());
		gs_effect_destroy(effect_);
		effect_ = nullptr;
		return false;
	}

	return true;
}

void Renderer::destroy()
{
	if (effect_) {
		gs_effect_destroy(effect_);
		effect_ = nullptr;
	}
	if (fallback_) {
		gs_effect_destroy(fallback_);
		fallback_ = nullptr;
	}
	std::memset(p_m, 0, sizeof(p_m));
	std::memset(p_f, 0, sizeof(p_f));
}

void Renderer::set_mask(int i, float x, float y, float w, float h, float fade, float score)
{
	if (!p_m[i] || !p_f[i])
		return;
	struct vec4 r;
	vec4_set(&r, x, y, w, h);
	gs_effect_set_vec4(p_m[i], &r);
	gs_effect_set_float(p_f[i], fade);
	if (p_s[i])
		gs_effect_set_float(p_s[i], score);
}

void Renderer::clear_masks()
{
	for (int i = 0; i < MaskFrame::kMax; ++i)
		set_mask(i, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
}

void Renderer::draw(obs_source_t *context, uint32_t w, uint32_t h, const Config &cfg,
		    const MaskFrame &mask, const RoiRect &roi, int64_t now, bool test_mode)
{
	// 特效全挂时退化为直通，绝不让画面黑掉
	gs_effect_t *fx = effect_ ? effect_ : fallback_;
	if (!fx) {
		if (obs_source_t *target = obs_filter_get_target(context))
			obs_source_video_render(target);
		return;
	}

	if (!obs_source_process_filter_begin(context, GS_RGBA, OBS_NO_DIRECT_RENDERING))
		return;

	if (fx != effect_) {
		obs_source_process_filter_end(context, fx, w, h);
		return;
	}

	const float fw = (float)std::max(1u, w);
	const float fh = (float)std::max(1u, h);

	struct vec4 texel;
	vec4_set(&texel, 1.0f / fw, 1.0f / fh, fw, fh);
	gs_effect_set_vec4(p_texel, &texel);

	struct vec2 pad;
	vec2_set(&pad, cfg.pad_px / fw, cfg.pad_px / fh);
	gs_effect_set_vec2(p_pad, &pad);

	struct vec2 feather;
	vec2_set(&feather, 1.0f / fw, 1.0f / fh);
	gs_effect_set_vec2(p_feather, &feather);

	const uint32_t c = cfg.mask_color;
	struct vec3 color;
	vec3_set(&color, (float)(c & 0xFF) / 255.0f, (float)((c >> 8) & 0xFF) / 255.0f,
		 (float)((c >> 16) & 0xFF) / 255.0f);
	gs_effect_set_vec3(p_color, &color);
	gs_effect_set_float(p_opacity, cfg.mask_opacity);
	gs_effect_set_float(p_style, (float)cfg.mask_kind);
	gs_effect_set_float(p_sec, (float)((double)now / 1e9));

	// 模糊 / 马赛克尺寸按 1080p 归一，换分辨率观感一致
	const float scale = fh / 1080.0f;
	struct vec2 blur;
	vec2_set(&blur, cfg.blur_px * scale / fw, cfg.blur_px * scale / fh);
	gs_effect_set_vec2(p_blur, &blur);

	struct vec2 block;
	vec2_set(&block, std::max(2.0f, cfg.block_px * scale) / fw,
		 std::max(2.0f, cfg.block_px * scale) / fh);
	gs_effect_set_vec2(p_block, &block);

	struct vec4 roi_r;
	vec4_set(&roi_r, roi.x, roi.y, roi.w, roi.h);
	gs_effect_set_vec4(p_roi, &roi_r);
	gs_effect_set_float(p_roi_on, cfg.debug_roi ? 1.0f : 0.0f);
	gs_effect_set_float(p_box_on, cfg.debug_box ? 1.0f : 0.0f);
	gs_effect_set_float(p_conf_on, cfg.debug_conf ? 1.0f : 0.0f);

	clear_masks();

	int n = 0;
	if (test_mode) {
		// 自检：用一条满 ROI 的样本验证取景与样式
		set_mask(0, roi.x, roi.y, roi.w, roi.h, 1.0f, 0.0f);
		n = 1;
	} else if (mask.count > 0) {
		const int64_t hold_ns = (int64_t)std::max(0, cfg.hold_ms) * 1000000LL;
		const int64_t fade_ns = (int64_t)std::max(1, cfg.fade_ms) * 1000000LL;

		for (int i = 0; i < mask.count && n < MaskFrame::kMax; ++i) {
			const MaskCmd &m = mask.items[i];
			const int64_t age = now - m.first_seen;
			const int64_t idle = now - m.last_seen;

			float fade = 1.0f;
			if (age >= 0 && age < kFadeInNs)
				fade = (float)age / (float)kFadeInNs;
			if (idle > hold_ns)
				fade *= 1.0f - (float)(idle - hold_ns) / (float)fade_ns;
			if (m.state == 0)
				fade *= 0.55f; // SUSPECT：轻度遮挡

			fade = std::clamp(fade, 0.0f, 1.0f);
			if (fade <= 0.002f)
				continue;

			set_mask(n, m.x, m.y, m.w, m.h, fade, m.score);
			++n;
		}
	}

	gs_effect_set_float(p_count, (float)n);
	obs_source_process_filter_end(context, fx, w, h);
}

} // namespace guard
