#pragma once

#include <Arduino.h>
#include <M5Unified.h>

struct VokieLvglUiState {
  String status;
  String batteryLine;
  String finalText;
  bool recording = false;
  bool connected = false;
  bool authenticated = false;
  bool charging = false;
  int batteryPercent = -1;
};

bool initVokieLvglUi(M5GFX& display);
void renderVokieLvglUi(const VokieLvglUiState& state, bool force);
void tickVokieLvglUi();
