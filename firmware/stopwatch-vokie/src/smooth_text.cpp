#include "smooth_text.h"

namespace {

const VokieSmoothGlyph* findGlyph(const VokieSmoothFont& font, const char c) {
  const uint16_t code = static_cast<uint8_t>(c);
  for (uint8_t i = 0; i < font.glyphCount; ++i) {
    if (font.glyphs[i].code == code) return &font.glyphs[i];
  }
  return nullptr;
}

uint16_t blendOnBlack(const uint16_t color, const uint8_t alpha4) {
  const uint16_t r = (((color >> 11) & 0x1f) * alpha4 + 7) / 15;
  const uint16_t g = (((color >> 5) & 0x3f) * alpha4 + 7) / 15;
  const uint16_t b = ((color & 0x1f) * alpha4 + 7) / 15;
  return (r << 11) | (g << 5) | b;
}

int textWidth(const VokieSmoothFont& font, const String& text) {
  int width = 0;
  for (size_t i = 0; i < text.length(); ++i) {
    if (const VokieSmoothGlyph* glyph = findGlyph(font, text[i])) {
      width += glyph->xAdvance;
    }
  }
  return width;
}

void drawGlyph(M5Canvas& canvas, const VokieSmoothGlyph& glyph, const int originX, const int baselineY, const uint16_t color) {
  if (glyph.width == 0 || glyph.height == 0) return;
  const uint8_t* bitmap = kVokieSmoothTextBitmap + glyph.dataOffset;
  const int startX = originX + glyph.xOffset;
  const int startY = baselineY + glyph.yOffset;
  int pixelIndex = 0;
  for (int y = 0; y < glyph.height; ++y) {
    const int py = startY + y;
    if (py < 0 || py >= canvas.height()) {
      pixelIndex += glyph.width;
      continue;
    }
    for (int x = 0; x < glyph.width; ++x, ++pixelIndex) {
      const uint8_t packed = bitmap[pixelIndex >> 1];
      const uint8_t alpha4 = (pixelIndex & 1) ? (packed & 0x0f) : (packed >> 4);
      if (alpha4 == 0) continue;
      const int px = startX + x;
      if (px < 0 || px >= canvas.width()) continue;
      canvas.drawPixel(px, py, alpha4 == 15 ? color : blendOnBlack(color, alpha4));
    }
  }
}

}  // namespace

void drawSmoothCenteredText(M5Canvas& canvas,
                            const VokieSmoothFont& font,
                            String text,
                            const int centerX,
                            const int centerY,
                            const uint16_t color) {
  text.toUpperCase();
  const int width = textWidth(font, text);
  int cursorX = centerX - width / 2;
  const int baselineY = centerY - (font.ascent + font.descent) / 2 + font.ascent;
  for (size_t i = 0; i < text.length(); ++i) {
    const VokieSmoothGlyph* glyph = findGlyph(font, text[i]);
    if (glyph == nullptr) glyph = findGlyph(font, ' ');
    if (glyph == nullptr) continue;
    drawGlyph(canvas, *glyph, cursorX, baselineY, color);
    cursorX += glyph->xAdvance;
  }
}
