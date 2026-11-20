// filter/properties.cpp -- 属性面板构建与变更回调
#include "filter/filter_internal.h"

#include <obs-module.h>
#include <util/platform.h>

#include <initializer_list>
#include <string>
#include <utility>

#include "core/constants.h"

namespace guard {
namespace {

// 下拉框：把「名称 + 取值」的重复样板收敛成一次调用
obs_property_t *add_combo(obs_properties_t *p, const char *name, const char *title,
			  std::initializer_list<std::pair<const char *, int>> items)
{
	obs_property_t *prop = obs_properties_add_list(p, name, title, OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_INT);
	for (const auto &it : items)
		obs_property_list_add_int(prop, it.first, it.second);
	return prop;
}

// 分组标题
void add_section(obs_properties_t *p, const char *name, const char *title)
{
	obs_properties_add_text(p, name, title, OBS_TEXT_INFO);
}

// 两套坐标系字段的显隐，构面板与回调共用同一份键名表
void apply_roi_visibility(obs_properties_t *props, bool pixel_mode)
{
	for (const char *key : kRoiPercentKeys) {
		if (obs_property_t *prop = obs_properties_get(props, key))
			obs_property_set_visible(prop, !pixel_mode);
	}
	for (const char *key : kRoiPixelKeys) {
		if (obs_property_t *prop = obs_properties_get(props, key))
			obs_property_set_visible(prop, pixel_mode);
	}
}

// ---------------------------------------------------------------------------
// 变更回调
// ---------------------------------------------------------------------------

bool on_coord_mode_changed(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	apply_roi_visibility(props, obs_data_get_int(settings, "coord_mode") == COORD_PIXEL);
	return true;
}

bool on_mask_kind_changed(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const int kind = (int)obs_data_get_int(settings, "mask_kind");
	if (obs_property_t *prop = obs_properties_get(props, "blur_px"))
		obs_property_set_visible(prop, kind == MASK_BLUR);
	if (obs_property_t *prop = obs_properties_get(props, "block_px"))
		obs_property_set_visible(prop, kind == MASK_MOSAIC);
	return true;
}

bool on_test_mask(obs_properties_t *, obs_property_t *, void *data)
{
	auto *f = static_cast<GuardFilter *>(data);
	f->test_until.store(os_gettime_ns() + kTestMaskNs);
	return false;
}

bool on_refresh_status(obs_properties_t *props, obs_property_t *, void *data)
{
	auto *f = static_cast<GuardFilter *>(data);
	const std::string s = f->pipeline ? f->pipeline->status() : std::string("-");
	if (obs_property_t *prop = obs_properties_get(props, "status"))
		obs_property_set_description(prop, s.c_str());
	return true;
}

} // namespace

// ---------------------------------------------------------------------------
// 面板
// ---------------------------------------------------------------------------

obs_properties_t *build_properties(void *data, void *type_data)
{
	obs_properties_t *p = obs_properties_create();

	// type_data 在滤镜场景下即 obs_source_t*（OBS 33 声明为 void*）
	obs_source_t *source = static_cast<obs_source_t *>(type_data);

	bool pixel_mode = false;
	int mask_kind = MASK_MOSAIC;
	std::string status = "未初始化";

	if (source) {
		obs_data_t *s = obs_source_get_settings(source);
		pixel_mode = obs_data_get_int(s, "coord_mode") == COORD_PIXEL;
		mask_kind = (int)obs_data_get_int(s, "mask_kind");
		obs_data_release(s);
	}
	if (data) {
		auto *f = static_cast<GuardFilter *>(data);
		if (f->pipeline)
			status = f->pipeline->status();
	}

	// ---------------- 基本 ----------------
	add_section(p, "sec_basic", tr("Sec.Basic"));
	obs_properties_add_bool(p, "enabled", tr("Enabled"));

	const int max_hz = display_max_refresh_hz();
	obs_property_t *det_fps = obs_properties_add_int_slider(
		p, "det_fps", tr("DetFps"), kMinDetFps, max_hz, 1);
	obs_property_int_set_suffix(det_fps, " fps");
	obs_property_set_long_description(det_fps, tr("DetFps.Desc"));

	obs_property_set_long_description(
		add_combo(p, "fail_mode", tr("FailMode"),
			  {{tr("FailOpen"), FAIL_OPEN}, {tr("FailClosed"), FAIL_CLOSED}}),
		tr("FailMode.Desc"));

	// ---------------- 检测区域 ----------------
	add_section(p, "sec_roi", tr("Sec.Roi"));

	obs_property_t *coord = add_combo(p, "coord_mode", tr("CoordMode"),
					  {{tr("CoordPercent"), COORD_PERCENT},
					   {tr("CoordPixel"), COORD_PIXEL}});
	obs_property_set_modified_callback(coord, on_coord_mode_changed);
	obs_property_set_long_description(coord, tr("CoordMode.Desc"));

	obs_properties_add_text(p, "roi_hint", tr("RoiHint"), OBS_TEXT_INFO);

	const char *const labels[4] = {tr("RoiX"), tr("RoiY"), tr("RoiW"), tr("RoiH")};
	for (int i = 0; i < 4; ++i) {
		// 坐标下限为 0；宽高下限为 1，避免拖出零面积区域
		const double minPct = (i < 2) ? 0.0 : 1.0;
		const int minPx = (i < 2) ? 0 : 1;
		obs_property_t *pct = obs_properties_add_float_slider(
			p, kRoiPercentKeys[i], labels[i], minPct, 100.0, 0.1);
		obs_property_float_set_suffix(pct, " %");

		obs_property_t *px = obs_properties_add_int(
			p, kRoiPixelKeys[i], labels[i], minPx, kRoiPixelMax, 1);
		obs_property_int_set_suffix(px, " px");
	}
	obs_property_set_long_description(obs_properties_get(p, kRoiPercentKeys[0]),
					  tr("RoiPct.Desc"));
	obs_property_set_long_description(obs_properties_get(p, kRoiPixelKeys[0]),
					  tr("RoiPx.Desc"));
	apply_roi_visibility(p, pixel_mode);

	// ---------------- 模型 ----------------
	add_section(p, "sec_model", tr("Sec.Model"));

	obs_property_set_long_description(
		obs_properties_add_path(p, "model_path", tr("ModelPath"), OBS_PATH_FILE,
					tr("ModelFilter"), nullptr),
		tr("ModelPath.Desc"));

	add_combo(p, "device", tr("Device"),
		  {{tr("DeviceAuto"), DEVICE_AUTO},
		   {tr("DeviceDml"), DEVICE_DML},
		   {tr("DeviceCpu"), DEVICE_CPU}});

	obs_property_set_long_description(
		obs_properties_add_float_slider(p, "conf", tr("Conf"), 0.01, 0.99, 0.01),
		tr("Conf.Desc"));

	obs_properties_add_float_slider(p, "iou", tr("Iou"), 0.10, 0.95, 0.01);
	obs_property_int_set_suffix(
		obs_properties_add_int_slider(p, "max_det", tr("MaxDet"), 1, 64, 1), " 个");

	obs_property_t *limit = obs_properties_add_int_slider(
		p, "mask_limit", tr("MaskLimit"), 1, MaskFrame::kMax, 1);
	obs_property_int_set_suffix(limit, " 个");
	obs_property_set_long_description(limit, tr("MaskLimit.Desc"));

	obs_property_set_long_description(
		obs_properties_add_text(p, "class_filter", tr("ClassFilter"), OBS_TEXT_DEFAULT),
		tr("ClassFilter.Desc"));

	// ---------------- 遮罩样式 ----------------
	add_section(p, "sec_mask", tr("Sec.Mask"));

	obs_property_t *mask = add_combo(p, "mask_kind", tr("MaskKind"),
					 {{tr("MaskSolid"), MASK_SOLID},
					  {tr("MaskBlur"), MASK_BLUR},
					  {tr("MaskMosaic"), MASK_MOSAIC}});
	obs_property_set_modified_callback(mask, on_mask_kind_changed);
	obs_property_set_long_description(mask, tr("MaskKind.Desc"));

	obs_properties_add_color(p, "mask_color", tr("MaskColor"));
	obs_properties_add_float_slider(p, "mask_opacity", tr("MaskOpacity"), 0.05, 1.0, 0.01);
	obs_property_t *blur = obs_properties_add_float_slider(
		p, "blur_px", tr("BlurPx"), 2.0, 64.0, 1.0);
	obs_property_float_set_suffix(blur, " px");
	obs_property_set_visible(blur, mask_kind == MASK_BLUR);

	obs_property_t *block = obs_properties_add_float_slider(
		p, "block_px", tr("BlockPx"), 4.0, 64.0, 1.0);
	obs_property_float_set_suffix(block, " px");
	obs_property_set_visible(block, mask_kind == MASK_MOSAIC);

	obs_properties_add_float_slider(p, "expand", tr("Expand"), 0.0, 0.30, 0.01);
	obs_property_float_set_suffix(
		obs_properties_add_float_slider(p, "pad_px", tr("PadPx"), 0.0, 24.0, 1.0),
		" px");

	// ---------------- 平滑 ----------------
	add_section(p, "sec_smooth", tr("Sec.Smooth"));
	obs_property_int_set_suffix(
		obs_properties_add_int_slider(p, "hold_ms", tr("HoldMs"), 0, 1000, 10),
		" ms");
	obs_property_int_set_suffix(
		obs_properties_add_int_slider(p, "fade_ms", tr("FadeMs"), 0, 800, 10),
		" ms");

	// ---------------- 诊断 ----------------
	add_section(p, "sec_debug", tr("Sec.Debug"));
	obs_properties_add_bool(p, "debug_roi", tr("DebugRoi"));
	obs_properties_add_bool(p, "debug_box", tr("DebugBox"));
	obs_property_set_long_description(
		obs_properties_add_bool(p, "debug_conf", tr("DebugConf")),
		tr("DebugConf.Desc"));

	obs_property_set_description(
		obs_properties_add_text(p, "status", tr("Status"), OBS_TEXT_INFO),
		status.c_str());

	// button2 的最后一个参数会原样传给回调，这里传滤镜实例
	obs_properties_add_button2(p, "refresh", tr("Refresh"), on_refresh_status, data);
	obs_properties_add_button2(p, "test_mask", tr("TestMask"), on_test_mask, data);
	obs_properties_add_text(p, "test_hint", tr("TestMask.Desc"), OBS_TEXT_INFO);

	return p;
}

} // namespace guard
