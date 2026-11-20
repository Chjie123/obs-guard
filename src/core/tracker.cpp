#include "core/tracker.h"

#include <algorithm>
#include <cmath>

namespace guard {
namespace {

float iou_xywh(float ax, float ay, float aw, float ah, float bx, float by, float bw, float bh)
{
	const float ix1 = std::max(ax, bx);
	const float iy1 = std::max(ay, by);
	const float ix2 = std::min(ax + aw, bx + bw);
	const float iy2 = std::min(ay + ah, by + bh);
	const float iw = std::max(0.0f, ix2 - ix1);
	const float ih = std::max(0.0f, iy2 - iy1);
	const float inter = iw * ih;
	const float uni = aw * ah + bw * bh - inter;
	return uni > 1e-6f ? inter / uni : 0.0f;
}

float clamp01(float v)
{
	return std::clamp(v, 0.0f, 1.0f);
}

} // namespace

void Tracker::reset()
{
	tracks_.clear();
	next_id_ = 1;
}

void Tracker::update(const std::vector<Detection> &dets, const RoiRect &roi, int64_t now,
		     const TrackerParams &p, MaskFrame &out)
{
	out.count = 0;
	out.stamp = now;

	// ROI 内归一化 → 帧归一化
	auto to_frame = [&](float u, float v, float w, float h, float &ox, float &oy, float &ow,
			    float &oh) {
		ox = roi.x + u * roi.w;
		oy = roi.y + v * roi.h;
		ow = w * roi.w;
		oh = h * roi.h;
	};

	// --- 关联：同类别、IoU 贪心 ---
	std::vector<int> matched(tracks_.size(), -1);
	std::vector<bool> used(dets.size(), false);

	for (size_t t = 0; t < tracks_.size(); ++t) {
		float best_iou = 0.30f;
		int best_i = -1;
		for (size_t i = 0; i < dets.size(); ++i) {
			if (used[i] || dets[i].class_id != tracks_[t].cls)
				continue;
			float fx, fy, fw, fh;
			to_frame(dets[i].x, dets[i].y, dets[i].w, dets[i].h, fx, fy, fw, fh);
			const float v = iou_xywh(tracks_[t].x, tracks_[t].y, tracks_[t].w,
						 tracks_[t].h, fx, fy, fw, fh);
			if (v > best_iou) {
				best_iou = v;
				best_i = (int)i;
			}
		}
		if (best_i >= 0) {
			matched[t] = best_i;
			used[(size_t)best_i] = true;
		}
	}

	// --- 更新命中轨迹：EMA 平滑框体 ---
	for (size_t t = 0; t < tracks_.size(); ++t) {
		if (matched[t] < 0)
			continue;
		const Detection &d = dets[(size_t)matched[t]];
		float fx, fy, fw, fh;
		to_frame(d.x, d.y, d.w, d.h, fx, fy, fw, fh);

		const float k = 0.45f; // 新观测权重
		tracks_[t].x = tracks_[t].x * (1.0f - k) + fx * k;
		tracks_[t].y = tracks_[t].y * (1.0f - k) + fy * k;
		tracks_[t].w = tracks_[t].w * (1.0f - k) + fw * k;
		tracks_[t].h = tracks_[t].h * (1.0f - k) + fh * k;
		tracks_[t].score = std::max(tracks_[t].score * 0.8f, d.score);
		tracks_[t].hits += 1;
		tracks_[t].last_ns = now;
		if (tracks_[t].hits >= p.confirm_hits)
			tracks_[t].confirmed = true;
	}

	// --- 新轨迹（SUSPECT 立即出遮罩）---
	for (size_t i = 0; i < dets.size(); ++i) {
		if (used[i])
			continue;
		TrackState ts;
		ts.id = next_id_++;
		ts.cls = dets[i].class_id;
		to_frame(dets[i].x, dets[i].y, dets[i].w, dets[i].h, ts.x, ts.y, ts.w, ts.h);
		ts.hits = 1;
		ts.score = dets[i].score;
		ts.first_ns = now;
		ts.last_ns = now;
		ts.confirmed = false;
		tracks_.push_back(ts);
	}

	// --- 淘汰：超过 hold + fade 未再出现 ---
	const int64_t ttl = (int64_t)(p.hold_ms + p.fade_ms + 300) * 1000000LL;
	tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
				     [&](const TrackState &t) {
					     return now - t.last_ns > ttl;
				     }),
		      tracks_.end());

	// --- 发布：确认的优先，其次按置信度 ---
	std::vector<const TrackState *> order;
	order.reserve(tracks_.size());
	for (const auto &t : tracks_)
		order.push_back(&t);

	std::stable_sort(order.begin(), order.end(), [](const TrackState *a, const TrackState *b) {
		if (a->confirmed != b->confirmed)
			return a->confirmed;
		return a->score > b->score;
	});

	const float ex = p.expand;
	const int limit = std::clamp(p.max_masks, 1, MaskFrame::kMax);
	for (const auto *t : order) {
		if (out.count >= limit)
			break;

		float w = t->w * (1.0f + ex * 2.0f);
		float h = t->h * (1.0f + ex * 2.0f);
		float x = t->x - (w - t->w) * 0.5f;
		float y = t->y - (h - t->h) * 0.5f;

		// 在帧坐标系收口；允许略微越界以覆盖 ROI 边缘外的目标
		x = clamp01(x);
		y = clamp01(y);
		w = std::min(w, 1.0f - x);
		h = std::min(h, 1.0f - y);
		if (w < 0.003f || h < 0.003f)
			continue;

		MaskCmd &c = out.items[out.count++];
		c.x = x;
		c.y = y;
		c.w = w;
		c.h = h;
		c.state = t->confirmed ? 1 : 0;
		c.score = t->score;
		c.first_seen = t->first_ns;
		c.last_seen = t->last_ns;
	}
}

} // namespace guard
