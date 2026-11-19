// guard-grabber.h -- 把ROI从GPU纹理取回CPU，并缩放到模型输入尺寸
#pragma once

#include <graphics/graphics.h>
#include <obs-module.h>

#include "core/types.h"

namespace guard {

// 取帧说明：
//   1) 把滤镜输入源渲染进一个全分辨率 texrender（纯 GPU，不读回）
//   2) 整帧 stage 到 staging surface，一次性 map 拿 CPU 指针
//   3) 只按行读取 ROI 区域，双线性缩放到 det_w x det_h 并做 letterbox
// 相比 D3D11 共享纹理方案，这里多了一次回读（1080p 约 3~5ms），
// 但完全不依赖渲染后端，OpenGL / D3D11 / Vulkan 都能跑，适合作为第一版通路。
class Grabber {
public:
	void release();

	bool grab(obs_source_t *target, uint32_t src_w, uint32_t src_h, const RoiRect &roi,
		  int det_w, int det_h, FrameView &out);

	~Grabber() { release(); }

private:
	bool ensure_texrender(uint32_t w, uint32_t h);
	bool ensure_stage(uint32_t w, uint32_t h);

	gs_texrender_t *tr_ = nullptr;
	uint32_t tr_w_ = 0, tr_h_ = 0;

	gs_stagesurf_t *stage_ = nullptr;
	uint32_t st_w_ = 0, st_h_ = 0;
};

} // namespace guard
