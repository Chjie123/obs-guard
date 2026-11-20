// detect/yolo_post.h -- YOLO 输出后处理（不依赖推理框架，可单独测试）
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/types.h"

namespace guard {

struct YoloPostParams {
	float conf = 0.35f;
	float iou = 0.45f;
	int max_det = 32;

	// 模型类别数；0 表示未知。用于消解 c == 6 的布局二义性。
	int num_classes = 0;

	// 模型输入尺寸与 letterbox 填充比例
	int input_w = 640;
	int input_h = 384;
	float pad_x = 0.0f;
	float pad_y = 0.0f;

	// 允许的类别；nullptr 表示不过滤
	const std::map<int, bool> *allowed = nullptr;
};

// 解析 ONNX 元数据中的 names 字段：{0: 'person', 1: 'bicycle'}
void parse_class_names(const std::string &src, std::vector<std::string> &out);

// 解码模型输出，结果归一化到检测区域内部（0..1）
// 支持的布局：[1, 4+nc, N]、[1, N, 4+nc]、[1, N, 6]（已内置 NMS）
bool decode_yolo_output(const float *data, const std::vector<int64_t> &dims,
			const YoloPostParams &params, std::vector<Detection> &out);

} // namespace guard
