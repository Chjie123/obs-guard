#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""把 Ultralytics YOLO 权重导出为 obs-guard 可直接加载的 ONNX。

用法:
    python export_onnx.py --weights yolo11s.pt
    python export_onnx.py --weights yolo11s.pt --imgsz 384x640
    python export_onnx.py --weights best.pt --imgsz 384x640 --check

要点:
  * 别用 640x640。16:9 源塞进方形输入有 43% 是黑边，纯浪费算力。
    640x384（stride 32 对齐）计算量低 40%，有效像素密度更高。
  * 模型输入尺寸由插件从计算图读取，不需要在 OBS 面板里再填一遍。
  * 类别名会写进 ONNX metadata，插件面板里可以直接按名字过滤。
"""
import argparse
import os
import sys


def parse_size(text):
    text = text.lower().replace(" ", "")
    if "x" in text:
        h, w = text.split("x", 1)
        return int(h), int(w)
    v = int(text)
    return v, v


def main():
    ap = argparse.ArgumentParser(description="Export YOLO .pt to .onnx for obs-guard")
    ap.add_argument("--weights", required=True, help="path to .pt file")
    ap.add_argument("--imgsz", default="384x640", help="HxW, default 384x640 (16:9)")
    ap.add_argument("--opset", type=int, default=12)
    ap.add_argument("--batch", type=int, default=1)
    ap.add_argument("--half", action="store_true", help="FP16 (needs GPU at runtime)")
    ap.add_argument("--dynamic", action="store_true", help="dynamic input size")
    ap.add_argument("--check", action="store_true", help="inspect the exported model")
    args = ap.parse_args()

    if not os.path.isfile(args.weights):
        print("[x] weights not found: %s" % args.weights)
        return 1

    try:
        from ultralytics import YOLO
    except ImportError:
        print("[x] pip install ultralytics onnx onnxsim onnxruntime")
        return 1

    h, w = parse_size(args.imgsz)
    if h % 32 or w % 32:
        print("[!] H and W should be multiples of 32 (stride); got %dx%d" % (h, w))

    print("[*] exporting %s  imgsz=%dx%d opset=%d" % (args.weights, w, h, args.opset))
    model = YOLO(args.weights)
    out = model.export(
        format="onnx",
        imgsz=(h, w),
        opset=args.opset,
        batch=args.batch,
        half=args.half,
        dynamic=args.dynamic,
        simplify=True,
    )
    print("[+] done: %s" % out)
    print("[+] size: %.1f MB" % (os.path.getsize(out) / 1024.0 / 1024.0))

    if args.check:
        try:
            import onnxruntime as ort
        except ImportError:
            print("[!] onnxruntime not installed, skip check")
            return 0

        sess = ort.InferenceSession(out, providers=["CPUExecutionProvider"])
        inp = sess.get_inputs()[0]
        outs = sess.get_outputs()
        print("[i] input : %s %s" % (inp.name, inp.shape))
        print("[i] output: %s %s" % (outs[0].name, outs[0].shape))

        meta = sess.get_modelmeta().custom_metadata_map
        names = meta.get("names", "")
        if names:
            print("[i] classes: %s" % names[:300])
        else:
            print("[!] no 'names' metadata; class filter must use indices")

        shape = outs[0].shape
        if len(shape) == 3:
            c = shape[1] if isinstance(shape[1], int) else None
            if c == 6:
                print("[i] layout: end2end (N,6) with built-in NMS")
            elif c:
                print("[i] layout: channels-first, %d classes" % (c - 4))
    return 0


if __name__ == "__main__":
    sys.exit(main())
