# VibeBoard 接入 Vokie 开发记录

> 记录日期：2026-08-08  
> 设备名称：VibeBoard（对话中也曾写作 Vibe Broad、Vive Broad、vibebroad）  
> 记录范围：原厂固件设备调研、Vokie 桌面端接入、实机调试与 UI 设计  
> 固件状态：未修改、未烧录 VibeBoard 固件

## 1. 开发目标

在不修改 VibeBoard 原厂固件、不模拟 M5Stack StopWatch BLE 协议的前提下，让
Vokie 在 macOS 上直接发现并使用 VibeBoard：

- K4 触发现有 PTT 会话；
- VibeBoard 麦克风音频作为外部 PCM 音频源；
- K2 发送 Enter，K3 撤销上一次 Vokie 输出；
- 外设接入不得创建或改变 ASR 识别链路；
- VibeBoard 与 StopWatch 在设置页中作为两个独立外设展示。

## 2. 已验证设备基线

| 项目 | 实机值 |
| --- | --- |
| USB VID/PID | `303a:1001` |
| USB Serial | `14:C1:9F:48:59:4C` |
| Vokie 归一化设备 ID | `14C19F48594C` |
| 固件设备 ID | `VS-14C19F48594C` |
| hardware | `vibe_keyboard` |
| 原厂固件 | `0.3.8` |
| 主控 | ESP32-S3 |
| 已验证桌面平台 | macOS |

设备连接后，Vokie 日志可观察到：

```text
[VibeBoard] connected device=14C19F48594C firmware=0.3.8
```

## 3. 资料来源

- 产品页：<https://torchfun.com/vibeboard/>
- 参考实现：<https://github.com/zerob13/vibe-keyboard>
- 接入时参考的源码版本：`019cfa099c2cb3b8d7b9aad665dbc4fd4a09096a`
- 参考实现许可证：MIT；授权文本已随 Vokie helper 源码保留。

`zerob13/vibe-keyboard` 包含自定义 ESP32-S3 固件及 macOS 控制端。Vokie 本次只
参考其公开协议与主机侧实现，没有将该自定义固件烧录到实机。原厂 `0.3.8` 与
参考仓库的替换固件必须视为两个不同对象，不能直接把替换固件能力当作原厂能力。

## 4. 架构原则

### 4.1 外设与 ASR 是两个正交维度

外设只负责两类输入：

1. 触发事件，例如 K4 press/release；
2. 音频数据，例如 VibeBoard USB Opus 解码后的 PCM。

ASR 链路只由 Vokie 的会话模式、用户配置和服务端配置决定，包括本地/远端、
WebRTC/WebSocket、模型、热词、失败策略和结果处理。任何外设都不得：

- 选择或覆盖 ASR provider；
- 创建外设专属 ASR client；
- 复制 PTT 的识别、后处理、历史或粘贴链路；
- 因音频来源不同而改变既有 PTT 策略。

K4 和键盘快捷键触发的都是同一个 `ptt` 会话。两者只允许在触发来源和采集来源
上不同。

### 4.2 `pcm-generator` 不是新 ASR 链路

VibeBoard USB PCM 通过 Vokie 已有的 `pcm-generator` 写入既有 WebRTC transport。
它只是外部 PCM 的媒体音轨适配方式，不是独立识别通道。

## 5. 当前已实现的数据链路

```text
VibeBoard 原厂固件 0.3.8
  -> USB Serial/JTAG
  -> vibeboard-helper
  -> JSON Lines + base64 Opus
  -> VibeBoardService
  -> Int16 PCM + 按键事件
  -> VoiceSessionCoordinator / PastePresenter
  -> 既有 PTT WebRTC/ASR/结果处理链路
```

当前 Vokie 实现通过 USB helper 同时接收按键状态事件和音频帧。此处是“当前实现
事实”，不代表最终控制面设计已经确定。

## 6. USB 发现与握手

Vokie 启动时自动启动 `vibeboard-helper`。helper 使用 macOS IOKit 每秒扫描一次
串口设备：

1. 匹配 USB VID/PID `303a:1001`；
2. 读取 USB Serial 并生成归一化设备 ID；
3. 发送 `transport` 与 `get_device_info`；
4. 校验 `hardware == "vibe_keyboard"`；
5. 配置 `interaction_mode = hold_to_talk`；
6. 配置 `voice_key = k4`；
7. 每两秒发送一次 ping 保持连接。

无设备时 helper 保持运行并继续扫描；USB 断开后释放活动 PTT、清理音频状态并
等待设备重新连接。用户不需要手动开启 VibeBoard 开关。

## 7. 按键映射

| VibeBoard 事件 | 当前 Vokie 行为 |
| --- | --- |
| K4 down | 启动 VibeBoard 音频源并触发现有 PTT press |
| K4 up | 停止音频源并触发现有 PTT release |
| K2 click | 发送 Enter |
| K3 click | 撤销上一次 Vokie 输出 |
| K1 click | 保留，不产生副作用 |

K2/K3 只处理 `button_click`，避免同一次物理操作的 down/up/click 被执行多次。

## 8. 音频编码能力

### 8.1 原厂 `0.3.8` 实机已确认

- 编码：Opus；
- 采样率：16 kHz；
- 声道：mono；
- 每包：960 samples；
- 包时长：60 ms；
- Vokie 解码后每包：1920 bytes PCM s16le。

早期实现错误地把每个 Opus 包固定为 320 samples/20 ms，实机报错：

```text
VibeBoard Opus decoder must return 320 samples, got 960
```

修正后允许 Opus 合法可变帧长，并用上一有效包的采样数进行丢帧静音补偿。实机
抓取的 60 ms Opus 包已成功解码为 960 samples。

### 8.2 公开兼容固件的完整编码配置

以下参数来自 `zerob13/vibe-keyboard` 的公开音频合同，不能未经实机验证就全部
归因于原厂固件：

| 参数 | 公开兼容固件配置 |
| --- | --- |
| PDM 输入 | 16 kHz、16-bit slots |
| 前处理 | Espressif AFE，输出 mono PCM |
| Opus application | `OPUS_APPLICATION_VOIP` |
| 目标码率 | 16 kbps |
| VBR | 开启 |
| DTX | 关闭 |
| complexity | 4 |
| signal | `OPUS_SIGNAL_VOICE` |
| 单包最大值 | 220 bytes |

USB 线上传输的是带 session、sequence、flags 的 Opus payload，不是 USB Audio
Class PCM。Ogg Opus 是主机保存录音时使用的封装，不是设备传输格式。

## 9. PTT 接入问题与修复

### 9.1 首页模式拦截

实机调试时首页模式为 `handsfreePtt`，普通键盘 PTT 入口会输出：

```text
ptt mode disabled
```

为硬件 K4 增加了硬件触发入口。它只绕过“键盘 PTT 是否启用”的判断，会话冲突、
独占处理、录音生命周期和结果处理仍复用 `VoiceSessionCoordinator`。

### 9.2 WebRTC 等待错误音轨

首版虽然把 USB PCM 喂给本地 recorder/ASR，但 WebRTC 仍选择系统麦克风的
`native-track`，导致：

```text
Timed out waiting for capture audio track
remote-connect-failed
```

正确处理是让 VibeBoard PTT 使用既有 `pcm-generator` 音轨，把同一份 USB PCM
写入既有 PTT WebRTC transport。修复后的诊断字段应为：

```text
mode: ptt
configuredAudioSource: vibeboard-usb
mediaMode: audio-track
trackSource: pcm-generator
```

不应再出现等待系统麦克风 capture track 的超时。

## 10. 设置页 UI

Vokie 的“设置 -> 外部设备”已设计为两个独立设备卡片：

- VibeBoard：USB 自动发现，只读展示连接状态、设备 ID、固件版本和 USB 音频；
- StopWatch：BLE 连接，保留连接/断开、设为默认、自动扫描和显示风格设置。

VibeBoard 当前实机 UI 已显示：

```text
VibeBoard · 已连接 · USB
设备 ID: VS-14C19F48594C
固件版本: 0.3.8
```

UI 明示“外设负责触发与音频采集，不会改变当前 ASR 识别链路”。

## 11. 蓝牙控制面：待验证事项

用户给出的目标架构是：

```text
控制面：VibeBoard --BLE--> Vokie
采集面：VibeBoard --USB Opus--> Vokie
识别面：Vokie PCM --既有 ASR--> 识别结果
```

但截至本记录，已提交的 Vokie 实现仍从 USB 状态帧读取 K1-K4 事件，并通过 USB
下发 `interaction_mode`、`voice_key` 和 UI 状态。因此：

- “BLE 控制 + USB 音频”尚未在 Vokie 中完成；
- 需要先实机确认原厂 `0.3.8` 广播的 BLE service/characteristic、按键 notify 和
  身份信息；
- 验证后应只替换控制事件来源，不能改变 USB 音频源和 ASR 链路；
- USB 与 BLE 断开状态需要分别处理，不能用一个连接状态互相推断。

## 12. 蓝牙音频调研结论

ESP32-S3 支持 Bluetooth 5 LE，当前 Opus 目标码率约 16 kbps，从纯带宽角度可以
通过自定义 BLE GATT Notification 或 L2CAP CoC 传输 Opus。但目前没有证据证明
原厂 `0.3.8` 已实现 BLE 音频。

同时需要区分：

- 自定义 BLE Opus：技术上可实现，需要设备固件与 Vokie 同时增加协议；
- 标准蓝牙麦克风：传统 HFP/A2DP 依赖经典蓝牙，ESP32-S3 只有 BLE，不适合直接
  作为常规系统蓝牙麦克风；
- 标准 LE Audio 使用 LC3，不是当前 Opus 路线。

因此当前稳定方案仍是 USB Opus 音频。

## 13. Vokie 代码留痕

Vokie 仓库：`/Users/hc/working/gitbfb/xiguashuo-pc`

| 项目 | 值 |
| --- | --- |
| 分支 | `feat/999.6` |
| commit | `9613c826` |
| commit message | `feat: 支持 VibeBoard USB 外设接入与设置展示` |
| 远端 | `origin/feat/999.6` |
| 推送状态 | 已推送 |

核心模块：

```text
src/main/services/vibeboard/
  native/                  # IOKit USB 发现、串口帧解析、Swift helper
  helperProcess.ts         # helper 生命周期与 JSON Lines
  protocol.ts              # 事件校验
  audioSource.ts           # sequence、丢帧补偿、PCM 输出
  wasmOpusDecoder.ts       # 16 kHz mono Opus 解码
  vibeBoardService.ts      # 按键与音频 facade
```

相关 Vokie spec：

```text
docs/superpowers/specs/2026-08-08-vibeboard-usb-integration.md
```

## 14. 验证结果

Vokie 提交前完成：

- VibeBoard 聚焦测试：5 个测试文件、12 项测试通过；
- `pnpm typecheck` 通过；
- `pnpm build` 通过；
- Swift helper 可构建；
- 原厂 `0.3.8` 实机 USB 握手成功；
- K4 PTT、USB Opus 解码、文本输入通过简单实机测试；
- 设置页实机显示设备 ID 与固件版本正确。

## 15. 本仓库版本与烧录说明

本次只在 `m5stack` 仓库新增开发记录，没有修改 `firmware/stopwatch-vokie/`，没有
生成 VibeBoard 固件版本，也没有执行任何固件烧录。因此无需更新 StopWatch 固件
版本号或 `firmware/stopwatch-vokie/CHANGELOG.md`。

