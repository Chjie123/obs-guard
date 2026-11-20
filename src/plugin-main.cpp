// plugin-main.cpp -- OBS 模块入口
//
// libobs 的加载顺序：LoadLibrary -> obs_module_set_pointer -> obs_module_load。
// 只有 obs_module_load 返回 true，模块才会出现在滤镜列表里。
//
// 这里不做任何初始化工作（不建推理会话、不碰图形），只注册 source 类型；
// 模型加载与图形资源都推迟到滤镜实例创建时，避免拖慢 OBS 启动。
#include "filter/filter.h"

#include <obs-module.h>

#include "core/config.h"

OBS_DECLARE_MODULE()

// 定义插件自己的 obs_module_text / obs_module_set_locale 等符号。
// libobs 不导出 obs_module_text，必须由模块本身提供，否则链接期报 LNK2019。
OBS_MODULE_USE_DEFAULT_LOCALE("obs-guard", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "GUI-free real-time prohibited-content masking filter for OBS Studio";
}

bool obs_module_load(void)
{
	guard::register_guard_filter();
	// 把刷新率上限写进日志：面板滑块的量程由它决定，排障时需要对照
	blog(LOG_INFO, "[obs-guard] 插件已加载 v" GUARD_VERSION
		       "（滤镜：违禁内容遮挡，检测频率上限 %d fps）",
	     guard::display_max_refresh_hz());
	return true;
}

void obs_module_unload(void)
{
	// 滤镜实例由 OBS 负责销毁，这里没有需要回收的全局资源
	blog(LOG_INFO, "[obs-guard] 插件已卸载");
}
