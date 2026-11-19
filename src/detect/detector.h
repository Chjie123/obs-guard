// guard-detector.h -- 检测器抽象
// 规划 §5.3 要求：检测器统一抽象为 IDetector，与推理框架解耦，
// 后续换 TensorRT / YOLOX / RT-DETR 只替换实现，不动上层。
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/types.h"

namespace guard {

struct DetectorConfig {
	std::string path;
	int device = 0; // 0 auto / 1 DML / 2 CPU
	float conf = 0.35f;
	float iou = 0.45f;
	int max_det = 32;
	std::string class_filter;
};

class IDetector {
public:
	virtual ~IDetector() = default;

	virtual bool load(const DetectorConfig &cfg, std::string &err) = 0;
	virtual bool run(const FrameView &frame, std::vector<Detection> &out) = 0;

	virtual int input_width() const = 0;
	virtual int input_height() const = 0;
	virtual const char *backend() const = 0;
	virtual std::string status() const = 0;
};

// 构建时未启用 ORT 则返回 nullptr
std::unique_ptr<IDetector> create_detector();
bool detector_available();

// 进程内显式预加载 onnxruntime.dll（延迟加载 + 指定搜索路径），
// 返回 false 表示运行库缺失，UI 应直接给出可读提示。
bool preload_runtime(std::string &err);

} // namespace guard
