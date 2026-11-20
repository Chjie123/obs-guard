// filter/filter_internal.h -- 滤镜实例的内部状态
// 仅在 filter.cpp 与 properties.cpp 之间共享，不对外暴露。
#pragma once

#include <obs-module.h>

#include <atomic>
#include <cstdint>
#include <memory>

#include "capture/grabber.h"
#include "core/config.h"
#include "core/pipeline.h"
#include "core/types.h"
#include "render/renderer.h"

namespace guard {

// 取文案的薄封装，避免在实现中反复写 obs_module_text
inline const char *tr(const char *key)
{
	return obs_module_text(key);
}

struct GuardFilter {
	obs_source_t *context = nullptr;

	// 配置由 UI 线程写、渲染线程读：原子替换 shared_ptr，避免字符串撕裂
	std::shared_ptr<const Config> cfg;
	int64_t start_ns = 0;

	std::unique_ptr<Pipeline> pipeline;
	Renderer renderer;
	Grabber grabber;

	uint32_t width = 0;
	uint32_t height = 0;
	int64_t last_grab_ns = 0;
	int64_t next_frame_id = 0;

	MaskFrame mask; // 渲染线程私有副本
	bool in_grab = false;

	std::atomic<int64_t> test_until{0};
};

// 属性面板由 properties.cpp 构建
obs_properties_t *build_properties(void *data, void *type_data);

} // namespace guard
