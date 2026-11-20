// core/pipeline.h -- 推理线程 + 帧邮箱 + 结果发布
// 关键约束（规划 §2.3 / §8.2）：
//   - 渲染线程绝不等待推理：投递不加锁阻塞，取结果用 try_lock，失败就复用上一帧
//   - 只保最新帧：渲染快于推理时丢旧帧，延迟不累积
#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/config.h"
#include "detect/detector.h"
#include "core/tracker.h"
#include "core/types.h"

namespace guard {

class Pipeline {
public:
	Pipeline() = default;
	~Pipeline();

	void start();
	void stop();

	// 渲染线程调用：非阻塞
	void submit(FrameView &&frame);
	bool fetch(MaskFrame &out); // try_lock，失败返回 false（保留上一帧）

	// 任意线程调用：更新配置；影响模型的字段变化会触发推理会话重建
	void set_config(const Config &cfg);

	int model_width() const { return model_w_.load(std::memory_order_relaxed); }
	int model_height() const { return model_h_.load(std::memory_order_relaxed); }
	bool ready() const { return model_ready_.load(std::memory_order_relaxed); }
	bool failed() const { return model_failed_.load(std::memory_order_relaxed); }
	std::string status() const;

private:
	void worker_main();
	void publish(const MaskFrame &frame);

	mutable std::mutex mutex_;
	std::condition_variable cv_;
	std::thread thread_;
	bool quit_ = false;

	// 待推理帧（单槽）
	FrameView pending_;
	bool has_pending_ = false;
	uint64_t frame_id_ = 0;

	// 配置
	Config cfg_;
	Config last_req_;
	bool reload_ = false;

	// 已发布结果
	MaskFrame shared_;

	// 线程内独占
	std::unique_ptr<IDetector> detector_;
	Tracker tracker_;
	int dbg_counter_ = 0;    // 仅推理线程访问
	int last_pub_count_ = 0; // 仅推理线程访问，用于命中/消失的状态跃迁日志

	std::atomic<int> model_w_{640};
	std::atomic<int> model_h_{384};
	std::atomic<bool> model_ready_{false};
	std::atomic<bool> model_failed_{false};

	mutable std::mutex status_mutex_;
	std::string status_ = "未初始化";
};

} // namespace guard
