// guard-detector-ort.cpp -- YOLO (v8/v11 detect / v10 end2end) on ONNX Runtime
//
// 输出布局自适应：
//   [1, 4+nc, N]  v8/v11 标准导出（channels-first）
//   [1, N, 4+nc]  被转置导出
//   [1, N, 6]     v10 / 已内置 NMS（x1,y1,x2,y2,score,cls）
// 类别名从 ONNX metadata "names" 读取（Ultralytics 导出时会写入）。
#include "detect/detector.h"

#ifdef GUARD_WITH_ORT

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

namespace guard {
namespace {

std::wstring widen(const std::string &s)
{
	if (s.empty())
		return {};
#ifdef _WIN32
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
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
	size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos)
		return {};
	size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

std::string to_lower(std::string s)
{
	for (auto &c : s)
		c = (char)::tolower((unsigned char)c);
	return s;
}

// "{0: 'person', 1: 'bicycle'}" -> ["person", "bicycle"]
void parse_names(const std::string &src, std::vector<std::string> &out)
{
	size_t i = 0;
	while (i < src.size()) {
		size_t colon = src.find(':', i);
		if (colon == std::string::npos)
			break;

		size_t d = colon;
		while (d > i && ::isdigit((unsigned char)src[d - 1]))
			--d;

		size_t q = colon + 1;
		while (q < src.size() && (src[q] == ' ' || src[q] == '\t'))
			++q;
		if (q >= src.size() || (src[q] != '\'' && src[q] != '"')) {
			i = colon + 1;
			continue;
		}

		const char quote = src[q++];
		size_t end = src.find(quote, q);
		if (end == std::string::npos)
			break;

		int idx = 0;
		if (d < colon)
			idx = std::atoi(src.substr(d, colon - d).c_str());
		if (idx >= 0) {
			if ((int)out.size() <= idx)
				out.resize((size_t)idx + 1);
			out[(size_t)idx] = src.substr(q, end - q);
		}
		i = end + 1;
	}
}

struct RawBox {
	float x1, y1, x2, y2;
	float score;
	int cls;
};

float iou_of(const RawBox &a, const RawBox &b)
{
	const float ix1 = std::max(a.x1, b.x1), iy1 = std::max(a.y1, b.y1);
	const float ix2 = std::min(a.x2, b.x2), iy2 = std::min(a.y2, b.y2);
	const float iw = std::max(0.0f, ix2 - ix1), ih = std::max(0.0f, iy2 - iy1);
	const float inter = iw * ih;
	const float ua = (a.x2 - a.x1) * (a.y2 - a.y1) + (b.x2 - b.x1) * (b.y2 - b.y1) - inter;
	return ua > 1e-6f ? inter / ua : 0.0f;
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
	void parse_filter();

	std::unique_ptr<Ort::Env> env_;
	std::unique_ptr<Ort::Session> session_;
	std::string in_name_, out_name_;
	std::string backend_ = "none";
	std::string status_;

	std::vector<std::string> names_;
	std::map<int, bool> allowed_;
	bool filter_all_ = true;

	std::string cfg_filter_;
	float thresh_ = 0.35f;
	float iou_thresh_ = 0.45f;
	int max_det_ = 32;

	int in_w_ = 640, in_h_ = 384;
	std::vector<float> input_;
	std::vector<const char *> in_names_, out_names_;
};

void OrtDetector::parse_filter()
{
	filter_all_ = true;
	allowed_.clear();
	if (cfg_filter_.empty())
		return;

	std::stringstream ss(cfg_filter_);
	std::string token;
	bool any = false;
	while (std::getline(ss, token, ',')) {
		token = trim(token);
		if (token.empty())
			continue;
		any = true;

		char *endp = nullptr;
		long idx = std::strtol(token.c_str(), &endp, 10);
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
		if (found >= 0) {
			allowed_[found] = true;
		} else {
			blog(LOG_WARNING, "[obs-guard] 类别过滤串中的 \"%s\" 不在模型类别表里，已忽略",
			     token.c_str());
		}
	}
	filter_all_ = !any || allowed_.empty();
}

bool OrtDetector::load(const DetectorConfig &cfg, std::string &err)
{
	session_.reset();
	names_.clear();
	allowed_.clear();
	filter_all_ = true;
	cfg_filter_ = cfg.class_filter;
	thresh_ = cfg.conf;
	iou_thresh_ = cfg.iou;
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
				     "[obs-guard] DirectML 不可用，回退 CPU 推理（延迟会明显上升）");
		}

		const std::wstring wpath = widen(cfg.path);
		session_ = std::make_unique<Ort::Session>(*env_, wpath.c_str(), so);
		Ort::AllocatorWithDefaultOptions alloc;

		// ---- 输入尺寸：以模型图为准，不信任配置里的猜测 ----
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
			auto nm = session_->GetInputNameAllocated(0, alloc);
			in_name_ = nm.get();
		}
		{
			auto nm = session_->GetOutputNameAllocated(0, alloc);
			out_name_ = nm.get();
		}

		// ---- 类别名（可选）----
		try {
			auto meta = session_->GetModelMetadata();
			auto names_meta = meta.LookupCustomMetadataMapAllocated("names", alloc);
			if (names_meta)
				parse_names(names_meta.get(), names_);
		} catch (...) {
			// 没有 metadata 不算错误，用索引过滤即可
		}

		parse_filter();

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
	if (!session_)
		return false;
	if (frame.width != in_w_ || frame.height != in_h_)
		return false;

	// --- NCHW + /255 ---
	{
		const uint8_t *p = frame.rgb.data();
		const size_t planes = (size_t)in_w_ * (size_t)in_h_;
		float *r = input_.data();
		float *g = r + planes;
		float *b = g + planes;
		const float k = 1.0f / 255.0f;
		for (size_t i = 0; i < planes; ++i) {
			r[i] = (float)p[i * 3 + 0] * k;
			g[i] = (float)p[i * 3 + 1] * k;
			b[i] = (float)p[i * 3 + 2] * k;
		}
	}

	std::array<int64_t, 4> shape{1, 3, in_h_, in_w_};
	auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

	std::vector<Ort::Value> outs;
	try {
		auto tensor = Ort::Value::CreateTensor<float>(
			mem, input_.data(), input_.size(), shape.data(), shape.size());
		outs = session_->Run(Ort::RunOptions{nullptr}, in_names_.data(), &tensor, 1,
				     out_names_.data(), 1);
	} catch (const Ort::Exception &e) {
		blog(LOG_ERROR, "[obs-guard] 推理失败: %s", e.what());
		return false;
	}

	if (outs.empty())
		return false;

	auto info = outs[0].GetTensorTypeAndShapeInfo();
	const auto dims = info.GetShape();
	const float *data = outs[0].GetTensorData<float>();
	if (!data || dims.size() != 3)
		return false;

	std::vector<RawBox> cands;

	auto push_box = [&](float cx, float cy, float w, float h, float score, int cls) {
		if (score < thresh_)
			return;
		if (!filter_all_ && allowed_.find(cls) == allowed_.end())
			return;
		RawBox rb;
		rb.x1 = cx - w * 0.5f;
		rb.y1 = cy - h * 0.5f;
		rb.x2 = cx + w * 0.5f;
		rb.y2 = cy + h * 0.5f;
		rb.score = score;
		rb.cls = cls;
		cands.push_back(rb);
	};

	const int64_t d1 = dims[1], d2 = dims[2];
	const bool transposed = d1 > d2; // [1, N, C]
	const int64_t n = transposed ? d1 : d2;
	const int64_t c = transposed ? d2 : d1;

	if (c == 6) {
		// 已内置 NMS：x1,y1,x2,y2,score,cls
		for (int64_t i = 0; i < n; ++i) {
			const float *row = transposed ? data + i * c : nullptr;
			float v[6];
			for (int k = 0; k < 6; ++k)
				v[k] = transposed ? row[k] : data[k * n + i];
			if (v[4] < thresh_)
				continue;
			const int cls = (int)v[5];
			if (!filter_all_ && allowed_.find(cls) == allowed_.end())
				continue;
			RawBox rb{v[0], v[1], v[2], v[3], v[4], cls};
			cands.push_back(rb);
		}
	} else if (c >= 5) {
		const int64_t nc = c - 4;
		for (int64_t i = 0; i < n; ++i) {
			auto at = [&](int64_t k) -> float {
				return transposed ? data[i * c + k] : data[k * n + i];
			};
			float best = 0.0f;
			int best_cls = -1;
			for (int64_t k = 0; k < nc; ++k) {
				const float s = at(4 + k);
				if (s > best) {
					best = s;
					best_cls = (int)k;
				}
			}
			if (best_cls < 0 || best < thresh_)
				continue;
			if (!filter_all_ && allowed_.find(best_cls) == allowed_.end())
				continue;
			push_box(at(0), at(1), at(2), at(3), best, best_cls);
		}
	} else {
		return false;
	}

	if (cands.empty())
		return true;

	// --- 类内 NMS ---
	std::sort(cands.begin(), cands.end(),
		  [](const RawBox &a, const RawBox &b) { return a.score > b.score; });

	std::vector<RawBox> keep;
	keep.reserve(std::min<size_t>(cands.size(), (size_t)max_det_));
	for (const auto &cand : cands) {
		bool dup = false;
		for (const auto &k : keep) {
			if (k.cls == cand.cls && iou_of(k, cand) > iou_thresh_) {
				dup = true;
				break;
			}
		}
		if (!dup) {
			keep.push_back(cand);
			if ((int)keep.size() >= max_det_)
				break;
		}
	}

	// --- 模型像素 → ROI 归一化（去掉 letterbox 边）---
	const float sx = std::max(1e-3f, 1.0f - 2.0f * frame.pad_x);
	const float sy = std::max(1e-3f, 1.0f - 2.0f * frame.pad_y);
	const float mw = (float)in_w_, mh = (float)in_h_;

	for (const auto &k : keep) {
		float u1 = (k.x1 / mw - frame.pad_x) / sx;
		float v1 = (k.y1 / mh - frame.pad_y) / sy;
		float u2 = (k.x2 / mw - frame.pad_x) / sx;
		float v2 = (k.y2 / mh - frame.pad_y) / sy;

		u1 = std::clamp(u1, -0.02f, 1.02f);
		v1 = std::clamp(v1, -0.02f, 1.02f);
		u2 = std::clamp(u2, -0.02f, 1.02f);
		v2 = std::clamp(v2, -0.02f, 1.02f);

		Detection d;
		d.class_id = k.cls;
		d.score = k.score;
		d.x = u1;
		d.y = v1;
		d.w = std::max(0.0f, u2 - u1);
		d.h = std::max(0.0f, v2 - v1);
		if (d.w < 0.002f || d.h < 0.002f)
			continue;
		out.push_back(d);
	}

	return true;
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
// onnxruntime.dll 预加载
// OBS 插件放在 obs-plugins/64bit 下，Windows 的 DLL 搜索路径默认不含插件目录，
// 因此这里显式用 LOAD_WITH_ALTERED_SEARCH_PATH 把同目录的 onnxruntime.dll
// 先加载进进程，随后 /DELAYLOAD 的导入表会直接命中已加载模块。
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
	DWORD len = GetModuleFileNameW(self, path, MAX_PATH);
	if (len == 0 || len >= MAX_PATH) {
		cached_err = "插件路径过长";
		err = cached_err;
		return false;
	}

	std::wstring dir(path, len);
	size_t slash = dir.find_last_of(L"\\/");
	if (slash == std::wstring::npos) {
		cached_err = "插件路径异常";
		err = cached_err;
		return false;
	}
	dir.resize(slash);

	const std::wstring dll = dir + L"\\onnxruntime.dll";
	HMODULE h = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!h) {
		cached_err = "onnxruntime.dll 加载失败（错误码 " + std::to_string(GetLastError()) +
			     "），请确认已随插件一同部署";
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
