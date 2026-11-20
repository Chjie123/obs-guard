// detect/detector_ort.cpp -- 基于 ONNX Runtime 的 YOLO 检测器
//
// 本文件只负责：会话生命周期、执行提供程序选择、输入张量填充、调用推理。
// 输出解析与坐标回映射由 yolo_post.cpp 承担，便于独立验证。
#include "detect/detector.h"

#ifdef GUARD_WITH_ORT

#include <util/base.h> // blog / LOG_*

// ORT 的 C++ 头默认把 API 表初始化写成全局初始化器：
//     const OrtApi *Global<T>::api_ = OrtGetApiBase()->GetApi(ORT_API_VERSION);
// 它在 DllMain 期间执行，会触发对 obs_guard_ort.dll 的延迟加载。插件目录不在
// DLL 搜索路径内，加载失败后 thunk 跳空指针，产生访问违例；CRT 捕获后返回
// FALSE，于是 LoadLibrary 报 ERROR_DLL_INIT_FAILED（1114），插件根本装不上。
// 改为手动初始化：load() 里先按完整路径预加载运行库，再取 API 表。
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>
#if __has_include(<dml_provider_factory.h>)
#include <dml_provider_factory.h>
#define GUARD_HAVE_DML 1
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

#include "detect/yolo_post.h"

namespace guard {
namespace {

std::wstring widen(const std::string &s)
{
	if (s.empty())
		return {};
#ifdef _WIN32
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	if (n <= 0)
		return {};
	std::wstring w((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
	return w;
#else
	return std::wstring(s.begin(), s.end());
#endif
}

std::string trim(const std::string &s)
{
	const size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos)
		return {};
	const size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

std::string to_lower(std::string s)
{
	for (auto &ch : s)
		ch = (char)::tolower((unsigned char)ch);
	return s;
}

class OrtDetector : public IDetector {
public:
	bool load(const DetectorConfig &cfg, std::string &err) override;
	bool run(const FrameView &frame, std::vector<Detection> &out) override;

	int input_width() const override { return in_w_; }
	int input_height() const override { return in_h_; }
	const char *backend() const override { return backend_.c_str(); }
	std::string status() const override { return status_; }

private:
	void resolve_class_filter();

	std::unique_ptr<Ort::Env> env_;
	std::unique_ptr<Ort::Session> session_;
	std::string in_name_, out_name_;
	std::string backend_ = "none";
	std::string status_;

	std::vector<std::string> names_;
	std::map<int, bool> allowed_;
	bool filter_all_ = true;

	std::string filter_text_;
	float conf_ = 0.35f;
	float iou_ = 0.45f;
	int max_det_ = 32;

	int in_w_ = 640, in_h_ = 384;
	std::vector<float> input_;
	std::vector<const char *> in_names_, out_names_;
};

// 类别过滤串：可混写类别名与索引，未匹配条目告警后忽略
void OrtDetector::resolve_class_filter()
{
	filter_all_ = true;
	allowed_.clear();
	if (filter_text_.empty())
		return;

	std::stringstream ss(filter_text_);
	std::string token;
	bool any = false;

	while (std::getline(ss, token, ',')) {
		token = trim(token);
		if (token.empty())
			continue;
		any = true;

		char *endp = nullptr;
		const long idx = std::strtol(token.c_str(), &endp, 10);
		if (endp && *endp == '\0') {
			allowed_[(int)idx] = true;
			continue;
		}

		int found = -1;
		for (size_t i = 0; i < names_.size(); ++i) {
			if (to_lower(names_[i]) == to_lower(token)) {
				found = (int)i;
				break;
			}
		}
		if (found >= 0)
			allowed_[found] = true;
		else
			blog(LOG_WARNING, "[obs-guard] 类别过滤中的 \"%s\" 不在模型类别表内，已忽略",
			     token.c_str());
	}

	filter_all_ = !any || allowed_.empty();
}

bool OrtDetector::load(const DetectorConfig &cfg, std::string &err)
{
	session_.reset();
	names_.clear();
	allowed_.clear();
	filter_all_ = true;

#ifdef _WIN32
	// 运行库必须先按完整路径加载，否则延迟加载会按文件名在默认路径里找，
	// 既可能找不到，也可能命中系统目录中的同名旧版本。
	if (!preload_runtime(err))
		return false;

	// 手动初始化 API 表（见文件头部 ORT_API_MANUAL_INIT 的说明）。
	// 先验证 GetApi 返回非空，避免运行库与头文件版本不一致时二次崩溃。
	static bool api_ready = false;
	if (!api_ready) {
		const OrtApiBase *base = OrtGetApiBase();
		const OrtApi *api = base ? base->GetApi(ORT_API_VERSION) : nullptr;
		if (!api) {
			err = "ONNX Runtime 版本不匹配（运行库与编译头文件不一致）";
			return false;
		}
		Ort::InitApi(api);
		api_ready = true;
	}
#endif

	filter_text_ = cfg.class_filter;
	conf_ = cfg.conf;
	iou_ = cfg.iou;
	max_det_ = std::max(1, cfg.max_det);

	if (cfg.path.empty()) {
		err = "模型路径为空";
		return false;
	}

	try {
		if (!env_)
			env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "obs-guard");

		Ort::SessionOptions so;
		so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
		so.SetIntraOpNumThreads(2);
		so.SetInterOpNumThreads(1);

		const auto providers = Ort::GetAvailableProviders();
		const bool want_dml = (cfg.device == 0 || cfg.device == 1);
		bool dml_ok = false;

#ifdef GUARD_HAVE_DML
		const bool has_dml = std::find(providers.begin(), providers.end(),
					       "DmlExecutionProvider") != providers.end();
		if (want_dml && has_dml) {
			Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(so, 0));
			backend_ = "DirectML";
			dml_ok = true;
		}
#else
		(void)want_dml;
#endif

		if (!dml_ok) {
			backend_ = "CPU";
			if (cfg.device != 2)
				blog(LOG_WARNING,
				     "[obs-guard] DirectML 不可用，回退 CPU 推理（延迟显著上升）");
		}

		const std::wstring wpath = widen(cfg.path);
		session_ = std::make_unique<Ort::Session>(*env_, wpath.c_str(), so);
		Ort::AllocatorWithDefaultOptions alloc;

		// 输入尺寸以计算图为准，不信任配置中的猜测
		{
			auto info = session_->GetInputTypeInfo(0);
			auto shape = info.GetTensorTypeAndShapeInfo().GetShape();
			if (shape.size() == 4) {
				if (shape[2] > 0)
					in_h_ = (int)shape[2];
				if (shape[3] > 0)
					in_w_ = (int)shape[3];
				if (shape[2] <= 0 || shape[3] <= 0)
					blog(LOG_WARNING,
					     "[obs-guard] 模型输入为动态尺寸，按 %dx%d 推理",
					     in_w_, in_h_);
			}
			in_name_ = session_->GetInputNameAllocated(0, alloc).get();
		}
		out_name_ = session_->GetOutputNameAllocated(0, alloc).get();

		// 类别名缺失不影响推理，仅影响按名称过滤
		try {
			auto meta = session_->GetModelMetadata();
			auto names_meta = meta.LookupCustomMetadataMapAllocated("names", alloc);
			if (names_meta)
				parse_class_names(names_meta.get(), names_);
		} catch (...) {
		}

		resolve_class_filter();

		std::ostringstream oss;
		oss << backend_ << " · " << in_w_ << "x" << in_h_ << " · "
		    << (names_.empty() ? std::string("类别名未知")
				       : std::to_string(names_.size()) + " 类");
		status_ = oss.str();

		input_.assign((size_t)3 * (size_t)in_w_ * (size_t)in_h_, 0.0f);
		in_names_ = {in_name_.c_str()};
		out_names_ = {out_name_.c_str()};

		blog(LOG_INFO, "[obs-guard] 模型已加载: %s (%s)", cfg.path.c_str(),
		     status_.c_str());
		return true;
	} catch (const Ort::Exception &e) {
		err = std::string("ONNX Runtime: ") + e.what();
		return false;
	} catch (const std::exception &e) {
		err = e.what();
		return false;
	}
}

bool OrtDetector::run(const FrameView &frame, std::vector<Detection> &out)
{
	out.clear();
	if (!session_ || frame.width != in_w_ || frame.height != in_h_)
		return false;

	// RGB24 -> NCHW 归一化
	{
		const uint8_t *src = frame.rgb.data();
		const size_t planes = (size_t)in_w_ * (size_t)in_h_;
		float *r = input_.data();
		float *g = r + planes;
		float *b = g + planes;
		const float k = 1.0f / 255.0f;
		for (size_t i = 0; i < planes; ++i) {
			r[i] = (float)src[i * 3 + 0] * k;
			g[i] = (float)src[i * 3 + 1] * k;
			b[i] = (float)src[i * 3 + 2] * k;
		}
	}

	std::array<int64_t, 4> shape{1, 3, in_h_, in_w_};
	auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

	std::vector<Ort::Value> outputs;
	try {
		auto tensor = Ort::Value::CreateTensor<float>(
			mem, input_.data(), input_.size(), shape.data(), shape.size());
		outputs = session_->Run(Ort::RunOptions{nullptr}, in_names_.data(), &tensor, 1,
					out_names_.data(), 1);
	} catch (const Ort::Exception &e) {
		blog(LOG_ERROR, "[obs-guard] 推理失败: %s", e.what());
		return false;
	}

	if (outputs.empty())
		return false;

	auto info = outputs[0].GetTensorTypeAndShapeInfo();

	YoloPostParams params;
	params.conf = conf_;
	params.iou = iou_;
	params.max_det = max_det_;
	params.num_classes = (int)names_.size();
	params.input_w = in_w_;
	params.input_h = in_h_;
	params.pad_x = frame.pad_x;
	params.pad_y = frame.pad_y;
	params.allowed = filter_all_ ? nullptr : &allowed_;

	return decode_yolo_output(outputs[0].GetTensorData<float>(), info.GetShape(), params,
				  out);
}

} // namespace

std::unique_ptr<IDetector> create_detector()
{
	return std::make_unique<OrtDetector>();
}

bool detector_available()
{
	return true;
}

} // namespace guard

// ---------------------------------------------------------------------------
// onnxruntime 运行库预加载
//
// 运行库以 obs_guard_ort.dll 的名字随插件分发，而不是原名。原因：Windows 的
// 延迟加载按文件名解析，若进程内已存在同名模块（例如第三方软件把旧版
// onnxruntime.dll 放进了 System32），就会复用它，导致 API 版本不匹配并在
// OrtDetector::load 里空指针崩溃。唯一文件名可以排除这种可能。
//
// 插件位于 obs-plugins\64bit，该目录不在 Windows 默认 DLL 搜索路径内，
// 因此用 LOAD_WITH_ALTERED_SEARCH_PATH 显式加载。
// ---------------------------------------------------------------------------
#ifdef _WIN32

namespace guard {

bool preload_runtime(std::string &err)
{
	static bool loaded = false;
	static std::string cached_err;

	if (loaded)
		return true;
	if (!cached_err.empty()) {
		err = cached_err;
		return false;
	}

	HMODULE self = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
					GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				(LPCWSTR)(const void *)&preload_runtime, &self)) {
		cached_err = "无法定位插件模块路径";
		err = cached_err;
		return false;
	}

	wchar_t path[MAX_PATH] = {0};
	const DWORD len = GetModuleFileNameW(self, path, MAX_PATH);
	if (len == 0 || len >= MAX_PATH) {
		cached_err = "插件路径过长";
		err = cached_err;
		return false;
	}

	std::wstring dir(path, len);
	const size_t slash = dir.find_last_of(L"\\/");
	if (slash == std::wstring::npos) {
		cached_err = "插件路径异常";
		err = cached_err;
		return false;
	}
	dir.resize(slash);

	const std::wstring dll = dir + L"\\obs_guard_ort.dll";
	if (!LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
		cached_err = "obs_guard_ort.dll 加载失败（错误码 " +
			     std::to_string(GetLastError()) + "），请确认已随插件部署";
		err = cached_err;
		return false;
	}

	loaded = true;
	return true;
}

} // namespace guard

#else

namespace guard {

bool preload_runtime(std::string &)
{
	return true;
}

} // namespace guard

#endif

#else // !GUARD_WITH_ORT

namespace guard {

std::unique_ptr<IDetector> create_detector()
{
	return nullptr;
}

bool detector_available()
{
	return false;
}

bool preload_runtime(std::string &err)
{
	err = "构建时未启用 ONNX Runtime（-DGUARD_WITH_ORT=OFF）";
	return false;
}

} // namespace guard

#endif // GUARD_WITH_ORT
