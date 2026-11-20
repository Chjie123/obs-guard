// detect/yolo_post.cpp -- 输出布局识别、解码、NMS、坐标回映射
#include "detect/yolo_post.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace guard {
namespace {

struct RawBox {
	float x1, y1, x2, y2;
	float score;
	int cls;
};

float iou_of(const RawBox &a, const RawBox &b)
{
	const float ix1 = std::max(a.x1, b.x1), iy1 = std::max(a.y1, b.y1);
	const float ix2 = std::min(a.x2, b.x2), iy2 = std::min(a.y2, b.y2);
	const float iw = std::max(0.0f, ix2 - ix1), ih = std::max(0.0f, iy2 - iy1);
	const float inter = iw * ih;
	const float uni = (a.x2 - a.x1) * (a.y2 - a.y1) + (b.x2 - b.x1) * (b.y2 - b.y1) - inter;
	return uni > 1e-6f ? inter / uni : 0.0f;
}

bool class_allowed(const YoloPostParams &p, int cls)
{
	return !p.allowed || p.allowed->find(cls) != p.allowed->end();
}

// 类内贪心 NMS
void suppress(std::vector<RawBox> &cands, const YoloPostParams &p, std::vector<RawBox> &keep)
{
	std::sort(cands.begin(), cands.end(),
		  [](const RawBox &a, const RawBox &b) { return a.score > b.score; });

	keep.reserve(std::min<size_t>(cands.size(), (size_t)p.max_det));
	for (const auto &cand : cands) {
		bool dup = false;
		for (const auto &k : keep) {
			if (k.cls == cand.cls && iou_of(k, cand) > p.iou) {
				dup = true;
				break;
			}
		}
		if (dup)
			continue;
		keep.push_back(cand);
		if ((int)keep.size() >= p.max_det)
			break;
	}
}

} // namespace

void parse_class_names(const std::string &src, std::vector<std::string> &out)
{
	size_t i = 0;
	while (i < src.size()) {
		size_t colon = src.find(':', i);
		if (colon == std::string::npos)
			break;

		size_t d = colon;
		while (d > i && std::isdigit((unsigned char)src[d - 1]))
			--d;

		size_t q = colon + 1;
		while (q < src.size() && (src[q] == ' ' || src[q] == '\t'))
			++q;
		if (q >= src.size() || (src[q] != '\'' && src[q] != '"')) {
			i = colon + 1;
			continue;
		}

		const char quote = src[q++];
		const size_t end = src.find(quote, q);
		if (end == std::string::npos)
			break;

		int idx = 0;
		if (d < colon)
			idx = std::atoi(src.substr(d, colon - d).c_str());
		if (idx >= 0) {
			if ((int)out.size() <= idx)
				out.resize((size_t)idx + 1);
			out[(size_t)idx] = src.substr(q, end - q);
		}
		i = end + 1;
	}
}

bool decode_yolo_output(const float *data, const std::vector<int64_t> &dims,
			const YoloPostParams &p, std::vector<Detection> &out)
{
	out.clear();
	if (!data || dims.size() != 3)
		return false;

	const int64_t d1 = dims[1], d2 = dims[2];
	const bool transposed = d1 > d2; // [1, N, C]
	const int64_t n = transposed ? d1 : d2;
	const int64_t c = transposed ? d2 : d1;

	// c == 6 存在二义性：既可能是「4 个框参数 + 2 个类别」的密集输出，
	// 也可能是已内置 NMS 的 [x1, y1, x2, y2, score, cls]。旧实现无条件按
	// 后者解码，导致 2 类模型每一行都被读成 (score, cls)，整批候选被置信度
	// 过滤干净——表现为「模型加载成功，但画面永远不被处理」。
	// 判定：类别数已知时看 c 是否等于 4 + nc；未知时按候选行数区分
	// （密集头有数千个 anchor，NMS 输出通常只剩几百行）。
	bool nms_layout = false;
	if (c == 6) {
		if (p.num_classes > 0)
			nms_layout = (c != 4 + (int64_t)p.num_classes);
		else
			nms_layout = (n <= 1000);
	}

	std::vector<RawBox> cands;

	if (nms_layout) {
		// 已内置 NMS：x1, y1, x2, y2, score, cls
		for (int64_t i = 0; i < n; ++i) {
			float v[6];
			for (int k = 0; k < 6; ++k)
				v[k] = transposed ? data[i * c + k] : data[k * n + i];
			if (v[4] < p.conf)
				continue;
			const int cls = (int)v[5];
			if (!class_allowed(p, cls))
				continue;
			cands.push_back(RawBox{v[0], v[1], v[2], v[3], v[4], cls});
		}
	} else if (c >= 5) {
		// 通道优先：cx, cy, w, h, cls0..clsN
		const int64_t nc = c - 4;
		for (int64_t i = 0; i < n; ++i) {
			auto at = [&](int64_t k) -> float {
				return transposed ? data[i * c + k] : data[k * n + i];
			};

			float best = 0.0f;
			int best_cls = -1;
			for (int64_t k = 0; k < nc; ++k) {
				const float s = at(4 + k);
				if (s > best) {
					best = s;
					best_cls = (int)k;
				}
			}
			if (best_cls < 0 || best < p.conf || !class_allowed(p, best_cls))
				continue;

			const float cx = at(0), cy = at(1), w = at(2), h = at(3);
			cands.push_back(
				RawBox{cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f,
				       best, best_cls});
		}
	} else {
		return false;
	}

	if (cands.empty())
		return true;

	std::vector<RawBox> keep;
	suppress(cands, p, keep);

	// 模型像素 -> 去 letterbox -> 检测区域归一化
	const float sx = std::max(1e-3f, 1.0f - 2.0f * p.pad_x);
	const float sy = std::max(1e-3f, 1.0f - 2.0f * p.pad_y);
	const float mw = (float)p.input_w, mh = (float)p.input_h;

	for (const auto &k : keep) {
		float u1 = (k.x1 / mw - p.pad_x) / sx;
		float v1 = (k.y1 / mh - p.pad_y) / sy;
		float u2 = (k.x2 / mw - p.pad_x) / sx;
		float v2 = (k.y2 / mh - p.pad_y) / sy;

		u1 = std::clamp(u1, -0.02f, 1.02f);
		v1 = std::clamp(v1, -0.02f, 1.02f);
		u2 = std::clamp(u2, -0.02f, 1.02f);
		v2 = std::clamp(v2, -0.02f, 1.02f);

		Detection d;
		d.class_id = k.cls;
		d.score = k.score;
		d.x = u1;
		d.y = v1;
		d.w = std::max(0.0f, u2 - u1);
		d.h = std::max(0.0f, v2 - v1);
		if (d.w < 0.002f || d.h < 0.002f)
			continue;
		out.push_back(d);
	}

	return true;
}

} // namespace guard
