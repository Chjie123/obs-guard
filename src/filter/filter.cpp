// filter/filter.cpp -- 滤镜实体：生命周期、渲染入口、注册
//
// 挂载方式：
//   源级   -- 直接给「视频采集设备 / 显示器采集」添加滤镜
//   场景级 -- 新建审核场景，将原场景作为场景源加入，再对该场景源添加滤镜
//
// 每帧时序：
//   [检测节拍到] 取区域 -> 投递推理线程（不等待）
//   [每帧]       try_lock 取最新遮罩（取不到沿用上一帧）-> 合成
#include "filter/filter.h"

#include "filter/filter_internal.h"

#include <obs-module.h>
#include <util/platform.h>

#include <memory>
#include <string>

#include "core/constants.h"

namespace guard {
namespace {

// ---------------------------------------------------------------------------
// 单帧流程
// ---------------------------------------------------------------------------

// 取帧：按检测频率限流，投递后立即返回，绝不等待推理
void pump_frame(GuardFilter *f, obs_source_t *target, const Config &cfg, const RoiRect &roi,
		int64_t now)
{
	// 检测频率为 0 表示只保留已有遮罩的淡出，不再取帧推理
	if (!cfg.enabled || cfg.det_fps <= 0 || !roi.valid() || f->in_grab ||
	    !f->pipeline->ready())
		return;

	const int64_t interval = 1000000000LL / cfg.det_fps;
	if (now - f->last_grab_ns < interval)
		return;
	f->last_grab_ns = now;

	FrameView frame;
	f->in_grab = true;
	const bool ok = f->grabber.grab(target, f->width, f->height, roi,
					f->pipeline->model_width(), f->pipeline->model_height(),
					frame);
	f->in_grab = false;
	if (!ok)
		return;

	frame.pts_ns = now;
	frame.src_w = (int)f->width;
	frame.src_h = (int)f->height;
	frame.frame_id = (uint64_t)++f->next_frame_id;
	f->pipeline->submit(std::move(frame));
}

// 取结果：try_lock 失败即沿用上一帧，渲染线程零等待
void collect_mask(GuardFilter *f, const Config &cfg)
{
	if (!cfg.enabled) {
		f->mask.count = 0;
		return;
	}
	MaskFrame fresh;
	if (f->pipeline->fetch(fresh))
		f->mask = fresh;
}

// 失败策略：fail-closed 时在推理不可用期间输出全屏遮罩
MaskFrame apply_fail_policy(const GuardFilter *f, const Config &cfg, int64_t now)
{
	MaskFrame out = f->mask;

	if (!cfg.enabled || cfg.fail_mode != FAIL_CLOSED || out.count > 0)
		return out;
	if (now - f->start_ns <= kFailClosedGraceNs)
		return out;
	if (f->pipeline->ready() && !f->pipeline->failed())
		return out;

	out.count = 1;
	out.items[0] = {};
	out.items[0].w = 1.0f;
	out.items[0].h = 1.0f;
	out.items[0].state = 1;
	out.items[0].first_seen = now - kFadeInNs;
	out.items[0].last_seen = now;
	return out;
}

// ---------------------------------------------------------------------------
// 生命周期回调
// ---------------------------------------------------------------------------

void *on_create(obs_data_t *settings, obs_source_t *source)
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

	const char *name = obs_source_get_name(source);
	blog(LOG_INFO, "[obs-guard] 滤镜已创建 (源: %s)", name ? name : "?");
	return f;
}

void on_destroy(void *data)
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

void on_update(void *data, obs_data_t *settings)
{
	auto *f = static_cast<GuardFilter *>(data);
	Config next;
	config_load(next, settings);
	std::atomic_store(&f->cfg, std::make_shared<const Config>(next));
	if (f->pipeline)
		f->pipeline->set_config(next);
}

void on_defaults(obs_data_t *settings)
{
	config_defaults(settings);
}

void on_activate(void *data)
{
	auto *f = static_cast<GuardFilter *>(data);
	if (f && f->pipeline)
		f->pipeline->start();
}

void on_deactivate(void *data)
{
	// 滤镜链断开时清空遮罩，避免恢复时闪现旧结果
	auto *f = static_cast<GuardFilter *>(data);
	if (f)
		f->mask.count = 0;
}

// ---------------------------------------------------------------------------
// 渲染
// ---------------------------------------------------------------------------

void on_render(void *data, gs_effect_t *effect)
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

	pump_frame(f, target, *cfg, roi, now);
	collect_mask(f, *cfg);

	const MaskFrame out = apply_fail_policy(f, *cfg, now);
	const bool test = f->test_until.load() > now;

	f->renderer.draw(f->context, f->width, f->height, *cfg, out, roi, now, test);
}

obs_source_info make_info()
{
	obs_source_info info = {};
	info.id = "guard_mask_filter";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = [](void *) -> const char * { return tr("Name"); };
	info.create = on_create;
	info.destroy = on_destroy;
	info.update = on_update;
	info.get_defaults = on_defaults;
	info.get_properties2 = build_properties;
	info.video_render = on_render;
	info.activate = on_activate;
	info.deactivate = on_deactivate;
	return info;
}

} // namespace

void register_guard_filter()
{
	static obs_source_info info = make_info();
	obs_register_source_s(&info, sizeof(info));
}

} // namespace guard
