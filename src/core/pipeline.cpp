#include "core/pipeline.h"

#include <util/platform.h>

#include <algorithm>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace guard {
namespace {

// 推理调用来自第三方运行库与 GPU 驱动，越界访问属于 SEH 异常，C++ 的
// try/catch 捕获不到。这里做一层兜底：异常只让检测器失效，不上抛到 OBS。
// 包装函数内不持有需要析构的对象，否则 MSVC 会报 C2712。
#ifdef _WIN32
int seh_load(IDetector *det, const DetectorConfig &cfg, std::string &err)
{
	__try {
		return det->load(cfg, err) ? 0 : 1;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return -1;
	}
}

int seh_run(IDetector *det, const FrameView &frame, std::vector<Detection> &out)
{
	__try {
		return det->run(frame, out) ? 0 : 1;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return -1;
	}
}
#else
int seh_load(IDetector *det, const DetectorConfig &cfg, std::string &err)
{
	return det->load(cfg, err) ? 0 : 1;
}

int seh_run(IDetector *det, const FrameView &frame, std::vector<Detection> &out)
{
	return det->run(frame, out) ? 0 : 1;
}
#endif

} // namespace

Pipeline::~Pipeline()
{
	stop();
}

void Pipeline::start()
{
	if (thread_.joinable())
		return;
	quit_ = false;
	thread_ = std::thread([this] { worker_main(); });
}

void Pipeline::stop()
{
	{
		std::lock_guard<std::mutex> lk(mutex_);
		quit_ = true;
		has_pending_ = false;
	}
	cv_.notify_all();
	if (thread_.joinable())
		thread_.join();

	detector_.reset();
	tracker_.reset();
}

void Pipeline::set_config(const Config &cfg)
{
	{
		std::lock_guard<std::mutex> lk(mutex_);
		if (config_needs_reload(last_req_, cfg)) {
			last_req_ = cfg;
			reload_ = true;
		}
		cfg_ = cfg;
	}
	cv_.notify_one();
}

void Pipeline::submit(FrameView &&frame)
{
	{
		std::lock_guard<std::mutex> lk(mutex_);
		pending_ = std::move(frame);
		pending_.frame_id = ++frame_id_;
		has_pending_ = true;
	}
	cv_.notify_one();
}

bool Pipeline::fetch(MaskFrame &out)
{
	if (!mutex_.try_lock())
		return false;
	out = shared_;
	mutex_.unlock();
	return true;
}

void Pipeline::publish(const MaskFrame &frame)
{
	if (!mutex_.try_lock())
		return; // 渲染线程正在取，丢弃本次更新即可，下一帧再来
	shared_ = frame;
	mutex_.unlock();
}

std::string Pipeline::status() const
{
	std::lock_guard<std::mutex> lk(status_mutex_);
	return status_;
}

void Pipeline::worker_main()
{
	for (;;) {
		FrameView frame;
		Config cfg;
		bool do_reload = false;

		{
			std::unique_lock<std::mutex> lk(mutex_);
			cv_.wait(lk, [this] { return quit_ || has_pending_ || reload_; });
			if (quit_)
				break;

			cfg = cfg_;
			do_reload = reload_;
			reload_ = false;

			if (has_pending_) {
				frame = std::move(pending_);
				pending_ = FrameView{};
				has_pending_ = false;
			}
		}

		// ---- 模型（重）加载 ----
		if (do_reload) {
			detector_.reset();
			tracker_.reset();
			model_ready_.store(false);
			model_failed_.store(false);

			if (cfg.model_path.empty()) {
				std::lock_guard<std::mutex> lk(status_mutex_);
				status_ = "未配置模型（仅可预览遮罩样式）";
			} else if (!detector_available()) {
				model_failed_.store(true);
				std::lock_guard<std::mutex> lk(status_mutex_);
				status_ = "构建时未启用推理后端";
			} else {
				std::string err;
				if (!preload_runtime(err)) {
					model_failed_.store(true);
					{
						std::lock_guard<std::mutex> lk(status_mutex_);
						status_ = err;
					}
					blog(LOG_ERROR, "[obs-guard] %s", err.c_str());
				} else {
					DetectorConfig dc;
					dc.path = cfg.model_path;
					dc.device = cfg.device;
					dc.conf = cfg.conf;
					dc.iou = cfg.iou;
					dc.max_det = cfg.max_det;
					dc.class_filter = cfg.class_filter;

					auto det = create_detector();
					const int load_rc = det ? seh_load(det.get(), dc, err) : 1;
					if (load_rc == 0) {
						model_w_.store(det->input_width());
						model_h_.store(det->input_height());
						model_ready_.store(true);
						{
							std::lock_guard<std::mutex> lk(status_mutex_);
							status_ = det->status();
						}
						detector_ = std::move(det);
					} else {
						model_failed_.store(true);
						if (load_rc < 0)
							err = "推理运行时异常（访问违例），已停用检测";
						{
							std::lock_guard<std::mutex> lk(status_mutex_);
							status_ = "模型加载失败: " + err;
						}
						blog(LOG_ERROR, "[obs-guard] 模型加载失败: %s", err.c_str());
					}
				}
			}
		}

		if (frame.width == 0 || frame.height == 0)
			continue;

		if (!detector_)
			continue;

		// ---- 推理 + 跟踪 ----
		std::vector<Detection> dets;
		const int64_t t0 = os_gettime_ns();
		const int run_rc = seh_run(detector_.get(), frame, dets);
		const int64_t t1 = os_gettime_ns();

		if (run_rc < 0) {
			// 访问违例：停用检测器，避免每帧重复触发把 OBS 拖垮
			blog(LOG_ERROR, "[obs-guard] 推理发生访问违例，已停用检测（重启滤镜可重试）");
			detector_.reset();
			model_ready_.store(false);
			model_failed_.store(true);
			{
				std::lock_guard<std::mutex> lk(status_mutex_);
				status_ = "推理运行时异常，已停用检测";
			}
			continue;
		}

		const bool ok = (run_rc == 0);

		TrackerParams tp;
		tp.expand = cfg.expand;
		tp.hold_ms = cfg.hold_ms;
		tp.fade_ms = cfg.fade_ms;
		tp.max_masks = cfg.mask_limit;

		MaskFrame mf;
		tracker_.update(ok ? dets : std::vector<Detection>{}, frame.roi, t1, tp, mf);
		mf.stamp = t1;
		publish(mf);

		// 命中/消失只在跃迁时记一条 INFO，正常日志级别下也能看到，
		// 避免为了排障去开 Debug 级别而被刷屏。
		if ((mf.count > 0) != (last_pub_count_ > 0)) {
			if (mf.count > 0) {
				float best = 0.0f;
				for (int i = 0; i < mf.count; ++i)
					best = std::max(best, mf.items[i].score);
				blog(LOG_INFO, "[obs-guard] 命中 %d 个目标（最高置信度 %.2f）",
				     mf.count, best);
			} else {
				blog(LOG_INFO, "[obs-guard] 目标已消失，遮罩淡出");
			}
			last_pub_count_ = mf.count;
		}

		if (++dbg_counter_ % 100 == 0) {
			blog(LOG_DEBUG, "[obs-guard] 推理 %.2fms, %zu 命中, %d 遮罩",
			     (t1 - t0) / 1000000.0, dets.size(), mf.count);
		}
	}
}

} // namespace guard
