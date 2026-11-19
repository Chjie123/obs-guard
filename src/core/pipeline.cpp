#include "core/pipeline.h"

#include <util/platform.h>

#include <sstream>

namespace guard {

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
					if (det && det->load(dc, err)) {
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
		const bool ok = detector_->run(frame, dets);
		const int64_t t1 = os_gettime_ns();

		TrackerParams tp;
		tp.expand = cfg.expand;
		tp.hold_ms = cfg.hold_ms;
		tp.fade_ms = cfg.fade_ms;

		MaskFrame mf;
		tracker_.update(ok ? dets : std::vector<Detection>{}, frame.roi, t1, tp, mf);
		mf.stamp = t1;
		publish(mf);

		static int dbg = 0;
		if (++dbg % 100 == 0) {
			blog(LOG_DEBUG, "[obs-guard] 推理 %.2fms, %zu 命中, %d 遮罩",
			     (t1 - t0) / 1000000.0, dets.size(), mf.count);
		}
	}
}

} // namespace guard
