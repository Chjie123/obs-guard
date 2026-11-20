// core/constants.h -- 全局常量
// 集中管理跨模块使用的数值，避免魔法数字散落在实现里。
#pragma once

#include <cstdint>

namespace guard {

// 检测节拍
constexpr int kDefaultDetFps = 10;
constexpr int kMinDetFps = 0; // 0 = 不推理，仅保留已有遮罩的淡出

// 刷新率探测的兜底上限：个别虚拟显示器会报出异常大的值
constexpr int kMaxDetFpsCap = 240;

// 遮罩矩形数量：面板默认值，硬上限见 MaskFrame::kMax
constexpr int kDefaultMaskLimit = 16;

// 遮罩淡入时长：目标刚出现时避免突然跳出
constexpr int64_t kFadeInNs = 60LL * 1000000LL;

// fail-closed 启动宽限：模型尚未加载完成时不要立刻全屏遮挡
constexpr int64_t kFailClosedGraceNs = 3000LL * 1000000LL;

// 「测试遮罩」的持续时长
constexpr int64_t kTestMaskNs = 3000LL * 1000000LL;

// 属性面板取值范围
constexpr int kRoiPixelMax = 8192;

// 属性键：两套坐标系的字段名，属性面板与显隐回调共用
inline constexpr const char *kRoiPercentKeys[4] = {"pct_x", "pct_y", "pct_w", "pct_h"};
inline constexpr const char *kRoiPixelKeys[4] = {"px_x", "px_y", "px_w", "px_h"};

} // namespace guard
