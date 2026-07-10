# Stopwatch Vokie Firmware Changelog

## 0.2.0-vokie-pcm - 2026-07-10

- 记录类型：历史补记。
- 修改内容：修复 Stopwatch 屏幕闪烁；保持 250ms UI 刷新节奏；改为 `M5Canvas` 离屏缓冲后一次性推送到屏幕；无变化时跳过重复绘制。
- 烧录状态：已烧录到设备。
- 测试结论：用户确认界面已经不闪烁，效果良好。
- 对应 commit：`33cfbb3 修复 Stopwatch 界面闪烁`。
- 备注：该次修复未更新固件版本号，后续固件烧录必须遵守 `AGENTS.md` 中的版本与追踪规则。
