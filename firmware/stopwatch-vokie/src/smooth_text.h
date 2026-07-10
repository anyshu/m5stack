#pragma once

#include <Arduino.h>
#include <M5Unified.h>

#include "assets/vokie_smooth_text_assets.h"

void drawSmoothCenteredText(M5Canvas& canvas,
                            const VokieSmoothFont& font,
                            String text,
                            int centerX,
                            int centerY,
                            uint16_t color);

int smoothTextWidth(const VokieSmoothFont& font, const String& text);

void drawSmoothText(M5Canvas& canvas,
                    const VokieSmoothFont& font,
                    const String& text,
                    int x,
                    int centerY,
                    uint16_t color);
