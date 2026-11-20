// tests/yolo_decode_test.cpp -- 输出布局解码的回归测试
//
// 覆盖三类布局与 letterbox 回映射，不依赖 OBS / ONNX Runtime，可直接用 cl 编译：
//   cl /nologo /utf-8 /std:c++17 /EHsc /MD /I src tests\yolo_decode_test.cpp ^
//      src\detect\yolo_post.cpp /Fe:yolo_decode_test.exe
//
// 背景：c == 6 的布局二义性曾导致 2 类模型（4 + 2）被误判成已内置 NMS 的
// [x1, y1, x2, y2, score, cls]，整批候选被置信度过滤干净，表现为「模型加载
// 成功但画面永远不被处理」。
#include "detect/yolo_post.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace guard;

static int failures = 0;

static void check(bool cond, const char *what)
{
	printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
	if (!cond)
		failures++;
}

int main()
{
	// ---- 1) 2 类稠密输出 [1, 6, 10]：c == 6 且 nc == 2 ----
	{
		const int C = 6, N = 10;
		std::vector<float> d((size_t)C * N, 0.0f);
		// anchor 0：cx=320 cy=192 w=64 h=128，类别 0 得分 0.90
		d[0 * N + 0] = 320; d[1 * N + 0] = 192; d[2 * N + 0] = 64; d[3 * N + 0] = 128;
		d[4 * N + 0] = 0.90f; d[5 * N + 0] = 0.05f;
		// anchor 1：重叠框，得分更低，应被 NMS 掉
		d[0 * N + 1] = 322; d[1 * N + 1] = 194; d[2 * N + 1] = 64; d[3 * N + 1] = 128;
		d[4 * N + 1] = 0.70f; d[5 * N + 1] = 0.02f;
		// anchor 2：另一处，低分，应被 conf 过滤
		d[0 * N + 2] = 50; d[1 * N + 2] = 50; d[2 * N + 2] = 20; d[3 * N + 2] = 20;
		d[4 * N + 2] = 0.10f;

		YoloPostParams p;
		p.conf = 0.35f; p.iou = 0.45f; p.max_det = 32;
		p.num_classes = 2;
		p.input_w = 640; p.input_h = 384;

		std::vector<Detection> out;
		const bool ok = decode_yolo_output(d.data(), {1, C, N}, p, out);
		printf("case1 dense 2-class:/n");
		check(ok, "decode 返回 true");
		check(out.size() == 1, "只保留 1 个目标（NMS + conf）");
		if (!out.empty()) {
			check(out[0].class_id == 0, "类别为 0");
			check(out[0].score > 0.89f && out[0].score < 0.91f, "置信度 0.90");
			// 期望 x = (320-32)/640 = 0.45
			check(out[0].x > 0.44f && out[0].x < 0.46f, "x 归一化 0.45");
			check(out[0].y > 0.32f && out[0].y < 0.35f, "y 归一化 (192-64)/384=0.333");
		}
	}

	// ---- 2) 已内置 NMS 的输出 [1, 100, 6]，类别未知 ----
	{
		const int N = 100, C = 6;
		std::vector<float> d((size_t)N * C, 0.0f);
		d[0 * C + 0] = 10;  d[0 * C + 1] = 20; d[0 * C + 2] = 110; d[0 * C + 3] = 220;
		d[0 * C + 4] = 0.88f; d[0 * C + 5] = 1.0f;

		YoloPostParams p;
		p.conf = 0.35f; p.iou = 0.45f; p.max_det = 32;
		p.num_classes = 0;
		p.input_w = 640; p.input_h = 384;

		std::vector<Detection> out;
		const bool ok = decode_yolo_output(d.data(), {1, N, C}, p, out);
		printf("case2 nms-layout [1,N,6]:\n");
		check(ok, "decode 返回 true");
		check(out.size() == 1, "识别出 1 个目标");
		if (!out.empty())
			check(out[0].class_id == 1 && out[0].score > 0.87f, "类别 1 / 置信度 0.88");
	}

	// ---- 3) letterbox 回映射：pad_x = 0.1 时 x 应被放大 ----
	{
		const int C = 6, N = 10;
		std::vector<float> d((size_t)C * N, 0.0f);
		d[0 * N + 0] = 320; d[1 * N + 0] = 192; d[2 * N + 0] = 64; d[3 * N + 0] = 64;
		d[4 * N + 0] = 0.9f;

		YoloPostParams p;
		p.num_classes = 2;
		p.input_w = 640; p.input_h = 384;
		p.pad_x = 0.1f; p.pad_y = 0.0f;

		std::vector<Detection> out;
		decode_yolo_output(d.data(), {1, C, N}, p, out);
		printf("case3 letterbox:/n");
		check(out.size() == 1, "有输出");
		if (!out.empty())
			check(fabsf(out[0].x + out[0].w * 0.5f - 0.5f) < 0.01f, "pad_x 补偿后中心 x 约 0.5");
	}

	printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASS", failures);
	return failures ? 1 : 0;
}
