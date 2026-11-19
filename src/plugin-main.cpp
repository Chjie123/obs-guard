#include "filter/filter.h"

#include <obs-module.h>

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
	blog(LOG_INFO, "[obs-guard] 插件已加载 v" GUARD_VERSION
		       "（滤镜：违禁内容遮挡）");
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[obs-guard] 插件已卸载");
}
