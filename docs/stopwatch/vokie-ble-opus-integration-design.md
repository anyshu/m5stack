# M5Stack StopWatch BLE ADPCM 接入 Vokie 设计

## 背景

用户希望将 M5Stack StopWatch 作为 Vokie 专用外设使用。StopWatch 固件可以按 Vokie 需求修改；日常使用优先走蓝牙通讯，有线连接主要用于固件开发、串口日志和调试。选中 StopWatch 外设后，Vokie 的默认麦克风应切换为 StopWatch 上的麦克风，后续 ASR、后处理、历史、粘贴和学习循环继续复用现有 Vokie 管线。

本方案放弃 Wi-Fi 音频链路。StopWatch 侧采集 `16kHz / mono / 16-bit PCM`，使用 **IMA ADPCM 4:1 压缩**（或原始 PCM16，按 MTU 自适应）后通过 BLE GATT 通知发送；Vokie 端接收、解码为 `PCM s16le 16k mono`，再送入现有录音/ASR 后续逻辑。

> **最终选型**：经实测，`esp32_opus` 库在 ESP32-S3 上调用 `opus_encode()` 会直接断开 BLE 连接。ADPCM 纯整数运算、零依赖、稳定可靠，320 采样 × 20ms 帧为音质最佳配置。I2S 需使用 Philips 标准格式（`left_align = false`），4D Systems GEN4-ESP32 板默认的左对齐格式会导致采样值整体偏移 1 位产生爆音。

## 目标

- StopWatch 作为 Vokie 专用 BLE 外设，可被 Vokie 发现、配对、连接和断开。
- 用户选择 StopWatch 后，Vokie 自动将默认输入切到 StopWatch 虚拟麦克风。
- StopWatch 通过 BLE 发送按键事件，支持 toggle、PTT 和取消本次录音。
- StopWatch 通过 BLE 发送 ADPCM 音频帧，Vokie 解码成 PCM 后进入现有 ASR 管线。
- Vokie 将录音状态、ASR 状态、最终识别文本和错误状态回传给 StopWatch。
- Vokie 设置页展示 StopWatch 连接状态、电量、麦克风选择状态、音频格式和固件版本。
- StopWatch 屏幕展示连接状态、电量、录音计时、识别状态和最终识别文本摘要。
- USB 保留为开发/调试通道，不作为第一版日常使用链路。

## 非目标

- 不做 Wi-Fi 配网、Wi-Fi 音频流或 SoftAP 模式。
- 不把 StopWatch 注册成系统级蓝牙麦克风。
- 不模拟键盘快捷键，不依赖焦点或系统快捷键。
- 不新增独立 ASR、后处理、历史或粘贴链路。
- 第一版不回传实时 partial ASR 文本，只回传最终文本。
- 第一版不做多设备同时连接。
- 第一版不做 OTA 固件升级。
- 音频编码优先 ADPCM（4:1），保留 PCM16 fallback，不强制单一格式。

## 用户体验

### 首次连接

1. 用户打开 Vokie 设置页的 StopWatch 区域。
2. 点击连接设备，Vokie 扫描可用 BLE 设备。
3. 用户选择 `M5Stack StopWatch`。
4. 连接成功后，Vokie 写入：
   - `defaultMicrophoneId = "vokie-stopwatch:<deviceId>"`
   - `defaultMicrophoneLabel = "M5Stack StopWatch"`
5. 设置页显示已连接、电量、固件版本、音频格式和“已设为默认输入”。
6. StopWatch 屏幕显示 Vokie 已连接、麦克风已启用和电量。

### 后续使用

- 单击主键：待机时开始 toggle 录音，录音中停止录音。
- 长按主键：按下进入 PTT 录音，松开停止。
- 双击主键：取消当前录音或中断当前识别。
- 录音时 StopWatch 显示录音计时和音量条。
- 识别中 StopWatch 显示 `Transcribing...`。
- 识别完成后 StopWatch 显示最终文本前几行。
- BLE 断开时 Vokie 回退到浏览器默认麦克风，不修改用户保存的 StopWatch 选择；下次 StopWatch 恢复连接后继续优先使用 StopWatch。

## 按键映射

| 操作 | Vokie 行为 | 说明 |
| --- | --- | --- |
| 单击主键 | toggle start / toggle stop | 适合免按住输入 |
| 长按主键按下 | ptt down | 立即开始 PTT |
| 长按主键松开 | ptt up | 停止 PTT 并进入识别 |
| 双击主键 | cancel | 取消录音、清理当前会话 |

固件负责区分单击、双击和长按，向 Vokie 发送语义事件。Vokie 不根据原始按键时间重新推断手势。

## 音频参数

| 项 | 值 |
| --- | --- |
| 采集格式 | PCM s16le |
| 采样率 | 16000 Hz |
| 声道 | mono |
| 编码格式 | IMA ADPCM (4:1) |
| 帧长 | 20 ms |
| 每帧 PCM | 320 samples / 640 bytes |
| 每帧 ADPCM | 160 bytes（压缩后） |
| 通知总长 | 178 bytes（18 header + 160 payload） |
| MTU | 512（NimBLE），Vokie 端 Web Bluetooth 自动协商 |
| Vokie 内部输出 | PCM s16le 16k mono |

固件侧使用纯整数 IMA ADPCM 编码（零依赖、零堆分配、`<Arduino.h>` 即可编译）。
Vokie 端 `stopwatchAudioSource.ts` 内置 ADPCM 解码器，直接输出 PCM 送入现有录音/ASR 管线。
同时保留 `pcm16` 和 `opus` codec 协议支持，通过 device config 协商切换。

## BLE GATT 设计

### Service

使用 Vokie 自定义服务：

```text
Service UUID: 7b5a0000-766f-6b69-652d-73746f707731
```

### Characteristics

| 名称 | UUID | 方向 | 用途 |
| --- | --- | --- | --- |
| Control | `7b5a0001-766f-6b69-652d-73746f707731` | StopWatch -> Vokie notify | 按键、电量、设备状态 |
| Audio | `7b5a0002-766f-6b69-652d-73746f707731` | StopWatch -> Vokie notify | ADPCM 音频帧 |
| Status | `7b5a0003-766f-6b69-652d-73746f707731` | Vokie -> StopWatch write | 录音状态、ASR 状态、最终文本、错误 |
| Config | `7b5a0004-766f-6b69-652d-73746f707731` | Vokie <-> StopWatch read/write | 固件版本、设备能力、音频参数 |

## 协议

### Control 消息

Control 使用 UTF-8 JSON line，单条消息必须小于协商 MTU 可承载大小；超长字段不得放入 Control。

```json
{"v":1,"type":"button","event":"toggle","seq":101}
{"v":1,"type":"button","event":"ptt_down","seq":102}
{"v":1,"type":"button","event":"ptt_up","seq":103}
{"v":1,"type":"button","event":"cancel","seq":104}
{"v":1,"type":"battery","level":86,"charging":false,"seq":105}
{"v":1,"type":"device","state":"ready","firmware":"0.1.0","seq":106}
```

Control 事件必须包含递增 `seq`。Vokie 用 `seq` 去重，重复事件忽略。

### Audio 帧

Audio 使用二进制帧：

```text
byte 0..1   magic: 0x56 0x4f
byte 2      version: 1
byte 3      flags
byte 4..7   streamId uint32 little-endian
byte 8..11  seq uint32 little-endian
byte 12..15 timestampMs uint32 little-endian
byte 16..17 payloadLength uint16 little-endian
byte 18..N  adpcm payload (160 bytes for 320-sample frame)
```

`flags` 第一版定义：

```text
0x01 = stream_start
0x02 = stream_end
0x04 = discontinuity
```

Vokie 收到 Audio 帧后：

1. 校验 magic、version、payloadLength。
2. 按 `streamId` 找到当前音频流。
3. 按 `seq` 做轻量排序和丢包检测。
4. 将 ADPCM payload 送入解码器输出 PCM。
5. 输出 PCM chunk 给当前录音会话。

丢包不做重传；实时录音优先。ADPCM 解码器按帧独立解码，丢帧不累积误差。

### Status 消息

Status 使用 UTF-8 JSON。Vokie 写入 StopWatch。

```json
{"v":1,"type":"state","state":"idle"}
{"v":1,"type":"state","state":"recording","mode":"toggle","elapsedMs":12000}
{"v":1,"type":"state","state":"transcribing"}
{"v":1,"type":"final_text","text":"今天我们讨论一下项目进度。"}
{"v":1,"type":"error","code":"asr_failed","message":"识别失败"}
```

文本回传限制：

- 第一版只回传最终文本。
- 单条 `final_text.text` 最多 120 个 UTF-8 字符。
- 超出部分由 Vokie 截断并追加省略号。
- StopWatch 不参与文本编辑，不修改 Vokie 识别结果。

### Config

Vokie 连接后读取设备能力：

```json
{
  "v":1,
  "device":"m5-stopwatch",
  "firmware":"0.2.0",
  "audio":{"codec":"adpcm","sampleRate":16000,"channels":1,"frameMs":20},
  "features":["button","battery","display","adpcm_mic"]
}
```

如果设备能力不匹配，Vokie 不把它设为默认输入，并在设置页展示明确错误。

## Vokie 架构

新增 StopWatch 相关模块，避免把外设逻辑塞进快捷键或现有麦克风 presenter。

```text
src/main/services/stopwatch/
  protocol.ts
  opusDecoder.ts
  jitterBuffer.ts
  stopwatchBleService.ts
  stopwatchAudioSource.ts

src/main/presenter/stopwatchDevicePresenter.ts

src/shared/stopwatch.ts
```

### 模块职责

- `protocol.ts`：解析和编码 Control、Audio、Status、Config 协议。支持 `opus`/`pcm16`/`adpcm` 三种 audio codec 和 `12`/`15`/`20`/`30`ms 帧长。
- `stopwatchAudioSource.ts`：实现 Vokie 内部 PCM 音频源接口。内置 IMA ADPCM 解码器（纯整数运算），`pcm16` 模式直接透传。注册 BLE 监听在解码器就绪之前，避免丢帧。
- `jitterBuffer.ts`：Opus 模式按 `seq` 排序、检测丢包（ADPCM/PCM16 模式不使用 jitter buffer）。
- `opusDecoder.ts`：封装 Opus WASM 解码（当前未启用，ADPCM 为默认）。
- `stopwatchBleService.ts`：BLE 连接状态、认证、external connection 接口。
- `stopwatchDevicePresenter.ts`：向 renderer 暴露连接状态、配对、断开、设备信息；PTT 按键路由到 voiceSessionCoordinator。
- `stopwatchWebBluetoothBridge.ts`：Web Bluetooth API 封装，renderer 侧订阅 control/audio notify，认证后自动设置 StopWatch 为默认麦克风。
- `src/shared/stopwatch.ts`：共享类型、虚拟麦克风 id 工具函数、事件 payload。

### 音频源抽象

Vokie 内部增加 PCM 音频源抽象：

```ts
interface PcmAudioSource {
  start(options: {
    onChunk: (chunk: Int16Array) => void;
    onLevel?: (level: number) => void;
    onError?: (error: Error) => void;
  }): Promise<void>;
  stop(): Promise<void>;
}
```

现有浏览器麦克风继续使用 `getUserMedia`。当 `defaultMicrophoneId` 以 `vokie-stopwatch:` 开头时，录音会话使用 `StopWatchBleAudioSource`。该音频源只负责输出 PCM，后续 ASR、后处理、粘贴、历史和学习循环不分支。

### VoiceSessionCoordinator 接入

`VoiceSessionCoordinator` 新增外部触发入口：

```ts
handleExternalVoiceTrigger(payload: {
  source: 'm5-stopwatch';
  mode: 'toggle' | 'ptt';
  action: 'press' | 'release' | 'cancel';
  timestampMs: number;
  seq: number;
}): void;
```

内部复用现有 toggle/PTT 状态机，不复制录音逻辑。StopWatch 事件不是快捷键事件，但最终应进入同一录音会话控制路径。

## 设置页设计

设置页新增 StopWatch 区域：

```text
StopWatch
状态: 已连接 / 未连接 / 正在连接 / 错误
设备: M5Stack StopWatch
电量: 86%
麦克风: 已设为默认输入
音频: BLE ADPCM 16kHz mono
固件: 0.2.0

[连接设备] [断开] [设为默认麦克风]
```

麦克风选择列表中显示：

```text
M5Stack StopWatch
连接后将优先作为 Vokie 麦克风
```

选中 StopWatch 后，配置写入 `ConfigPresenter`，并触发现有 capture 重启逻辑。

## StopWatch 屏幕设计

StopWatch 屏幕第一版使用信息密度高的状态面板：

```text
Vokie
BLE OK     BAT 86%
MIC StopWatch

Idle
```

录音中：

```text
Vokie
Recording 00:12
||||||||||----
```

识别中：

```text
Vokie
Transcribing...
```

完成：

```text
Vokie
Done
今天我们讨论一下...
```

错误：

```text
Vokie
Error
连接已断开
```

## 数据流

```text
StopWatch MEMS MIC
  -> PCM s16le 16k mono
  -> IMA ADPCM encode
  -> BLE Audio notify
  -> Vokie stopwatchBleService
  -> stopwatchAudioSource (ADPCM decode)
  -> existing recording session
  -> ASR
  -> post process
  -> paste/history/learning loop
  -> Vokie writes Status/final_text to StopWatch
```

## 错误处理

- BLE 连接失败：设置页显示失败原因，保留用户当前麦克风配置。
- 设备能力不匹配：不设为默认输入，提示固件版本或音频参数不支持。
- 音频帧校验失败：丢弃该帧，记录诊断日志。
- `seq` 跳号：记录丢包计数，继续解码后续帧。
- ADPCM 解码失败：丢弃该帧；连续失败超过阈值时停止本次 StopWatch 音频流并提示。
- 录音中断连：结束当前录音会话并提示外设断开，不自动粘贴不完整文本。
- Vokie 未授权麦克风权限：仍要求用户授权，因为 StopWatch 音频进入 Vokie 录音能力；权限缺失时不启动录音。
- StopWatch 电量低：设置页和 StopWatch 屏幕展示低电量状态，不阻断录音。
- Status 写入失败：不影响 Vokie 本地 ASR 和粘贴，仅记录诊断。

## 开发复盘

### 选型迭代

| 阶段 | 尝试 | 结果 | 根因 |
|------|------|------|------|
| 1 | Opus 编码 (`esp32_opus`) | BLE 连接秒断 | 库调用 `opus_encode()` 在 ESP32-S3 上直接崩 NimBLE |
| 2 | PCM16 原始 320 采样 | 能连，通知只收 509 字节 | BLE MTU 512 单包上限，浏览器不自动重组分包 |
| 3 | PCM16 缩小帧 200 采样 | 音质差、爆音 | 帧太小 + I2S 左对齐格式不匹配 |
| 4 | ADPCM 480 采样 | BLE 连接断 | `pumpAudio()` 局部数组 960 字节栈溢出 |
| 5 | ADPCM 200-300 采样 | 能用，爆音依旧 | I2S `left_align=true`（左对齐），该板需要 Philips 标准格式 |
| 6 | Philips I2S（改 M5Unified 库） | 爆音大幅改善 | 格式对了，但增益/帧长还需微调 |
| 7 | 320 vs 480 采样 AB 对比 | **320 最优** | 480 帧太长导致 I2S 缓冲边界跳跃，"啵啵"声明显 |
| 8 | 增益 2→4→6→8 梯度测试 | **增益 6 最佳** | RMS ~1085 不削顶，与"好版本"音质对齐 |

### 最终收敛参数

| 参数 | 值 |
|------|-----|
| I2S 格式 | Philips 标准（`left_align=false`） |
| 音频编码 | IMA ADPCM（4:1 纯整数） |
| 帧采样 | 320 samples / 20ms |
| 每帧 ADPCM | 160 bytes |
| 通知总长 | 178 bytes |
| 麦克风增益 | 6（magnification） |
| BLE MTU | 512 |

### Stopwatch 屏幕刷新

- 固件保留当前 UI 刷新节奏：非强制刷新最短间隔 250ms。
- UI 不再直接对物理屏幕执行整屏清黑后逐项绘制；改为先绘制到 `M5Canvas` 离屏缓冲，再一次性 `pushSprite(0, 0)` 到屏幕，避免清屏瞬间造成肉眼可见闪烁。
- 若状态文本、状态字号、状态颜色和电量行均未变化，跳过本次绘制，减少无意义屏幕写入。

### 遗留问题

1. **帧边界偶发跳跃**（~7.5%）：I2S DMA 缓冲区与 `M5.Mic.record()` 的对齐偏差，软件层面无法彻底消除。ADPCM 解码器按帧独立解码，单帧跳跃不会累积误差。

2. **M5Unified 库文件修改**：`Mic_Class.cpp` 中 `left_align=false` 改动在 `platformio.ini` 的 `lib_deps` 路径下（`.pio/libdeps/`），`pio run -t clean` 会丢失。后续需做成 PlatformIO 编译补丁或 fork 库。

3. **4D Systems GEN4-ESP32 非标板**：该板不是标准 M5Stack，M5Unified 无对应板级定义，麦克风 I2S 引脚靠 Arduino 框架自动检测。若换用标准 M5Stack CoreS3 等板，可移除 Philips 格式修补和左移补偿。

## 测试

- `protocol.ts` 单元测试：Control JSON 解析、Audio 二进制帧解析、Status 编码、非法版本和长度拒绝。
- `jitterBuffer.ts` 单元测试：顺序帧、乱序帧、丢帧、重复帧、stream 切换。
- `opusDecoder.ts` 单元测试：使用固定 Opus fixture 解码为 16k mono PCM（保留，当前未启用）。
- `stopwatchAudioSource.ts` 单元测试：收到 ADPCM/PCM16 帧后输出 PCM chunk，stop 后不再输出。
- `VoiceSessionCoordinator` 单元测试：外部 toggle/PTT/cancel 触发现有录音状态路径。
- 设置页测试：连接状态、电量、默认麦克风标签展示。
- 手工联调：真实 StopWatch 固件发送按钮、电量、音频和接收 final_text。
- 验证命令：`pnpm typecheck`，相关单元测试使用 `pnpm test -- <test-file>`。

## 约束

- 本次设计不涉及版本号更新。
- 提交信息如需提交必须使用中文。
- 未明确要求提交、推送前，不自动提交或推送。
- 只提交本次修改文件。
- 新增代码按协议、BLE、音频源、Presenter、UI 分文件实现，不把功能集中塞入单个文件。
