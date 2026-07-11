#include <Arduino.h>
#include <M5Unified.h>
#include <NimBLEDevice.h>
#include <esp_system.h>

#include "assets/vokie_status_images.h"
#include "smooth_text.h"
#include "welcome_animation.h"

namespace {

constexpr const char* kFirmwareVersion = "0.2.1-test.20260711.1939";
constexpr const char* kDeviceName = "Vokie SW";

constexpr const char* kServiceUuid = "7b5a0000-766f-6b69-652d-73746f707731";
constexpr const char* kControlUuid = "7b5a0001-766f-6b69-652d-73746f707731";
constexpr const char* kAudioUuid = "7b5a0002-766f-6b69-652d-73746f707731";
constexpr const char* kStatusUuid = "7b5a0003-766f-6b69-652d-73746f707731";
constexpr const char* kConfigUuid = "7b5a0004-766f-6b69-652d-73746f707731";
constexpr const char* kAuthUuid = "7b5a0005-766f-6b69-652d-73746f707731";

constexpr uint32_t kRedSoftResetHoldMs = 2000;
constexpr uint32_t kBlueDoubleClickMs = 320;
constexpr uint32_t kButtonDebounceMs = 20;
constexpr uint32_t kAuthTimeoutMs = 10000;
constexpr uint32_t kDisconnectRestartDelayMs = 100;
constexpr uint32_t kMomentaryStatusMs = 2500;
constexpr uint32_t kDisplayIdleDimMs = 60000;
constexpr uint8_t kDisplayDimBrightness = 18;
constexpr int kSampleRate = 16000;
constexpr int kChannels = 1;
constexpr int kFrameSamples = 320;  // 20ms frames — best quality
constexpr int kFrameMs = 20;
constexpr int kAudioHeaderBytes = 18;
constexpr size_t kMaxAdpcmBytes = (kFrameSamples + 1) / 2;  // 160 bytes

// IMA ADPCM encoder
struct AdpcmState { int16_t prev = 0; int idx = 0; };
AdpcmState adpcm;
uint8_t adpcmBuf[kMaxAdpcmBytes];
uint8_t audioPacket[kAudioHeaderBytes + kMaxAdpcmBytes];

static constexpr int kStepTable[89] = {
  7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,
  73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,
  449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,
  2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,
  7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,
  24623,27086,29794,32767
};
static constexpr int kIdxTable[16] = {-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};

uint8_t encodeAdpcm(int16_t s) {
  int d = (int)s - (int)adpcm.prev; uint8_t n = 0;
  if (d < 0) { n = 8; d = -d; }
  int st = kStepTable[adpcm.idx], dt = st >> 3;
  if (d >= st) { n |= 4; d -= st; dt += st; }
  st >>= 1; if (d >= st) { n |= 2; d -= st; dt += st; }
  st >>= 1; if (d >= st) { n |= 1; dt += st; }
  adpcm.prev = constrain((int)adpcm.prev + ((n & 8) ? -dt : dt), -32768, 32767);
  adpcm.idx = constrain(adpcm.idx + kIdxTable[n], 0, 88);
  return n;
}

NimBLEServer* server = nullptr;
NimBLECharacteristic* controlChar = nullptr;
NimBLECharacteristic* audioChar = nullptr;
NimBLECharacteristic* statusChar = nullptr;
NimBLECharacteristic* configChar = nullptr;
NimBLECharacteristic* authChar = nullptr;

bool connected = false;
bool authenticated = false;
bool recording = false;
bool advertising = false;
uint32_t advertisingUntilMs = 0;
uint32_t pendingBlueClickMs = 0;
uint32_t connectedAtMs = 0;
uint32_t disconnectAtMs = 0;
uint32_t returnReadyAtMs = 0;
bool pendingRestartAdvertising = false;
uint32_t streamId = 1;
uint32_t audioSeq = 1;
uint32_t controlSeq = 1;
uint32_t lastUiMs = 0;
uint32_t audioFrameCount = 0;
uint32_t audioBytesSent = 0;
uint32_t micMissCount = 0;
uint32_t lastAudioLogMs = 0;
uint32_t lastAudioFrameMs = 0;
uint32_t lastUserActivityMs = 0;
uint8_t activeDisplayBrightness = 127;
bool displayDimmed = false;
bool chineseUiEnabled = true;

int16_t pcmFrame[kFrameSamples];
M5Canvas uiCanvas(&M5.Display);
M5Canvas thinkingImageCanvas(&M5.Display);
M5Canvas okImageCanvas(&M5.Display);
bool uiCanvasReady = false;
bool statusImageCanvasesReady = false;

String statusLine = "Booting";
String finalText = "";
String lastDrawnStatus = "";
String lastDrawnBatteryLine = "";
uint16_t lastDrawnStatusColor = 0;
uint8_t lastDrawnStatusFrame = 0xff;

enum class StatusImage : uint8_t {
  none,
  transcribing,
  done,
};

enum class DisplayStyle : uint8_t {
  text,
  sticker,
};

StatusImage lastDrawnStatusImage = StatusImage::none;
DisplayStyle displayStyle = DisplayStyle::sticker;

struct DebouncedButton {
  bool initialized = false;
  bool rawPressed = false;
  bool stablePressed = false;
  bool pressedEdge = false;
  bool releasedEdge = false;
  uint32_t rawChangedAtMs = 0;
};

DebouncedButton yellowButton;
DebouncedButton blueButton;

bool readYellowPressed() {
  return !m5gfx::gpio_in(GPIO_NUM_2);
}

bool readBluePressed() {
  return !m5gfx::gpio_in(GPIO_NUM_1);
}

void restoreDisplayBrightness() {
  if (!displayDimmed) return;
  M5.Display.setBrightness(activeDisplayBrightness);
  displayDimmed = false;
}

void markUserActivity() {
  lastUserActivityMs = millis();
  restoreDisplayBrightness();
}

void clearReadyTimer() {
  returnReadyAtMs = 0;
}

void setStatus(const String& status) {
  statusLine = status;
  clearReadyTimer();
  markUserActivity();
}

void showMomentaryStatus(const String& status, const uint32_t now) {
  statusLine = status;
  returnReadyAtMs = now + kMomentaryStatusMs;
  markUserActivity();
}

String normalizeRemoteState(const String& state) {
  if (state == "ASR" || state == "asr") return "Transcribing";
  if (state == "Done" || state == "done") return "Done";
  return state;
}

void updateButton(DebouncedButton& button, const bool rawPressed, const uint32_t now) {
  button.pressedEdge = false;
  button.releasedEdge = false;
  if (!button.initialized) {
    button.initialized = true;
    button.rawPressed = rawPressed;
    button.stablePressed = rawPressed;
    button.rawChangedAtMs = now;
    return;
  }
  if (button.rawPressed != rawPressed) {
    button.rawPressed = rawPressed;
    button.rawChangedAtMs = now;
  }
  if (button.stablePressed != button.rawPressed && now - button.rawChangedAtMs >= kButtonDebounceMs) {
    button.stablePressed = button.rawPressed;
    button.pressedEdge = button.stablePressed;
    button.releasedEdge = !button.stablePressed;
  }
}

int hexValue(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void appendUtf8(String& out, const uint32_t codepoint) {
  if (codepoint <= 0x7f) {
    out += static_cast<char>(codepoint);
  } else if (codepoint <= 0x7ff) {
    out += static_cast<char>(0xc0 | (codepoint >> 6));
    out += static_cast<char>(0x80 | (codepoint & 0x3f));
  } else if (codepoint <= 0xffff) {
    out += static_cast<char>(0xe0 | (codepoint >> 12));
    out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (codepoint & 0x3f));
  } else {
    out += static_cast<char>(0xf0 | (codepoint >> 18));
    out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f));
    out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (codepoint & 0x3f));
  }
}

String extractJsonString(const String& json, const char* key) {
  const String marker = String("\"") + key + "\":";
  int start = json.indexOf(marker);
  if (start < 0) return "";
  start += marker.length();
  while (start < static_cast<int>(json.length()) && isspace(json[start])) start++;
  if (start >= static_cast<int>(json.length()) || json[start] != '"') return "";
  start++;
  String out;
  for (int i = start; i < static_cast<int>(json.length()); ++i) {
    const char c = json[i];
    if (c == '"') break;
    if (c != '\\') {
      out += c;
      continue;
    }
    if (++i >= static_cast<int>(json.length())) break;
    const char escaped = json[i];
    if (escaped == 'n') out += '\n';
    else if (escaped == 'r') out += '\r';
    else if (escaped == 't') out += '\t';
    else if (escaped == 'u' && i + 4 < static_cast<int>(json.length())) {
      uint32_t codepoint = 0;
      bool valid = true;
      for (int j = 0; j < 4; ++j) {
        const int v = hexValue(json[i + 1 + j]);
        if (v < 0) {
          valid = false;
          break;
        }
        codepoint = (codepoint << 4) | static_cast<uint32_t>(v);
      }
      if (valid) {
        appendUtf8(out, codepoint);
        i += 4;
      }
    } else {
      out += escaped;
    }
  }
  return out;
}

String controlButtonJson(const char* button, const char* event) {
  return String("{\"v\":1,\"type\":\"button\",\"button\":\"") + button +
         "\",\"event\":\"" + event + "\",\"seq\":" + String(controlSeq++) + "}";
}

String batteryJson() {
  int level = M5.Power.getBatteryLevel();
  if (level < 0) level = 100;
  const bool charging = M5.Power.isCharging();
  return String("{\"v\":1,\"type\":\"battery\",\"level\":") + String(level) +
         ",\"charging\":" + (charging ? "true" : "false") +
         ",\"seq\":" + String(controlSeq++) + "}";
}

void notifyControl(const String& payload) {
  if (!connected || !authenticated || controlChar == nullptr) return;
  controlChar->setValue(reinterpret_cast<const uint8_t*>(payload.c_str()), payload.length());
  controlChar->notify();
  Serial.printf("[control] %s\n", payload.c_str());
}

String readableStatus() {
  if (recording) return "REC";
  if (statusLine == "Pairing") return "PAIR";
  if (statusLine == "BLE connected") return "BLE";
  if (statusLine == "Authenticated") return "OK";
  if (statusLine == "Disconnected") return "DISC";
  if (statusLine == "Not connected") return "NO BLE";
  if (statusLine == "Transcribing" || statusLine == "transcribing") return "TRANS";
  if (statusLine == "Recording") return "REC";
  if (statusLine == "Done" || statusLine == "done") return "DONE";
  if (statusLine == "Ready" || statusLine == "ready") return "READY";
  if (statusLine == "Idle") return "IDLE";
  return statusLine.substring(0, 6);
}

StatusImage statusImageForLine() {
  if (displayStyle != DisplayStyle::sticker) return StatusImage::none;
  if (recording) return StatusImage::none;
  if (statusLine == "Transcribing" || statusLine == "transcribing") return StatusImage::transcribing;
  if (statusLine == "Done" || statusLine == "done") return StatusImage::done;
  return StatusImage::none;
}

bool setDisplayStyle(const String& style) {
  if (style == "text") {
    displayStyle = DisplayStyle::text;
    return true;
  }
  if (style == "sticker") {
    displayStyle = DisplayStyle::sticker;
    return true;
  }
  return false;
}

uint8_t statusAnimationFrame(const StatusImage image, const uint32_t now) {
  if (image == StatusImage::none) return 0;
  constexpr uint32_t kFrameMs = 180;
  constexpr uint8_t kFrameCount = 6;
  return static_cast<uint8_t>((now / kFrameMs) % kFrameCount);
}

void drawChineseCentered(M5Canvas& canvas, const String& text, int centerY, uint16_t color, const lgfx::IFont* font, float size = 1.0f);
void drawFinalTextPreview(M5Canvas& canvas);

void initStatusImageCanvases() {
  thinkingImageCanvas.setColorDepth(16);
  okImageCanvas.setColorDepth(16);
  thinkingImageCanvas.createSprite(static_cast<int32_t>(kVokieStatusImageSourceSize),
                                   static_cast<int32_t>(kVokieStatusImageSourceSize));
  okImageCanvas.createSprite(static_cast<int32_t>(kVokieStatusImageSourceSize),
                             static_cast<int32_t>(kVokieStatusImageSourceSize));
  thinkingImageCanvas.fillScreen(WHITE);
  okImageCanvas.fillScreen(WHITE);
  thinkingImageCanvas.drawPng(kVokieThinkingPng, kVokieThinkingPngLen, 0, 0);
  okImageCanvas.drawPng(kVokieOkPng, kVokieOkPngLen, 0, 0);
  statusImageCanvasesReady = true;
}

void drawStatusImage(const StatusImage image, const uint8_t frame) {
  const uint8_t* png = image == StatusImage::transcribing ? kVokieThinkingPng : kVokieOkPng;
  const size_t pngLen = image == StatusImage::transcribing ? kVokieThinkingPngLen : kVokieOkPngLen;
  static constexpr int8_t kThinkingYOffsets[] = {0, -2, -4, -2, 0, 2};
  static constexpr float kThinkingScales[] = {0.95f, 0.96f, 0.98f, 0.96f, 0.95f, 0.96f};
  static constexpr int8_t kDoneYOffsets[] = {2, -3, -5, -2, 0, 1};
  static constexpr float kDoneScales[] = {0.90f, 0.96f, 0.98f, 0.95f, 0.96f, 0.94f};
  constexpr float kSourceImageSize = kVokieStatusImageSourceSize;
  // Stopwatch uses a round display: fit square artwork inside the circular safe area.
  constexpr float kScreenSafeFill = 0.62f;
  const float scaleX = static_cast<float>(uiCanvas.width()) / kSourceImageSize;
  const float scaleY = static_cast<float>(uiCanvas.height()) / kSourceImageSize;
  const float baseScale = (scaleX < scaleY ? scaleX : scaleY) * kScreenSafeFill;
  const float motionScale = image == StatusImage::transcribing ? kThinkingScales[frame] : kDoneScales[frame];
  const int8_t yOffset = image == StatusImage::transcribing ? kThinkingYOffsets[frame] : kDoneYOffsets[frame];
  const float scale = baseScale * motionScale;
  const int32_t imageSize = static_cast<int32_t>(kSourceImageSize * scale + 0.5f);
  const int32_t imageX = (uiCanvas.width() - imageSize) / 2;
  const int32_t imageY = (uiCanvas.height() - imageSize) / 2 + yOffset;

  uiCanvas.fillScreen(WHITE);
  if (statusImageCanvasesReady) {
    M5Canvas* imageCanvas = image == StatusImage::transcribing ? &thinkingImageCanvas : &okImageCanvas;
    imageCanvas->pushRotateZoom(&uiCanvas,
                                uiCanvas.width() / 2,
                                uiCanvas.height() / 2 + yOffset,
                                0.0f,
                                scale,
                                scale);
    if (chineseUiEnabled) {
      drawChineseCentered(uiCanvas, image == StatusImage::transcribing ? "转写中" : "完成", 72, BLACK, &fonts::efontCN_24_b, 1.5f);
      if (image == StatusImage::done) drawFinalTextPreview(uiCanvas);
    }
    return;
  }

  uiCanvas.drawPng(png, pngLen,
                   imageX,
                   imageY,
                   imageSize,
                   imageSize,
                   0,
                   0,
                   scale,
                   scale,
                   datum_t::top_left);
  if (chineseUiEnabled) {
    drawChineseCentered(uiCanvas, image == StatusImage::transcribing ? "转写中" : "完成", 72, BLACK, &fonts::efontCN_24_b, 1.5f);
    if (image == StatusImage::done) drawFinalTextPreview(uiCanvas);
  }
}


String chineseStatusText(const String& status) {
  if (status == "REC") return "录音中";
  if (status == "PAIR") return "配对中";
  if (status == "BLE") return "已连接";
  if (status == "OK") return "已认证";
  if (status == "DISC") return "已断开";
  if (status == "NO BLE") return "未连接";
  if (status == "TRANS") return "转写中";
  if (status == "DONE") return "完成";
  if (status == "READY") return "准备好了";
  if (status == "IDLE") return "空闲";
  if (status == "SEND" || status == "Send") return "发送";
  if (status == "UNDO" || status == "Undo") return "撤销";
  return status;
}

String truncateUtf8(const String& text, const uint8_t maxCodepoints) {
  String out;
  uint8_t count = 0;
  for (int i = 0; i < static_cast<int>(text.length()) && count < maxCodepoints;) {
    const uint8_t c = static_cast<uint8_t>(text[i]);
    int len = 1;
    if ((c & 0xe0) == 0xc0) len = 2;
    else if ((c & 0xf0) == 0xe0) len = 3;
    else if ((c & 0xf8) == 0xf0) len = 4;
    if (i + len > static_cast<int>(text.length())) break;
    for (int j = 0; j < len; ++j) out += text[i + j];
    i += len;
    count++;
  }
  if (static_cast<int>(out.length()) < static_cast<int>(text.length())) out += "...";
  return out;
}

void drawChineseCentered(M5Canvas& canvas, const String& text, const int centerY, const uint16_t color, const lgfx::IFont* font, const float size) {
  canvas.setFont(font);
  canvas.setTextDatum(middle_center);
  canvas.setTextSize(size);
  canvas.setTextColor(color);
  canvas.drawString(text, canvas.width() / 2, centerY);
  canvas.setTextSize(1.0f);
}

void drawChineseReadyPrompt(M5Canvas& canvas) {
  static const String kRecordText = "按住说话";
  static const String kSendText = "点击发送";
  constexpr int kSquareSize = 34;
  constexpr int kGap = 18;
  constexpr int kLineGap = 82;
  canvas.setFont(&fonts::efontCN_24_b);
  canvas.setTextSize(1.7f);
  const int textWidth = max(canvas.textWidth(kRecordText), canvas.textWidth(kSendText));
  const int groupWidth = kSquareSize + kGap + textWidth;
  const int x = (canvas.width() - groupWidth) / 2;
  const int textX = x + kSquareSize + kGap;
  const int firstY = canvas.height() / 2 - kLineGap / 2;
  const int secondY = canvas.height() / 2 + kLineGap / 2;

  canvas.fillRoundRect(x, firstY - kSquareSize / 2, kSquareSize, kSquareSize, 5, 0xffe0);
  canvas.setTextDatum(middle_left);
  canvas.setTextColor(WHITE);
  canvas.drawString(kRecordText, textX, firstY);
  canvas.fillRoundRect(x, secondY - kSquareSize / 2, kSquareSize, kSquareSize, 5, 0x04ff);
  canvas.drawString(kSendText, textX, secondY);
  canvas.setTextSize(1.0f);
}

void drawFinalTextPreview(M5Canvas& canvas) {
  if (finalText.length() == 0) return;
  const String preview = truncateUtf8(finalText, 18);
  canvas.setFont(&fonts::efontCN_16_b);
  canvas.setTextSize(1.0f);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(BLACK, WHITE);
  canvas.fillRoundRect(48, canvas.height() - 88, canvas.width() - 96, 44, 18, WHITE);
  canvas.drawString(preview, canvas.width() / 2, canvas.height() - 66);
}


void drawReadyPrompt(M5Canvas& canvas) {
  static const String kRecordText = "Press to Rec";
  static const String kSendText = "Click to Send";
  constexpr int kSquareSize = 28;
  constexpr int kGap = 16;
  constexpr int kLineGap = 58;
  const int textWidth = max(smoothTextWidth(kVokieSmoothPromptFont, kRecordText),
                            smoothTextWidth(kVokieSmoothPromptFont, kSendText));
  const int groupWidth = kSquareSize + kGap + textWidth;
  const int x = (canvas.width() - groupWidth) / 2;
  const int textX = x + kSquareSize + kGap;
  const int firstY = canvas.height() / 2 - kLineGap / 2;
  const int secondY = canvas.height() / 2 + kLineGap / 2;

  canvas.fillRoundRect(x, firstY - kSquareSize / 2, kSquareSize, kSquareSize, 5, 0xffe0);
  drawSmoothText(canvas, kVokieSmoothPromptFont, kRecordText, textX, firstY, WHITE);
  canvas.fillRoundRect(x, secondY - kSquareSize / 2, kSquareSize, kSquareSize, 5, 0x04ff);
  drawSmoothText(canvas, kVokieSmoothPromptFont, kSendText, textX, secondY, WHITE);
}

void drawUi(bool force = false) {
  const uint32_t now = millis();
  const StatusImage statusImage = statusImageForLine();
  const uint32_t refreshMs = statusImage == StatusImage::none ? 250 : 90;
  if (!force && now - lastUiMs < refreshMs) return;
  lastUiMs = now;

  const String status = readableStatus();
  const uint8_t statusFrame = statusAnimationFrame(statusImage, now);
  const int battery = M5.Power.getBatteryLevel();
  String batteryLine = String("BAT ") + (battery >= 0 ? String(battery) : String("--")) + "%";
  if (M5.Power.isCharging()) batteryLine += "+";

  const uint16_t statusColor = recording ? RED : GREEN;
  if (!force &&
      status == lastDrawnStatus &&
      batteryLine == lastDrawnBatteryLine &&
      statusColor == lastDrawnStatusColor &&
      statusImage == lastDrawnStatusImage &&
      statusFrame == lastDrawnStatusFrame) {
    return;
  }

  if (!uiCanvasReady) return;

  if (statusImage != StatusImage::none) {
    // Sticker states still use predecoded M5GFX sprites because the artwork is
    // already optimized for the round AMOLED safe area.
    drawStatusImage(statusImage, statusFrame);
  } else {
    // Draw into an offscreen canvas and push the complete frame once to avoid
    // visible black-frame flicker from clearing the physical display directly.
    uiCanvas.fillScreen(BLACK);
    if (status == "READY") {
      if (chineseUiEnabled) drawChineseReadyPrompt(uiCanvas);
      else drawReadyPrompt(uiCanvas);
    } else if (chineseUiEnabled) {
      drawChineseCentered(uiCanvas, chineseStatusText(status), uiCanvas.height() / 2 - 8, statusColor, &fonts::efontCN_24_b, 2.0f);
    } else {
      drawSmoothCenteredText(uiCanvas, kVokieSmoothStatusFont, status, uiCanvas.width() / 2, uiCanvas.height() / 2 - 10, statusColor);
    }
    drawSmoothCenteredText(uiCanvas, kVokieSmoothBatteryFont, batteryLine, uiCanvas.width() / 2, uiCanvas.height() - 42, WHITE);
  }
  uiCanvas.pushSprite(0, 0);

  lastDrawnStatus = status;
  lastDrawnBatteryLine = batteryLine;
  lastDrawnStatusColor = statusColor;
  lastDrawnStatusImage = statusImage;
  lastDrawnStatusFrame = statusFrame;
}

void startAdvertisingWindow() {
  if (recording) {
    Serial.println("[ble] WARNING: startAdvertisingWindow called during recording – ignored");
    return;
  }
  if (connected) {
    Serial.println("[ble] skip advertising: still connected");
    return;
  }

  if (advertising) {
    NimBLEDevice::stopAdvertising();
    advertising = false;
  }

  authenticated = false;
  advertising = true;
  advertisingUntilMs = 0;
  setStatus("Pairing");
  NimBLEDevice::startAdvertising();
  Serial.println("[ble] advertising started");
  drawUi(true);
}

void forceDisconnectAndRestart() {
  if (recording) {
    Serial.println("[ble] WARNING: forceDisconnectAndRestart called during recording – ignored");
    return;
  }
  if (!connected && !pendingRestartAdvertising) {
    Serial.println("[ble] force restart: not connected, restarting advertising");
    startAdvertisingWindow();
    return;
  }

  if (server != nullptr && connected) {
    Serial.println("[ble] force restart: disconnecting server");
    server->disconnect(0);
    delay(50);
  }

  connected = false;
  authenticated = false;
  recording = false;
  pendingRestartAdvertising = false;
  setStatus("Pairing");
  drawUi(true);

  startAdvertisingWindow();
}

void sendAudioPacket(const uint8_t flags, const uint8_t* payload, const int payloadLen) {
  if (!connected || !authenticated || audioChar == nullptr) return;
  if (payloadLen < 0 || payloadLen > static_cast<int>(kMaxAdpcmBytes)) return;

  audioPacket[0] = 0x56;  // 'V'
  audioPacket[1] = 0x4f;  // 'O'
  audioPacket[2] = 1;     // version
  audioPacket[3] = flags;
  memcpy(audioPacket + 4, &streamId, sizeof(streamId));
  memcpy(audioPacket + 8, &audioSeq, sizeof(audioSeq));
  const uint32_t timestampMs = (audioSeq - 1) * kFrameMs;
  memcpy(audioPacket + 12, &timestampMs, sizeof(timestampMs));
  const uint16_t payloadLength = static_cast<uint16_t>(payloadLen);
  memcpy(audioPacket + 16, &payloadLength, sizeof(payloadLength));
  if (payloadLen > 0) memcpy(audioPacket + kAudioHeaderBytes, payload, payloadLen);

  const size_t totalLen = kAudioHeaderBytes + payloadLen;
  // Use direct notify(ptr, len) to bypass setValue buffer issues.
  audioChar->notify(audioPacket, totalLen);
  audioFrameCount++;
  audioBytesSent += payloadLen;
  if (audioFrameCount <= 5 || millis() - lastAudioLogMs >= 1000) {
    lastAudioLogMs = millis();
    Serial.printf("[audio:pcm] frame=%lu seq=%lu flags=0x%02x pcmBytes=%d totalBytes=%lu\n",
                  static_cast<unsigned long>(audioFrameCount),
                  static_cast<unsigned long>(audioSeq),
                  flags,
                  payloadLen,
                  static_cast<unsigned long>(audioBytesSent));
  }
  audioSeq++;
}

void startRecording() {
  if (recording) return;
  if (!connected || !authenticated) {
    setStatus("Not connected");
    Serial.printf("[record] ignored: connected=%d authenticated=%d\n", connected, authenticated);
    drawUi(true);
    return;
  }
  Serial.printf("[record] START: connected=%d auth=%d\n", connected, authenticated);
  recording = true;
  streamId++;
  audioSeq = 1;
  audioFrameCount = 0;
  audioBytesSent = 0;
  micMissCount = 0;
  lastAudioLogMs = 0;
  lastAudioFrameMs = 0;
  finalText = "";
  adpcm.prev = 0;
  adpcm.idx = 0;
  setStatus("Recording");
  while (M5.Mic.isRecording()) M5.delay(1);
  Serial.printf("[record] start stream=%lu yellowRaw=%d blueRaw=%d\n",
                static_cast<unsigned long>(streamId),
                readYellowPressed(),
                readBluePressed());
  notifyControl(controlButtonJson("yellow", "ptt_down"));
  delay(20);
  drawUi(true);
}

void stopRecording() {
  if (!recording) return;
  recording = false;
  notifyControl(controlButtonJson("yellow", "ptt_up"));
  sendAudioPacket(0x02, nullptr, 0);
  setStatus("Transcribing");
  Serial.printf("[record] stop frames=%lu pcmBytes=%lu micMiss=%lu\n",
                static_cast<unsigned long>(audioFrameCount),
                static_cast<unsigned long>(audioBytesSent),
                static_cast<unsigned long>(micMissCount));
  drawUi(true);
}

void pumpAudio() {
  if (!recording) return;
  const uint32_t now = millis();
  if (lastAudioFrameMs != 0 && now - lastAudioFrameMs < kFrameMs) return;
  lastAudioFrameMs = now;

  if (!M5.Mic.record(pcmFrame, kFrameSamples, kSampleRate)) {
    micMissCount++;
    memset(pcmFrame, 0, sizeof(pcmFrame));
  }

  // ADPCM encode
  for (int i = 0; i < kFrameSamples; ++i) {
    uint8_t nib = encodeAdpcm(pcmFrame[i]);
    if (i & 1) adpcmBuf[i >> 1] |= (nib << 4);
    else      adpcmBuf[i >> 1] = nib & 0x0f;
  }

  const int bytes = static_cast<int>(kMaxAdpcmBytes);
  sendAudioPacket(audioSeq == 1 ? 0x01 : 0x00, adpcmBuf, bytes);
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* srv, NimBLEConnInfo& connInfo) override {
    connected = true;
    advertising = false;
    pendingRestartAdvertising = false;
    connectedAtMs = millis();
    setStatus("BLE connected");
    Serial.println("[ble] connected");
    drawUi(true);
  }

  void onDisconnect(NimBLEServer* srv, NimBLEConnInfo& connInfo, int reason) override {
    const bool wasRecording = recording;
    connected = false;
    authenticated = false;
    recording = false;
    disconnectAtMs = millis();
    pendingRestartAdvertising = true;
    setStatus("Disconnected");
    Serial.printf("[ble] disconnected reason=%d wasRecording=%d\n", reason, wasRecording);
    drawUi(true);
  }
};

class AuthCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) override {
    String body = characteristic->getValue().c_str();
    const String nonce = extractJsonString(body, "nonce");
    const String response = nonce.length() > 0 ? nonce : body;
    characteristic->setValue(response.c_str());
    authenticated = response.length() > 0;
    setStatus(authenticated ? "Ready" : "Auth failed");
    Serial.printf("[auth] challenge=%s response=%s\n", body.c_str(), response.c_str());
    drawUi(true);
  }
};

class StatusCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) override {
    String body = characteristic->getValue().c_str();
    const String type = extractJsonString(body, "type");
    if (type == "final_text") {
      finalText = extractJsonString(body, "text");
      showMomentaryStatus("Done", millis());
    } else if (type == "state") {
      const String state = normalizeRemoteState(extractJsonString(body, "state"));
      if (state == "Done") showMomentaryStatus(state, millis());
      else setStatus(state);
    } else if (type == "display_style") {
      const String style = extractJsonString(body, "style");
      if (setDisplayStyle(style)) {
        Serial.printf("[display] style=%s\n", style.c_str());
      } else {
        Serial.printf("[display] ignored unknown style=%s\n", style.c_str());
      }
    } else if (type == "error") {
      setStatus(extractJsonString(body, "message"));
    }
    Serial.printf("[status] %s\n", body.c_str());
    drawUi(true);
  }
};

void setupBle() {
  NimBLEDevice::init(kDeviceName);
  NimBLEDevice::setMTU(512);
  NimBLEDevice::setPower(ESP_PWR_LVL_P6);

  server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  NimBLEService* service = server->createService(kServiceUuid);

  controlChar = service->createCharacteristic(kControlUuid, NIMBLE_PROPERTY::NOTIFY);
  audioChar = service->createCharacteristic(kAudioUuid, NIMBLE_PROPERTY::NOTIFY);
  statusChar = service->createCharacteristic(kStatusUuid, NIMBLE_PROPERTY::WRITE);
  configChar = service->createCharacteristic(kConfigUuid, NIMBLE_PROPERTY::READ);
  authChar = service->createCharacteristic(kAuthUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);

  statusChar->setCallbacks(new StatusCallbacks());
  authChar->setCallbacks(new AuthCallbacks());

  const String config =
      String("{\"v\":1,\"device\":\"m5-stopwatch\",\"firmware\":\"") + kFirmwareVersion +
      "\",\"audio\":{\"codec\":\"adpcm\",\"sampleRate\":16000,\"channels\":1,\"frameMs\":20},"
      "\"display\":{\"styles\":[\"text\",\"sticker\"],\"defaultStyle\":\"sticker\"},"
      "\"features\":[\"button\",\"battery\",\"display\",\"display_style\",\"touch_wake\",\"pcm_mic\"]}";
  configChar->setValue(config.c_str());

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setName(kDeviceName);
  adv->addServiceUUID(kServiceUuid);
  adv->enableScanResponse(true);
  startAdvertisingWindow();
}

void handleTouchWake() {
  if (!M5.Touch.isEnabled()) return;

  const uint8_t touchCount = M5.Touch.getCount();
  if (touchCount == 0) return;

  bool touchActivity = false;
  for (uint8_t i = 0; i < touchCount; ++i) {
    const auto& touch = M5.Touch.getDetail(i);
    touchActivity = touchActivity || touch.wasPressed() || touch.wasClicked() || touch.wasReleased();
    if (displayDimmed && touch.isPressed()) {
      Serial.printf("[touch] wake display x=%d y=%d\n", touch.x, touch.y);
      markUserActivity();
      drawUi(true);
      return;
    }
  }

  if (touchActivity) {
    markUserActivity();
  }
}

void handleButtons() {
  const uint32_t now = millis();
  updateButton(yellowButton, readYellowPressed(), now);
  updateButton(blueButton, readBluePressed(), now);

  static uint8_t lastRawBits = 0xff;
  const uint8_t rawBits = (yellowButton.rawPressed ? 0x01 : 0) | (blueButton.rawPressed ? 0x02 : 0);
  if (lastRawBits != rawBits) {
    lastRawBits = rawBits;
    markUserActivity();
    Serial.printf("[button:raw] yellow=%d blue=%d m5A=%d m5B=%d pwr=%d\n",
                  yellowButton.rawPressed,
                  blueButton.rawPressed,
                  M5.BtnA.isPressed(),
                  M5.BtnB.isPressed(),
                  M5.BtnPWR.getState());
  }

  if (yellowButton.pressedEdge) {
    Serial.println("[button] yellow down");
    startRecording();
  }
  if (yellowButton.releasedEdge) {
    Serial.println("[button] yellow up");
    stopRecording();
  }

  if (blueButton.releasedEdge) {
    Serial.println("[button] blue click");
    if (pendingBlueClickMs > 0 && now - pendingBlueClickMs <= kBlueDoubleClickMs) {
      pendingBlueClickMs = 0;
      notifyControl(controlButtonJson("blue", "undo_last_output"));
      showMomentaryStatus("Undo", now);
      drawUi(true);
    } else {
      pendingBlueClickMs = now;
    }
  }
  if (pendingBlueClickMs > 0 && millis() - pendingBlueClickMs > kBlueDoubleClickMs) {
    pendingBlueClickMs = 0;
    notifyControl(controlButtonJson("blue", "send_enter"));
    showMomentaryStatus("Send", millis());
    drawUi(true);
  }

  if (M5.BtnPWR.wasClicked()) {
    Serial.println("[button] red clicked -> force restart advertising");
    forceDisconnectAndRestart();
  }

  if (M5.BtnPWR.wasHold()) {
    Serial.println("[button] red hold -> force restart advertising");
    forceDisconnectAndRestart();
  }
}

void tickMomentaryStatus() {
  if (returnReadyAtMs == 0) return;
  const uint32_t now = millis();
  if (static_cast<int32_t>(now - returnReadyAtMs) < 0) return;
  returnReadyAtMs = 0;
  if (connected && authenticated && !recording) {
    statusLine = "Ready";
    drawUi(true);
  }
}

void tickIdleBrightness() {
  if (displayDimmed || recording) return;
  if (lastUserActivityMs == 0) return;
  if (millis() - lastUserActivityMs < kDisplayIdleDimMs) return;
  M5.Display.setBrightness(kDisplayDimBrightness);
  displayDimmed = true;
}

void tickAuthTimeout() {
  if (!connected || authenticated) return;
  if (connectedAtMs == 0) return;
  if (millis() - connectedAtMs < kAuthTimeoutMs) return;
  Serial.printf("[ble] auth timeout at %lums - disconnecting\n",
                static_cast<unsigned long>(millis()));
  forceDisconnectAndRestart();
}

void tickPendingRestart() {
  if (!pendingRestartAdvertising) return;
  if (connected) {
    pendingRestartAdvertising = false;
    return;
  }
  if (millis() - disconnectAtMs < kDisconnectRestartDelayMs) return;
  pendingRestartAdvertising = false;
  startAdvertisingWindow();
}

void tickAdvertisingWindow() {
  if (!connected && !advertising && !pendingRestartAdvertising) {
    startAdvertisingWindow();
  }
}

void tickBattery() {
  static uint32_t lastBatteryMs = 0;
  if (millis() - lastBatteryMs < 10000) return;
  lastBatteryMs = millis();
  notifyControl(batteryJson());
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("[boot] firmware=%s reset_reason=%d\n", kFirmwareVersion, static_cast<int>(esp_reset_reason()));

  auto cfg = M5.config();
  M5.begin(cfg);
  m5gfx::pinMode(GPIO_NUM_1, m5gfx::pin_mode_t::input_pullup);
  m5gfx::pinMode(GPIO_NUM_2, m5gfx::pin_mode_t::input_pullup);
  M5.Display.setRotation(0);
  activeDisplayBrightness = M5.Display.getBrightness();
  if (activeDisplayBrightness == 0) activeDisplayBrightness = 127;
  markUserActivity();
  M5.Display.fillScreen(BLACK);
  uiCanvas.setColorDepth(16);
  uiCanvas.createSprite(M5.Display.width(), M5.Display.height());
  uiCanvasReady = true;
  initStatusImageCanvases();
  playWelcomeAnimation(M5.Display, uiCanvas);

  M5.Speaker.end();

  M5.Speaker.end();

  auto micCfg = M5.Mic.config();
  micCfg.magnification = 6;  // Match "good" version RMS ~1085
  M5.Mic.config(micCfg);

  if (!M5.Mic.begin()) {
    Serial.println("[mic] begin failed");
    setStatus("Mic failed");
  } else {
    Serial.println("[mic] ready");
  }

  setupBle();
  drawUi(true);
}

void loop() {
  M5.update();
  handleTouchWake();
  handleButtons();
  pumpAudio();
  tickAuthTimeout();
  tickPendingRestart();
  tickMomentaryStatus();
  tickIdleBrightness();
  tickBattery();
  tickAdvertisingWindow();
  drawUi();
  delay(1);
}
