// filter/filter.h -- 滤镜模块的对外入口
//
// 整个插件只暴露一个注册函数：把 obs_source_info 交给 libobs，之后由 OBS
// 负责实例化。实现被拆成三个编译单元：
//   filter.cpp         生命周期、每帧时序（取帧 -> 取结果 -> 合成）、注册
//   properties.cpp     属性面板构建与变更回调
//   filter_internal.h  两者共享的 GuardFilter 结构
#pragma once

namespace guard {

// 注册「违禁内容遮挡」滤镜（id: guard_mask_filter）。
// 必须在 obs_module_load 中调用一次，且位于任何源被创建之前。
void register_guard_filter();

} // namespace guard
