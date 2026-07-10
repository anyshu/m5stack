#include "welcome_animation.h"

#include <Arduino.h>

#include "assets/vokie_boot_assets.h"
#include "assets/vokie_welcome_frames_rgb888.h"

namespace {

constexpr uint16_t kBlack = 0x0000;
constexpr uint16_t kWhite = 0xffff;
constexpr uint16_t kCyan = 0x05ff;
constexpr uint16_t kPink = 0xf81f;

M5Canvas iconOpenCanvas;
M5Canvas iconClosedCanvas;
bool bootAssetsReady = false;

uint16_t blend565(const uint16_t from, const uint16_t to, const uint8_t amount) {
  const uint8_t fr = ((from >> 11) & 0x1f) << 3;
  const uint8_t fg = ((from >> 5) & 0x3f) << 2;
  const uint8_t fb = (from & 0x1f) << 3;
  const uint8_t tr = ((to >> 11) & 0x1f) << 3;
  const uint8_t tg = ((to >> 5) & 0x3f) << 2;
  const uint8_t tb = (to & 0x1f) << 3;
  const uint8_t r = fr + ((static_cast<int>(tr) - fr) * amount) / 255;
  const uint8_t g = fg + ((static_cast<int>(tg) - fg) * amount) / 255;
  const uint8_t b = fb + ((static_cast<int>(tb) - fb) * amount) / 255;
  return ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
}

uint8_t easeInOut(const int value, const int maxValue) {
  const float t = constrain(static_cast<float>(value) / static_cast<float>(maxValue), 0.0f, 1.0f);
  const float eased = t < 0.5f ? 2.0f * t * t : 1.0f - powf(-2.0f * t + 2.0f, 2.0f) / 2.0f;
  return static_cast<uint8_t>(eased * 255.0f + 0.5f);
}

void pushFrame(M5GFX& display, M5Canvas& canvas) {
  canvas.pushSprite(0, 0);
  display.waitDisplay();
}

void drawStage(M5Canvas& canvas, const uint8_t glow = 72) {
  canvas.fillScreen(kBlack);
  const int cx = canvas.width() / 2;
  const int cy = canvas.height() / 2;
  const int maxR = min(canvas.width(), canvas.height()) / 2;
  for (int i = 0; i < 6; ++i) {
    const int r = maxR - 8 - i * 19;
    const uint16_t color = blend565(kBlack, i & 1 ? kCyan : kPink, glow / (7 + i * 2));
    canvas.drawCircle(cx, cy, r, color);
  }
}

void initBootAssets(M5Canvas& canvas) {
  if (bootAssetsReady) return;

  iconOpenCanvas.setColorDepth(16);
  iconClosedCanvas.setColorDepth(16);
  iconOpenCanvas.createSprite(240, 240);
  iconClosedCanvas.createSprite(240, 240);
  iconOpenCanvas.fillScreen(kBlack);
  iconClosedCanvas.fillScreen(kBlack);
  iconOpenCanvas.drawPng(kVokieBootIconOpenPng, kVokieBootIconOpenPngLen, 0, 0);
  iconClosedCanvas.drawPng(kVokieBootIconClosedPng, kVokieBootIconClosedPngLen, 0, 0);

  bootAssetsReady = true;
}

void drawWelcomeFrame(M5Canvas& canvas, const int frameIndex, const int yOffset = 0) {
  const int safeFrame = constrain(frameIndex, 0, kVokieWelcomeFrameCount - 1);
  const uint8_t* frame = reinterpret_cast<const uint8_t*>(pgm_read_ptr(&kVokieWelcomeFramesRgb888[safeFrame]));
  const int x = (canvas.width() - kVokieWelcomeFrameWidth) / 2;
  const int y = canvas.height() / 2 - kVokieWelcomeFrameHeight / 2 - 24 + yOffset;
  canvas.pushImage(x, y, kVokieWelcomeFrameWidth, kVokieWelcomeFrameHeight, frame, lgfx::rgb888_3Byte, static_cast<const lgfx::rgb888_t*>(nullptr));
}

void drawWelcomeStill(M5Canvas& canvas, const int yOffset = 0) {
  drawWelcomeFrame(canvas, kVokieWelcomeFrameCount - 1, yOffset);
}


void drawVokieWord(M5Canvas& canvas, const uint8_t alpha, const int y) {
  canvas.setFont(&fonts::Font4);
  canvas.setTextDatum(middle_center);
  canvas.setTextSize(1.0f);
  canvas.setTextColor(blend565(kBlack, kWhite, alpha), kBlack);
  canvas.drawString("vokie", canvas.width() / 2, y);
}

void drawLogoFrame(M5GFX& display, M5Canvas& canvas, const uint8_t alpha, const bool closedEyes, const int lift) {
  drawStage(canvas, 64);
  const int baseSize = min(canvas.width(), canvas.height()) * 56 / 100;
  const float pop = 0.88f + 0.12f * (static_cast<float>(alpha) / 255.0f);
  M5Canvas* iconCanvas = closedEyes ? &iconClosedCanvas : &iconOpenCanvas;
  if (bootAssetsReady) {
    iconCanvas->pushRotateZoom(&canvas,
                               canvas.width() / 2,
                               canvas.height() / 2 - 18 - lift,
                               0.0f,
                               (static_cast<float>(baseSize) / 240.0f) * pop,
                               (static_cast<float>(baseSize) / 240.0f) * pop);
  } else {
    const int iconSize = static_cast<int>(baseSize * pop);
    const int iconX = (canvas.width() - iconSize) / 2;
    const int iconY = canvas.height() / 2 - iconSize / 2 - 18 - lift;
    const uint8_t* png = closedEyes ? kVokieBootIconClosedPng : kVokieBootIconOpenPng;
    const size_t pngLen = closedEyes ? kVokieBootIconClosedPngLen : kVokieBootIconOpenPngLen;
    canvas.drawPng(png, pngLen, iconX, iconY, iconSize, iconSize);
  }
  drawVokieWord(canvas, alpha, canvas.height() / 2 + baseSize / 2 + 17);
  pushFrame(display, canvas);
}



}  // namespace

void playWelcomeAnimation(M5GFX& display, M5Canvas& canvas) {
  if (canvas.width() <= 0 || canvas.height() <= 0) return;
  initBootAssets(canvas);
  for (int i = 0; i < kVokieWelcomeFrameCount; ++i) {
    drawStage(canvas, 66);
    drawWelcomeFrame(canvas, i);
    pushFrame(display, canvas);
    delay(24);
  }

  delay(260);

  for (int i = 0; i <= 255; i += 17) {
    const uint8_t eased = easeInOut(i, 255);
    drawStage(canvas, 64);
    drawWelcomeStill(canvas, -static_cast<int>(eased) / 7);
    drawVokieWord(canvas, eased, canvas.height() / 2 + 58);
    pushFrame(display, canvas);
    delay(16);
  }

  for (int i = 0; i <= 255; i += 21) {
    drawLogoFrame(display, canvas, static_cast<uint8_t>(i), false, map(i, 0, 255, 18, 0));
    delay(18);
  }

  const bool blinkPattern[] = {false, false, true, false, false, true, false};
  const uint16_t delayPattern[] = {130, 110, 95, 150, 120, 90, 170};
  for (size_t i = 0; i < sizeof(blinkPattern) / sizeof(blinkPattern[0]); ++i) {
    drawLogoFrame(display, canvas, 255, blinkPattern[i], 0);
    delay(delayPattern[i]);
  }

  canvas.setFont(&fonts::Font0);
  canvas.setTextSize(1);
  delay(120);
}
