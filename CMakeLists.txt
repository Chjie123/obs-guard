cmake_minimum_required(VERSION 3.20)

project(obs-guard VERSION 0.1.3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# OBS 插件必须与 OBS 使用同一套 CRT
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/rundir")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/rundir/obs-plugins/64bit")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/rundir/obs-plugins/64bit")

option(GUARD_WITH_ORT "Build with ONNX Runtime inference backend" ON)
set(ONNXRUNTIME_ROOT "" CACHE PATH "Extracted onnxruntime-win-x64-* package (empty = auto download)")

# ---------------------------------------------------------------------------
# 1. 定位 OBS SDK
#    推荐：tools\fetch-obs-sdk.ps1 生成 sdk\，再传 -DOBS_SDK_DIR=<该目录>
#          （目录需含 include/ 与 lib/obs.lib）
#    其他：已有 OBS 源码与构建树 -> -DOBS_SOURCE_DIR= -DOBS_BUILD_DIR=
#          环境中已提供 libobs 的 CMake 包 -> find_package(libobs) 直接命中
# ---------------------------------------------------------------------------
set(OBS_LIB "")
set(OBS_INCLUDE_DIRS "")

# 传入的相对路径必须转绝对：否则 CMake 会把 ".\sdk/lib/obs.lib" 当成库名，
# 链接时拼成 "obs.lib.lib"。
foreach(_var OBS_SDK_DIR OBS_SOURCE_DIR OBS_BUILD_DIR ONNXRUNTIME_ROOT)
	if(${_var})
		get_filename_component(${_var} "${${_var}}" ABSOLUTE)
	endif()
endforeach()

find_package(libobs QUIET)

if(TARGET OBS::libobs)
	message(STATUS "obs-guard: 使用 find_package(libobs)")
elseif(OBS_SOURCE_DIR AND OBS_BUILD_DIR)
	message(STATUS "obs-guard: 使用 OBS 构建树 ${OBS_BUILD_DIR}")
	set(OBS_INCLUDE_DIRS
		"${OBS_SOURCE_DIR}/libobs"
		"${OBS_SOURCE_DIR}/deps/obs-scripting"
		"${OBS_BUILD_DIR}/config")

	if(CMAKE_BUILD_TYPE)
		set(_obs_lib "${OBS_BUILD_DIR}/libobs/${CMAKE_BUILD_TYPE}/obs.lib")
	else()
		set(_obs_lib "${OBS_BUILD_DIR}/libobs/Release/obs.lib")
	endif()
	if(NOT EXISTS "${_obs_lib}")
		file(GLOB _cand "${OBS_BUILD_DIR}/libobs/*/obs.lib")
		list(LENGTH _cand _n)
		if(_n GREATER 0)
			list(GET _cand 0 _obs_lib)
		endif()
	endif()
	if(NOT EXISTS "${_obs_lib}")
		message(FATAL_ERROR "找不到 obs.lib，请确认 -DOBS_BUILD_DIR 指向已编译的 OBS 构建目录")
	endif()
	set(OBS_LIB "${_obs_lib}")
elseif(OBS_SDK_DIR)
	message(STATUS "obs-guard: 使用 OBS_SDK_DIR ${OBS_SDK_DIR}")
	set(OBS_INCLUDE_DIRS "${OBS_SDK_DIR}/include")
	set(OBS_LIB "${OBS_SDK_DIR}/lib/obs.lib")
	if(NOT EXISTS "${OBS_LIB}")
		message(FATAL_ERROR "找不到 ${OBS_LIB}")
	endif()
else()
	message(FATAL_ERROR
		"未找到 OBS SDK。三选一：\n"
		"  A) 用 obs-plugintemplate 的构建环境（推荐，会自动拉依赖）\n"
		"  B) -DOBS_SOURCE_DIR=<obs-studio 源码> -DOBS_BUILD_DIR=<obs-studio 构建目录>\n"
		"  C) -DOBS_SDK_DIR=<含 include/ 与 lib/obs.lib 的目录>")
endif()

# ---------------------------------------------------------------------------
# 2. ONNX Runtime
# ---------------------------------------------------------------------------
set(ORT_DLLS "")

if(GUARD_WITH_ORT)
	if(NOT ONNXRUNTIME_ROOT)
		# 官方 releases 已不再为每个版本提供 Windows zip 资产，改由 script 从 NuGet 装配
		message(FATAL_ERROR
			"ONNXRUNTIME_ROOT 未设置。\n"
			"  1) 运行 .\\tools\\fetch-ort.ps1 生成 sdk\\ort\n"
			"  2) 重新配置时传 -DONNXRUNTIME_ROOT=sdk/ort，或用 scripts\\build.ps1 -OrtRoot\n"
			"  3) 只想验证渲染链路时改用 -NoOrt")
	endif()

	if(NOT EXISTS "${ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h")
		message(FATAL_ERROR "ONNXRUNTIME_ROOT 无效：${ONNXRUNTIME_ROOT}")
	endif()

	# 运行库改名后重新生成导入库。
	# 原因：Windows 的延迟加载按“文件名”解析，若进程里已存在同名模块（例如某个
	# 软件把旧版 onnxruntime.dll 装进了 System32），会直接复用它，导致
	# OrtGetApiBase()->GetApi(ORT_API_VERSION) 返回空指针并崩溃。
	# 改成唯一文件名后，解析结果只可能是随插件分发的这一份。
	set(ORT_DLL_NAME "obs_guard_ort")
	set(ORT_DEF "${CMAKE_CURRENT_BINARY_DIR}/${ORT_DLL_NAME}.def")
	set(ORT_IMPLIB "${CMAKE_CURRENT_BINARY_DIR}/${ORT_DLL_NAME}.lib")

	file(WRITE "${ORT_DEF}"
		"LIBRARY ${ORT_DLL_NAME}.dll\n"
		"EXPORTS\n"
		"    OrtGetApiBase\n"
		"    OrtSessionOptionsAppendExecutionProvider_DML\n")

	add_custom_command(
		OUTPUT "${ORT_IMPLIB}"
		COMMAND "${CMAKE_AR}" /nologo "/def:${ORT_DEF}" /machine:x64 "/out:${ORT_IMPLIB}"
		DEPENDS "${ORT_DEF}"
		VERBATIM
		COMMENT "生成 ${ORT_DLL_NAME}.lib")

	add_custom_target(obs_guard_ort_implib DEPENDS "${ORT_IMPLIB}")

	message(STATUS "obs-guard: ONNX Runtime -> ${ONNXRUNTIME_ROOT} (runtime renamed to ${ORT_DLL_NAME}.dll)")
endif()

# ---------------------------------------------------------------------------
# 3. 目标
# ---------------------------------------------------------------------------
set(GUARD_SOURCES
	src/plugin-main.cpp
	src/filter/filter.cpp
	src/filter/properties.cpp
	src/core/config.cpp
	src/core/pipeline.cpp
	src/core/tracker.cpp
	src/capture/grabber.cpp
	src/detect/detector_ort.cpp
	src/detect/yolo_post.cpp
	src/render/renderer.cpp)

set(GUARD_HEADERS
	src/filter/filter.h
	src/filter/filter_internal.h
	src/core/types.h
	src/core/constants.h
	src/core/config.h
	src/core/pipeline.h
	src/core/tracker.h
	src/capture/grabber.h
	src/detect/detector.h
	src/detect/yolo_post.h
	src/render/renderer.h)

add_library(obs-guard MODULE ${GUARD_SOURCES} ${GUARD_HEADERS})

target_compile_definitions(obs-guard PRIVATE
	UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS
	GUARD_VERSION="${PROJECT_VERSION}")

if(MSVC)
	target_compile_options(obs-guard PRIVATE /W3 /utf-8 /MP)
endif()

target_include_directories(obs-guard PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
	${OBS_INCLUDE_DIRS})

if(TARGET OBS::libobs)
	target_link_libraries(obs-guard PRIVATE OBS::libobs)
else()
	target_link_libraries(obs-guard PRIVATE "${OBS_LIB}")
endif()

if(WIN32)
	# 检测频率上限需要枚举显示模式（EnumDisplayDevices / EnumDisplaySettings）
	target_link_libraries(obs-guard PRIVATE user32)

	set_target_properties(obs-guard PROPERTIES
		PREFIX ""
		OUTPUT_NAME "obs-guard"
		SUFFIX ".dll")
endif()

if(GUARD_WITH_ORT)
	target_compile_definitions(obs-guard PRIVATE GUARD_WITH_ORT=1)
	target_include_directories(obs-guard PRIVATE "${ONNXRUNTIME_ROOT}/include")

	add_dependencies(obs-guard obs_guard_ort_implib)
	target_link_libraries(obs-guard PRIVATE "${ORT_IMPLIB}")

	if(MSVC)
		# 插件目录不在默认 DLL 搜索路径里，改为延迟加载 + 运行时显式预加载
		target_link_options(obs-guard PRIVATE "/DELAYLOAD:${ORT_DLL_NAME}.dll")
		target_link_libraries(obs-guard PRIVATE delayimp)
	endif()

	add_custom_command(TARGET obs-guard POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll"
			"$<TARGET_FILE_DIR:obs-guard>/${ORT_DLL_NAME}.dll"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${ONNXRUNTIME_ROOT}/lib/DirectML.dll"
			"$<TARGET_FILE_DIR:obs-guard>/DirectML.dll"
		COMMENT "复制 ONNX Runtime 运行库（重命名为 ${ORT_DLL_NAME}.dll）")
endif()

# ---------------------------------------------------------------------------
# 4. 数据目录：构建后直接铺进 rundir，便于本地装载
# ---------------------------------------------------------------------------
add_custom_command(TARGET obs-guard POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		"${CMAKE_CURRENT_SOURCE_DIR}/data"
		"${CMAKE_BINARY_DIR}/rundir/data/obs-plugins/obs-guard"
	COMMENT "同步 data/ 到 rundir")

# ---------------------------------------------------------------------------
# 5. 安装：install 前缀指向 OBS 安装目录即可
# ---------------------------------------------------------------------------
install(TARGETS obs-guard
	RUNTIME DESTINATION "obs-plugins/64bit"
	LIBRARY DESTINATION "obs-plugins/64bit")

install(DIRECTORY data/ DESTINATION "data/obs-plugins/obs-guard")

if(GUARD_WITH_ORT)
	install(FILES "${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll"
		DESTINATION "obs-plugins/64bit"
		RENAME "${ORT_DLL_NAME}.dll")
	install(FILES "${ONNXRUNTIME_ROOT}/lib/DirectML.dll"
		DESTINATION "obs-plugins/64bit")
endif()
