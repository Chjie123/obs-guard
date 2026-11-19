// guard-types.h -- 模块间唯一的数据契约
// 对应《项目规划 v0.2》附录 A：FrameRef / Detection / Track / MaskCmd
// 规则：模块之间只通过本文件的结构体通信，不做跨模块直接调用。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace guard {

// 遮罩矩形，归一化到「输入帧」坐标系（0..1，左上原点，y 向下）
struct RoiRect {
	float x = 0.0f;
	float y = 0.0f;
	float w = 1.0f;
	float h = 1.0f;

	bool valid() const { return w > 1e-4f && h > 1e-4f; }
};

enum MaskKind : int {
	MASK_SOLID = 0,  // 纯色，L1 强制档
	MASK_BLUR = 1,   // 高斯近似，L2 中低风险
	MASK_MOSAIC = 2, // UV 量化马赛克，通用
};

// 单条遮罩指令，归一化到「整帧」坐标系
struct MaskCmd {
	float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
	int state = 0;          // 0=SUSPECT（轻度）1=LOCKED（稳定）
	float score = 0.0f;     // 检测置信度，仅用于日志/调试
	int64_t first_seen = 0; // ns，用于淡入
	int64_t last_seen = 0;  // ns，用于 HOLD / 淡出
	uint32_t track_id = 0;
};

// 推理线程 → 渲染线程 的一帧结果。渲染线程只做 try_lock，绝不等锁。
struct MaskFrame {
	static constexpr int kMax = 16;

	int count = 0;
	MaskCmd items[kMax];
	bool panic = false;      // 全屏遮罩（PANIC 状态）
	int64_t stamp = 0;       // 发布时间 ns
};

// 单条检测结果，归一化到「ROI 内部」（0..1）
struct Detection {
	int class_id = 0;
	float score = 0.0f;
	float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
};

// 交给推理线程的一帧 ROI 位图（已 letterbox 到模型输入尺寸）
struct FrameView {
	int width = 0;
	int height = 0;
	std::vector<uint8_t> rgb; // width*height*3，RGB888，行序自上而下

	// letterbox 在模型输入中的填充比例（左右 / 上下各一份）
	float pad_x = 0.0f;
	float pad_y = 0.0f;

	RoiRect roi;             // 本帧取样的 ROI（帧归一化）
	uint64_t frame_id = 0;
	int64_t pts_ns = 0;
	int src_w = 0;           // 源分辨率，供日志与像素换算
	int src_h = 0;
};

} // namespace guard
