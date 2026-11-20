// core/config.h -- 插件配置模型 + obs_data 读写
#pragma once

#include <obs-module.h>

#include <cstdint>
#include <string>

#include "core/constants.h"
#include "core/types.h"

namespace guard {

enum CoordMode : int {
	COORD_PERCENT = 0, // 按输入帧百分比，分辨率无关（默认）
	COORD_PIXEL = 1,   // 按输入帧像素；挂在「显示器采集」上时即屏幕坐标
};

enum DeviceKind : int {
	DEVICE_AUTO = 0,
	DEVICE_DML = 1,
	DEVICE_CPU = 2,
};

enum FailMode : int {
	FAIL_OPEN = 0, // 推理不可用 → 画面直通，保直播
	FAIL_CLOSED = 1, // 推理不可用 → 全屏遮罩，保合规
};

struct Config {
	// ---- 基本 ----
	bool enabled = true;
	int det_fps = 10;      // 检测频率（遮罩始终每帧绘制）
	int fail_mode = FAIL_OPEN;

	// ---- 检测区域 ----
	int coord_mode = COORD_PERCENT;
	double pct[4] = {0.0, 0.0, 100.0, 100.0};      // x, y, w, h（%）
	double px[4] = {0.0, 0.0, 1280.0, 720.0};      // x, y, w, h（像素）

	// ---- 模型 ----
	std::string model_path;
	int device = DEVICE_AUTO;
	float conf = 0.35f;
	float iou = 0.45f;
	int max_det = 32;
	int mask_limit = kDefaultMaskLimit; // 实际绘制的遮罩矩形上限
	std::string class_filter;           // 空 = 全部类别

	// ---- 遮罩样式 ----
	int mask_kind = MASK_MOSAIC;
	uint32_t mask_color = 0xFF000000; // obs_data color 为 0xAABBGGRR
	float mask_opacity = 0.85f;
	float blur_px = 16.0f;   // 1080p 基准
	float block_px = 16.0f;  // 1080p 基准
	float expand = 0.10f;    // 框体外扩比例（0.08~0.12）
	float pad_px = 4.0f;     // 额外像素边距

	// ---- 平滑 ----
	int hold_ms = 300; // 目标消失后保持时长
	int fade_ms = 180; // 淡出时长

	// ---- 调试 ----
	bool debug_roi = false;
	bool debug_box = false;
	bool debug_conf = false; // 在命中框上叠加置信度读数

	// ROI 换成归一化矩形（相对输入帧）
	RoiRect roi_rect(uint32_t src_w, uint32_t src_h) const;
};

void config_defaults(obs_data_t *settings);
void config_load(Config &cfg, obs_data_t *settings);

// 只比较「需要重建推理会话」的字段
bool config_needs_reload(const Config &a, const Config &b);

// 当前显示器支持的最高刷新率，作为「检测频率」滑块的上限。
// 探测失败（虚拟机 / 远程桌面）时返回 60。
int display_max_refresh_hz();

} // namespace guard
