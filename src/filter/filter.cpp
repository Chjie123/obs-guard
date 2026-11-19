// guard-filter.cpp -- OBS 视频滤镜本体
//
// 挂载方式（规划 §7.2）：
//   源级：直接给「视频采集设备 / 显示器采集」加本滤镜
//   场景级：新建审核场景，把原场景作为「场景源」加入，再对该场景源加滤镜
//
// 渲染时序（每帧）：
//   [检测节拍到] 取 ROI -> 投递推理线程（不等待）
//   [每帧]       try_lock 取最新遮罩结果（拿不到就复用上一帧）
//                process_filter_begin -> 设置 uniform -> process_filter_end
#include "filter/filter.h"

#include "core/config.h"
#include "capture/grabber.h"
#include "core/pipeline.h"
#include "render/renderer.h"
#include "core/types.h"

#include <obs-module.h>
#include <util/platform.h>

#include <atomic>
#include <cmath>
#include <memory>
#include <string>

namespace guard {
namespace {

constexpr int64_t kFailClosedGraceNs = 3000LL * 1000000LL;

struct GuardFilter {
	obs_source_t *context = nullptr;
	// 配置由 UI 线程写、渲染线程读：用原子替换的 shared_ptr，避免字符串撕裂
	std::shared_ptr<const Config> cfg;
	int64_t start_ns = 0;

	std::unique_ptr<Pipeline> pipeline;
	Renderer renderer;
	Grabber grabber;

	uint32_t width = 0;
	uint32_t height = 0;
	int64_t last_grab_ns = 0;
	int64_t next_frame_id = 0;

	MaskFrame mask; // 渲染线程私有副本
	bool in_grab = false;

	std::atomic<int64_t> test_until{0};
};

#define T_(key) obs_module_text(key)

// ---------------------------------------------------------------------------
// 属性变更回调
// ---------------------------------------------------------------------------

bool on_coord_mode_changed(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const bool pixel = obs_data_get_int(settings, "coord_mode") == COORD_PIXEL;
	const char *pct[] = {"pct_x", "pct_y", "pct_w", "pct_h"};
	const char *px[] = {"px_x", "px_y", "px_w", "px_h"};

	for (const char *n : pct) {
		if (obs_property_t *p = obs_properties_get(props, n))
			obs_property_set_visible(p, !pixel);
	}
	for (const char *n : px) {
		if (obs_property_t *p = obs_properties_get(props, n))
			obs_property_set_visible(p, pixel);
	}
	if (obs_property_t *p = obs_properties_get(props, "roi_hint"))
		obs_property_set_visible(p, true);
	return true;
}

bool on_mask_kind_changed(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const int kind = (int)obs_data_get_int(settings, "mask_kind");
	if (obs_property_t *p = obs_properties_get(props, "blur_px"))
		obs_property_set_visible(p, kind == MASK_BLUR);
	if (obs_property_t *p = obs_properties_get(props, "block_px"))
		obs_property_set_visible(p, kind == MASK_MOSAIC);
	return true;
}

bool on_test_mask(obs_properties_t *, obs_property_t *, void *data)
{
	auto *f = static_cast<GuardFilter *>(data);
	f->test_until.store(os_gettime_ns() + 3000LL * 1000000LL);
	return false;
}

bool on_refresh_status(obs_properties_t *props, obs_property_t *, void *data)
{
	auto *f = static_cast<GuardFilter *>(data);
	const std::string s = f->pipeline ? f->pipeline->status() : std::string("-");
	if (obs_property_t *p = obs_properties_get(props, "status"))
		obs_property_set_description(p, s.c_str());
	return true;
}

// ---------------------------------------------------------------------------
// 属性面板
// ---------------------------------------------------------------------------

obs_properties_t *guard_properties(void *data, void *type_data)
{
	obs_properties_t *p = obs_properties_create();

	// type_data 在滤镜场景下就是 obs_source_t*（OBS 33 声明为 void*）
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
	obs_properties_add_text(p, "sec_basic", T_("Sec.Basic"), OBS_TEXT_INFO);

	obs_properties_add_bool(p, "enabled", T_("Enabled"));

	obs_property_t *fps = obs_properties_add_int_slider(p, "det_fps", T_("DetFps"), 1, 30, 1);
	obs_property_set_long_description(fps, T_("DetFps.Desc"));

	obs_property_t *fm = obs_properties_add_list(p, "fail_mode", T_("FailMode"),
						     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(fm, T_("FailOpen"), FAIL_OPEN);
	obs_property_list_add_int(fm, T_("FailClosed"), FAIL_CLOSED);
	obs_property_set_long_description(fm, T_("FailMode.Desc"));

	// ---------------- 检测区域 ----------------
	obs_properties_add_text(p, "sec_roi", T_("Sec.Roi"), OBS_TEXT_INFO);

	obs_property_t *cm = obs_properties_add_list(p, "coord_mode", T_("CoordMode"),
						     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(cm, T_("CoordPercent"), COORD_PERCENT);
	obs_property_list_add_int(cm, T_("CoordPixel"), COORD_PIXEL);
	obs_property_set_modified_callback(cm, on_coord_mode_changed);
	obs_property_set_long_description(cm, T_("CoordMode.Desc"));

	obs_property_t *hint =
		obs_properties_add_text(p, "roi_hint", T_("RoiHint"), OBS_TEXT_INFO);
	obs_property_set_visible(hint, true);

	obs_property_t *pp[4];
	pp[0] = obs_properties_add_float_slider(p, "pct_x", T_("RoiX"), 0.0, 100.0, 0.1);
	pp[1] = obs_properties_add_float_slider(p, "pct_y", T_("RoiY"), 0.0, 100.0, 0.1);
	pp[2] = obs_properties_add_float_slider(p, "pct_w", T_("RoiW"), 1.0, 100.0, 0.1);
	pp[3] = obs_properties_add_float_slider(p, "pct_h", T_("RoiH"), 1.0, 100.0, 0.1);
	for (auto *x : pp)
		obs_property_set_visible(x, !pixel_mode);
	obs_property_set_long_description(pp[0], T_("RoiPct.Desc"));

	obs_property_t *qx[4];
	qx[0] = obs_properties_add_int(p, "px_x", T_("RoiX"), 0, 8192, 1);
	qx[1] = obs_properties_add_int(p, "px_y", T_("RoiY"), 0, 8192, 1);
	qx[2] = obs_properties_add_int(p, "px_w", T_("RoiW"), 1, 8192, 1);
	qx[3] = obs_properties_add_int(p, "px_h", T_("RoiH"), 1, 8192, 1);
	for (auto *x : qx)
		obs_property_set_visible(x, pixel_mode);
	obs_property_set_long_description(qx[0], T_("RoiPx.Desc"));

	// ---------------- 模型 ----------------
	obs_properties_add_text(p, "sec_model", T_("Sec.Model"), OBS_TEXT_INFO);

	obs_property_t *mp = obs_properties_add_path(p, "model_path", T_("ModelPath"),
						     OBS_PATH_FILE, T_("ModelFilter"), nullptr);
	obs_property_set_long_description(mp, T_("ModelPath.Desc"));

	obs_property_t *dev = obs_properties_add_list(p, "device", T_("Device"),
						      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(dev, T_("DeviceAuto"), DEVICE_AUTO);
	obs_property_list_add_int(dev, T_("DeviceDml"), DEVICE_DML);
	obs_property_list_add_int(dev, T_("DeviceCpu"), DEVICE_CPU);

	obs_property_t *conf =
		obs_properties_add_float_slider(p, "conf", T_("Conf"), 0.01, 0.99, 0.01);
	obs_property_set_long_description(conf, T_("Conf.Desc"));

	obs_properties_add_float_slider(p, "iou", T_("Iou"), 0.10, 0.95, 0.01);
	obs_properties_add_int_slider(p, "max_det", T_("MaxDet"), 1, 64, 1);

	obs_property_t *cf =
		obs_properties_add_text(p, "class_filter", T_("ClassFilter"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(cf, T_("ClassFilter.Desc"));

	// ---------------- 遮罩样式 ----------------
	obs_properties_add_text(p, "sec_mask", T_("Sec.Mask"), OBS_TEXT_INFO);

	obs_property_t *mk = obs_properties_add_list(p, "mask_kind", T_("MaskKind"),
						     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mk, T_("MaskSolid"), MASK_SOLID);
	obs_property_list_add_int(mk, T_("MaskBlur"), MASK_BLUR);
	obs_property_list_add_int(mk, T_("MaskMosaic"), MASK_MOSAIC);
	obs_property_set_modified_callback(mk, on_mask_kind_changed);
	obs_property_set_long_description(mk, T_("MaskKind.Desc"));

	obs_properties_add_color(p, "mask_color", T_("MaskColor"));
	obs_properties_add_float_slider(p, "mask_opacity", T_("MaskOpacity"), 0.05, 1.0, 0.01);

	obs_property_t *blur =
		obs_properties_add_float_slider(p, "blur_px", T_("BlurPx"), 2.0, 64.0, 1.0);
	obs_property_set_visible(blur, mask_kind == MASK_BLUR);

	obs_property_t *blk =
		obs_properties_add_float_slider(p, "block_px", T_("BlockPx"), 4.0, 64.0, 1.0);
	obs_property_set_visible(blk, mask_kind == MASK_MOSAIC);

	obs_properties_add_float_slider(p, "expand", T_("Expand"), 0.0, 0.30, 0.01);
	obs_properties_add_float_slider(p, "pad_px", T_("PadPx"), 0.0, 24.0, 1.0);

	// ---------------- 平滑 ----------------
	obs_properties_add_text(p, "sec_smooth", T_("Sec.Smooth"), OBS_TEXT_INFO);
	obs_properties_add_int_slider(p, "hold_ms", T_("HoldMs"), 0, 1000, 10);
	obs_properties_add_int_slider(p, "fade_ms", T_("FadeMs"), 0, 800, 10);

	// ---------------- 调试 ----------------
	obs_properties_add_text(p, "sec_debug", T_("Sec.Debug"), OBS_TEXT_INFO);
	obs_properties_add_bool(p, "debug_roi", T_("DebugRoi"));
	obs_properties_add_bool(p, "debug_box", T_("DebugBox"));

	obs_property_t *st =
		obs_properties_add_text(p, "status", T_("Status"), OBS_TEXT_INFO);
	obs_property_set_description(st, status.c_str());

	// button2 的最后一个参数会原样传给回调，这里传滤镜实例
	obs_properties_add_button2(p, "refresh", T_("Refresh"), on_refresh_status, data);
	obs_properties_add_button2(p, "test_mask", T_("TestMask"), on_test_mask, data);
	obs_properties_add_text(p, "test_hint", T_("TestMask.Desc"), OBS_TEXT_INFO);

	return p;
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

void *guard_create(obs_data_t *settings, obs_source_t *source)
{
	auto *f = new GuardFilter();
	f->context = source;
	f->start_ns = os_gettime_ns();

	config_defaults(settings);
	Config initial;
	config_load(initial, settings);
	f->cfg = std::make_shared<const Config>(initial);

	f->pipeline = std::make_unique<Pipeline>();
	f->pipeline->set_config(initial);
	f->pipeline->start();

	obs_enter_graphics();
	std::string err;
	if (!f->renderer.init(err))
		blog(LOG_ERROR, "[obs-guard] 遮罩特效初始化失败: %s", err.c_str());
	obs_leave_graphics();

	blog(LOG_INFO, "[obs-guard] 滤镜已创建 (源: %s)",
	     obs_source_get_name(source) ? obs_source_get_name(source) : "?");
	return f;
}

void guard_destroy(void *data)
{
	auto *f = static_cast<GuardFilter *>(data);
	if (!f)
		return;

	if (f->pipeline) {
		f->pipeline->stop();
		f->pipeline.reset();
	}

	obs_enter_graphics();
	f->renderer.destroy();
	f->grabber.release();
	obs_leave_graphics();

	delete f;
}

void guard_update(void *data, obs_data_t *settings)
{
	auto *f = static_cast<GuardFilter *>(data);
	Config next;
	config_load(next, settings);
	std::atomic_store(&f->cfg, std::make_shared<const Config>(next));
	if (f->pipeline)
		f->pipeline->set_config(next);
}

void guard_get_defaults(obs_data_t *settings)
{
	config_defaults(settings);
}

void guard_activate(void *data)
{
	auto *f = static_cast<GuardFilter *>(data);
	if (f && f->pipeline)
		f->pipeline->start();
}

void guard_deactivate(void *data)
{
	// 滤波链断开时清空遮罩，避免恢复时闪现旧结果
	auto *f = static_cast<GuardFilter *>(data);
	if (f) {
		f->mask.count = 0;
		f->mask.panic = false;
	}
}

// ---------------------------------------------------------------------------
// 渲染
// ---------------------------------------------------------------------------

void guard_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);

	auto *f = static_cast<GuardFilter *>(data);
	if (!f || !f->context)
		return;

	const std::shared_ptr<const Config> cfg = std::atomic_load(&f->cfg);
	if (!cfg)
		return;

	obs_source_t *target = obs_filter_get_target(f->context);
	if (!target)
		return;

	f->width = obs_source_get_base_width(target);
	f->height = obs_source_get_base_height(target);
	if (!f->width || !f->height)
		return;

	const int64_t now = os_gettime_ns();
	const RoiRect roi = cfg->roi_rect(f->width, f->height);

	// ---- 取帧节拍：只有到点才做回读，且绝不等推理 ----
	if (cfg->enabled && roi.valid() && !f->in_grab && f->pipeline->ready()) {
		const int fps = cfg->det_fps > 0 ? cfg->det_fps : 10;
		const int64_t interval = 1000000000LL / fps;
		if (now - f->last_grab_ns >= interval) {
			f->last_grab_ns = now;

			const int mw = f->pipeline->model_width();
			const int mh = f->pipeline->model_height();

			FrameView fv;
			f->in_grab = true;
			const bool ok = f->grabber.grab(target, f->width, f->height, roi, mw, mh, fv);
			f->in_grab = false;

			if (ok) {
				fv.pts_ns = now;
				fv.src_w = (int)f->width;
				fv.src_h = (int)f->height;
				fv.frame_id = (uint64_t)++f->next_frame_id;
				f->pipeline->submit(std::move(fv));
			}
		}
	}

	// ---- 取结果：try_lock，拿不到就用上一帧，渲染线程零等待 ----
	if (cfg->enabled) {
		MaskFrame fresh;
		if (f->pipeline->fetch(fresh))
			f->mask = fresh;
	} else {
		f->mask.count = 0;
		f->mask.panic = false;
	}

	const bool test = f->test_until.load() > now;

	// fail-closed：推理长期不可用时全屏遮挡（带 3s 启动宽限，避免开播闪黑）
	MaskFrame out = f->mask;
	if (cfg->enabled && cfg->fail_mode == FAIL_CLOSED && out.count == 0 &&
	    now - f->start_ns > kFailClosedGraceNs) {
		if (!f->pipeline->ready() || f->pipeline->failed()) {
			out.count = 1;
			out.items[0] = {};
			out.items[0].x = 0.0f;
			out.items[0].y = 0.0f;
			out.items[0].w = 1.0f;
			out.items[0].h = 1.0f;
			out.items[0].state = 1;
			out.items[0].first_seen = now - 1000000;
			out.items[0].last_seen = now;
		}
	}

	f->renderer.draw(f->context, f->width, f->height, *cfg, out, roi, now, test);
}

obs_source_info make_info()
{
	obs_source_info info = {};
	info.id = "guard_mask_filter";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = [](void *) -> const char * { return T_("Name"); };
	info.create = guard_create;
	info.destroy = guard_destroy;
	info.update = guard_update;
	info.get_defaults = guard_get_defaults;
	info.get_properties2 = guard_properties;
	info.video_render = guard_render;
	info.activate = guard_activate;
	info.deactivate = guard_deactivate;
	return info;
}

} // namespace

void register_guard_filter()
{
	static obs_source_info info = make_info();
	obs_register_source_s(&info, sizeof(info));
}

} // namespace guard
