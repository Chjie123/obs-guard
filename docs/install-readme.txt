obs-guard v@VERSION@
OBS Studio 实时内容遮挡滤镜
==================================================

安装
----
1. 完全退出 OBS Studio（含托盘图标）
2. 双击 install.cmd，在用户账户控制提示中确认
3. 启动 OBS Studio

使用
----
选中需要保护的源 -> 滤镜 -> + -> "违禁内容遮挡"

首次配置建议先完成两项自检：

  勾选"显示检测区域边框"，画面出现品红色矩形，说明挂载与坐标正确；
  点击"测试遮罩"，检测区域内叠加三秒样本遮罩，说明渲染链路正常。

以上两步均不依赖模型。

卸载
----
双击 uninstall.cmd

手动安装
--------
分发包采用 OBS 标准目录结构。将 obs-plugins 与 data 两个目录整体覆盖到
OBS 安装目录即可（默认 C:\Program Files\obs-studio）。

说明
----
- 本包为未签名构建，部分安全软件可能提示，属预期行为。
- 本包不含检测模型。模型需由使用方自行导出为 ONNX 格式，再在滤镜面板中指定。
- 安装清单位于 obs-plugins\64bit\obs-guard.manifest.txt，卸载按该清单执行，
  不会影响其他插件。
- 运行环境要求：Windows 10 1909+ / Windows 11（x64），OBS Studio 28 及以上。

许可
----
本包采用非商业许可，可免费下载与使用，禁止商业用途。
转发或发布修改版本时，必须保留本说明、注明原作者 Chjie123 与仓库地址
https://github.com/Chjie123/obs-guard ，并说明相对原作品做了哪些修改。
商业使用需事先取得作者书面授权。完整条款见源码仓库中的 LICENSE 文件。

随包的 onnxruntime.dll 与 DirectML.dll 由 Microsoft 提供，遵循 MIT 许可，
不受上述条款约束。
