#include "core/config.h"

#include <util/platform.h>

#include <algorithm>
#include <cstring>

namespace guard {
namespace {

// ROI 夹到 [0,1]，并保证宽高为正
RoiRect normalize_roi(RoiRect r)
{
	r.x = std::clamp(r.x, 0.0f, 1.0f);
	r.y = std::clamp(r.y, 0.0f, 1.0f);
	r.w = std::clamp(r.w, 0.0f, 1.0f - r.x);
	r.h = std::clamp(r.h, 0.0f, 1.0f - r.y);
	return r;
}

} // namespace

RoiRect Config::roi_rect(uint32_t src_w, uint32_t src_h) const
{
	RoiRect r;
	if (coord_mode == COORD_PIXEL) {
		if (!src_w || !src_h)
			return r;
		r.x = (float)(px[0] / (double)src_w);
		r.y = (float)(px[1] / (double)src_h);
		r.w = (float)(px[2] / (double)src_w);
		r.h = (float)(px[3] / (double)src_h);
	} else {
		r.x = (float)(pct[0] / 100.0);
		r.y = (float)(pct[1] / 100.0);
		r.w = (float)(pct[2] / 100.0);
		r.h = (float)(pct[3] / 100.0);
	}
	return normalize_roi(r);
}

void config_defaults(obs_data_t *s)
{
	obs_data_set_default_bool(s, "enabled", true);
	obs_data_set_default_int(s, "det_fps", 10);
	obs_data_set_default_int(s, "fail_mode", FAIL_OPEN);

	obs_data_set_default_int(s, "coord_mode", COORD_PERCENT);
	obs_data_set_default_double(s, "pct_x", 0.0);
	obs_data_set_default_double(s, "pct_y", 0.0);
	obs_data_set_default_double(s, "pct_w", 100.0);
	obs_data_set_default_double(s, "pct_h", 100.0);
	obs_data_set_default_int(s, "px_x", 0);
	obs_data_set_default_int(s, "px_y", 0);
	obs_data_set_default_int(s, "px_w", 1280);
	obs_data_set_default_int(s, "px_h", 720);

	obs_data_set_default_string(s, "model_path", "");
	obs_data_set_default_int(s, "device", DEVICE_AUTO);
	obs_data_set_default_double(s, "conf", 0.35);
	obs_data_set_default_double(s, "iou", 0.45);
	obs_data_set_default_int(s, "max_det", 32);
	obs_data_set_default_string(s, "class_filter", "");

	obs_data_set_default_int(s, "mask_kind", MASK_MOSAIC);
	obs_data_set_default_int(s, "mask_color", (long long)0xFF000000u);
	obs_data_set_default_double(s, "mask_opacity", 0.85);
	obs_data_set_default_double(s, "blur_px", 16.0);
	obs_data_set_default_double(s, "block_px", 16.0);
	obs_data_set_default_double(s, "expand", 0.10);
	obs_data_set_default_double(s, "pad_px", 4.0);

	obs_data_set_default_int(s, "hold_ms", 300);
	obs_data_set_default_int(s, "fade_ms", 180);

	obs_data_set_default_bool(s, "debug_roi", false);
	obs_data_set_default_bool(s, "debug_box", false);
	obs_data_set_default_bool(s, "debug_conf", false);
}

void config_load(Config &c, obs_data_t *s)
{
	c.enabled = obs_data_get_bool(s, "enabled");
	c.det_fps = (int)obs_data_get_int(s, "det_fps");
	c.fail_mode = (int)obs_data_get_int(s, "fail_mode");

	c.coord_mode = (int)obs_data_get_int(s, "coord_mode");
	c.pct[0] = obs_data_get_double(s, "pct_x");
	c.pct[1] = obs_data_get_double(s, "pct_y");
	c.pct[2] = obs_data_get_double(s, "pct_w");
	c.pct[3] = obs_data_get_double(s, "pct_h");
	c.px[0] = (double)obs_data_get_int(s, "px_x");
	c.px[1] = (double)obs_data_get_int(s, "px_y");
	c.px[2] = (double)obs_data_get_int(s, "px_w");
	c.px[3] = (double)obs_data_get_int(s, "px_h");

	const char *path = obs_data_get_string(s, "model_path");
	c.model_path = path ? path : "";
	c.device = (int)obs_data_get_int(s, "device");
	c.conf = (float)obs_data_get_double(s, "conf");
	c.iou = (float)obs_data_get_double(s, "iou");
	c.max_det = (int)obs_data_get_int(s, "max_det");
	const char *cf = obs_data_get_string(s, "class_filter");
	c.class_filter = cf ? cf : "";

	c.mask_kind = (int)obs_data_get_int(s, "mask_kind");
	c.mask_color = (uint32_t)obs_data_get_int(s, "mask_color");
	c.mask_opacity = (float)obs_data_get_double(s, "mask_opacity");
	c.blur_px = (float)obs_data_get_double(s, "blur_px");
	c.block_px = (float)obs_data_get_double(s, "block_px");
	c.expand = (float)obs_data_get_double(s, "expand");
	c.pad_px = (float)obs_data_get_double(s, "pad_px");

	c.hold_ms = (int)obs_data_get_int(s, "hold_ms");
	c.fade_ms = (int)obs_data_get_int(s, "fade_ms");

	c.debug_roi = obs_data_get_bool(s, "debug_roi");
	c.debug_box = obs_data_get_bool(s, "debug_box");
	c.debug_conf = obs_data_get_bool(s, "debug_conf");

	c.det_fps = std::clamp(c.det_fps, 1, 30);
	c.conf = std::clamp(c.conf, 0.01f, 0.99f);
	c.iou = std::clamp(c.iou, 0.01f, 0.99f);
	c.expand = std::clamp(c.expand, 0.0f, 0.30f);
	c.mask_opacity = std::clamp(c.mask_opacity, 0.0f, 1.0f);
}

bool config_needs_reload(const Config &a, const Config &b)
{
	return a.model_path != b.model_path || a.device != b.device ||
	       a.conf != b.conf || a.iou != b.iou || a.max_det != b.max_det ||
	       a.class_filter != b.class_filter;
}

} // namespace guard
