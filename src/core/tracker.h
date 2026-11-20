// core/tracker.h -- 跨帧关联 + 状态机
// 规划 §4.2 / §4.3：
//   只增不减：确认后的遮挡不因单帧漏检撤销，只走 HOLD -> 淡出
//   SUSPECT 阶段就出遮罩（轻度），不等置信度攒够
#pragma once

#include <cstdint>
#include <vector>

#include "core/types.h"

namespace guard {

struct TrackerParams {
	float expand = 0.10f;
	int hold_ms = 300;
	int fade_ms = 180;
	int confirm_hits = 3;
};

struct TrackState {
	uint32_t id = 0;
	int cls = 0;
	float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f; // 帧归一化
	int hits = 0;
	float score = 0.0f;
	int64_t first_ns = 0;
	int64_t last_ns = 0;
	bool confirmed = false;
};

class Tracker {
public:
	void reset();

	// dets: ROI 归一化框；roi: 本帧 ROI；now: 单调时钟 ns
	void update(const std::vector<Detection> &dets, const RoiRect &roi, int64_t now,
		    const TrackerParams &p, MaskFrame &out);

private:
	std::vector<TrackState> tracks_;
	uint32_t next_id_ = 1;
};

} // namespace guard
