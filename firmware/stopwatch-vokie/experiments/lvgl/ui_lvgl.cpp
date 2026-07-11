#include "ui_lvgl.h"

#include <lvgl.h>
#include <esp_heap_caps.h>

namespace {

M5GFX* displayRef = nullptr;
lv_disp_draw_buf_t drawBuf;
lv_color_t* buf1 = nullptr;
lv_color_t* buf2 = nullptr;
lv_disp_drv_t dispDrv;

lv_obj_t* root = nullptr;
lv_obj_t* halo = nullptr;
lv_obj_t* card = nullptr;
lv_obj_t* statusLabel = nullptr;
lv_obj_t* subLabel = nullptr;
lv_obj_t* batteryLabel = nullptr;
lv_obj_t* batteryBar = nullptr;
lv_obj_t* yellowChip = nullptr;
lv_obj_t* blueChip = nullptr;
lv_obj_t* pulseArc = nullptr;

String lastStatus;
String lastBattery;
String lastFinalText;
bool lastRecording = false;
bool lastConnected = false;
bool lastAuthenticated = false;
bool lastCharging = false;
int lastBatteryPercent = -2;
bool hasLastState = false;
uint32_t lastTickMs = 0;

lv_style_t styleRoot;
lv_style_t styleCard;
lv_style_t styleStatus;
lv_style_t styleSub;
lv_style_t styleBattery;
lv_style_t styleChip;
lv_style_t styleYellowChip;
lv_style_t styleBlueChip;
lv_style_t styleArc;

void flushDisplay(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* colorP) {
  if (displayRef == nullptr) {
    lv_disp_flush_ready(disp);
    return;
  }

  const int32_t width = area->x2 - area->x1 + 1;
  const int32_t height = area->y2 - area->y1 + 1;
  displayRef->startWrite();
  displayRef->setAddrWindow(area->x1, area->y1, width, height);
  displayRef->writePixels(reinterpret_cast<uint16_t*>(colorP), width * height, true);
  displayRef->endWrite();
  lv_disp_flush_ready(disp);
}

uint32_t statusAccent(const VokieLvglUiState& state) {
  if (state.recording) return 0xff3b30;
  if (!state.connected) return 0xffb000;
  if (!state.authenticated) return 0x3aa7ff;
  if (state.status == "TRANS" || state.status == "Transcribing") return 0x36d6ff;
  if (state.status == "DONE" || state.status == "Done") return 0x40e070;
  return 0x8cff66;
}

const char* subtitleFor(const VokieLvglUiState& state) {
  if (state.recording) return "Listening now";
  if (!state.connected) return "Hold red to pair";
  if (!state.authenticated) return "Waiting for Vokie";
  if (state.status == "TRANS") return "Turning speech into text";
  if (state.status == "DONE") return state.finalText.length() > 0 ? state.finalText.c_str() : "Saved to Vokie";
  if (state.status == "SEND") return "Send Enter";
  if (state.status == "UNDO") return "Undo last output";
  return "Yellow records - Blue sends";
}

void createStyles() {
  lv_style_init(&styleRoot);
  lv_style_set_bg_color(&styleRoot, lv_color_hex(0x07110d));
  lv_style_set_bg_grad_color(&styleRoot, lv_color_hex(0x102c28));
  lv_style_set_bg_grad_dir(&styleRoot, LV_GRAD_DIR_VER);

  lv_style_init(&styleCard);
  lv_style_set_radius(&styleCard, 42);
  lv_style_set_bg_color(&styleCard, lv_color_hex(0x101d18));
  lv_style_set_bg_opa(&styleCard, LV_OPA_90);
  lv_style_set_border_width(&styleCard, 2);
  lv_style_set_border_color(&styleCard, lv_color_hex(0x33594f));
  lv_style_set_shadow_width(&styleCard, 36);
  lv_style_set_shadow_color(&styleCard, lv_color_hex(0x06130f));
  lv_style_set_shadow_opa(&styleCard, LV_OPA_60);

  lv_style_init(&styleStatus);
  lv_style_set_text_color(&styleStatus, lv_color_hex(0xf6fff5));
  lv_style_set_text_font(&styleStatus, &lv_font_montserrat_48);
  lv_style_set_text_letter_space(&styleStatus, 1);

  lv_style_init(&styleSub);
  lv_style_set_text_color(&styleSub, lv_color_hex(0xb8cec3));
  lv_style_set_text_font(&styleSub, &lv_font_montserrat_18);
  lv_style_set_text_align(&styleSub, LV_TEXT_ALIGN_CENTER);

  lv_style_init(&styleBattery);
  lv_style_set_text_color(&styleBattery, lv_color_hex(0xe9fff6));
  lv_style_set_text_font(&styleBattery, &lv_font_montserrat_20);

  lv_style_init(&styleChip);
  lv_style_set_radius(&styleChip, 18);
  lv_style_set_bg_opa(&styleChip, LV_OPA_90);
  lv_style_set_pad_left(&styleChip, 16);
  lv_style_set_pad_right(&styleChip, 16);
  lv_style_set_pad_top(&styleChip, 8);
  lv_style_set_pad_bottom(&styleChip, 8);
  lv_style_set_text_color(&styleChip, lv_color_hex(0x07110d));
  lv_style_set_text_font(&styleChip, &lv_font_montserrat_16);

  lv_style_init(&styleYellowChip);
  lv_style_set_bg_color(&styleYellowChip, lv_color_hex(0xffdf3d));

  lv_style_init(&styleBlueChip);
  lv_style_set_bg_color(&styleBlueChip, lv_color_hex(0x4dd2ff));

  lv_style_init(&styleArc);
  lv_style_set_arc_width(&styleArc, 10);
  lv_style_set_arc_rounded(&styleArc, true);
}

void buildScreen() {
  root = lv_obj_create(nullptr);
  lv_obj_remove_style_all(root);
  lv_obj_add_style(root, &styleRoot, 0);
  lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
  lv_scr_load(root);

  halo = lv_obj_create(root);
  lv_obj_remove_style_all(halo);
  lv_obj_set_size(halo, 350, 350);
  lv_obj_set_style_radius(halo, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(halo, lv_color_hex(0x18463a), 0);
  lv_obj_set_style_bg_opa(halo, LV_OPA_40, 0);
  lv_obj_set_style_shadow_width(halo, 44, 0);
  lv_obj_set_style_shadow_color(halo, lv_color_hex(0x2bbf96), 0);
  lv_obj_set_style_shadow_opa(halo, LV_OPA_30, 0);
  lv_obj_center(halo);

  pulseArc = lv_arc_create(root);
  lv_obj_remove_style(pulseArc, nullptr, LV_PART_KNOB);
  lv_obj_set_size(pulseArc, 382, 382);
  lv_arc_set_rotation(pulseArc, 126);
  lv_arc_set_bg_angles(pulseArc, 0, 288);
  lv_arc_set_range(pulseArc, 0, 100);
  lv_arc_set_value(pulseArc, 64);
  lv_obj_add_style(pulseArc, &styleArc, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(pulseArc, lv_color_hex(0x7affbe), LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(pulseArc, LV_OPA_80, LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(pulseArc, 4, LV_PART_MAIN);
  lv_obj_set_style_arc_color(pulseArc, lv_color_hex(0x24483f), LV_PART_MAIN);
  lv_obj_center(pulseArc);

  card = lv_obj_create(root);
  lv_obj_remove_style_all(card);
  lv_obj_add_style(card, &styleCard, 0);
  lv_obj_set_size(card, 300, 244);
  lv_obj_center(card);

  statusLabel = lv_label_create(card);
  lv_obj_add_style(statusLabel, &styleStatus, 0);
  lv_label_set_text(statusLabel, "BOOT");
  lv_obj_align(statusLabel, LV_ALIGN_TOP_MID, 0, 38);

  subLabel = lv_label_create(card);
  lv_obj_add_style(subLabel, &styleSub, 0);
  lv_obj_set_width(subLabel, 238);
  lv_label_set_long_mode(subLabel, LV_LABEL_LONG_DOT);
  lv_label_set_text(subLabel, "Starting Vokie");
  lv_obj_align(subLabel, LV_ALIGN_TOP_MID, 0, 104);

  yellowChip = lv_label_create(card);
  lv_obj_add_style(yellowChip, &styleChip, 0);
  lv_obj_add_style(yellowChip, &styleYellowChip, 0);
  lv_label_set_text(yellowChip, "REC");
  lv_obj_align(yellowChip, LV_ALIGN_BOTTOM_LEFT, 34, -30);

  blueChip = lv_label_create(card);
  lv_obj_add_style(blueChip, &styleChip, 0);
  lv_obj_add_style(blueChip, &styleBlueChip, 0);
  lv_label_set_text(blueChip, "SEND");
  lv_obj_align(blueChip, LV_ALIGN_BOTTOM_RIGHT, -34, -30);

  batteryLabel = lv_label_create(root);
  lv_obj_add_style(batteryLabel, &styleBattery, 0);
  lv_label_set_text(batteryLabel, "BAT --%");
  lv_obj_align(batteryLabel, LV_ALIGN_BOTTOM_MID, 0, -38);

  batteryBar = lv_bar_create(root);
  lv_obj_set_size(batteryBar, 120, 8);
  lv_obj_set_style_radius(batteryBar, 8, 0);
  lv_obj_set_style_bg_color(batteryBar, lv_color_hex(0x173128), 0);
  lv_obj_set_style_bg_color(batteryBar, lv_color_hex(0x82ff9e), LV_PART_INDICATOR);
  lv_obj_set_style_radius(batteryBar, 8, LV_PART_INDICATOR);
  lv_bar_set_range(batteryBar, 0, 100);
  lv_bar_set_value(batteryBar, 0, LV_ANIM_OFF);
  lv_obj_align(batteryBar, LV_ALIGN_BOTTOM_MID, 0, -24);
}

bool unchanged(const VokieLvglUiState& state) {
  return hasLastState &&
         state.status == lastStatus &&
         state.batteryLine == lastBattery &&
         state.finalText == lastFinalText &&
         state.recording == lastRecording &&
         state.connected == lastConnected &&
         state.authenticated == lastAuthenticated &&
         state.charging == lastCharging &&
         state.batteryPercent == lastBatteryPercent;
}

void remember(const VokieLvglUiState& state) {
  lastStatus = state.status;
  lastBattery = state.batteryLine;
  lastFinalText = state.finalText;
  lastRecording = state.recording;
  lastConnected = state.connected;
  lastAuthenticated = state.authenticated;
  lastCharging = state.charging;
  lastBatteryPercent = state.batteryPercent;
  hasLastState = true;
}

}  // namespace

bool initVokieLvglUi(M5GFX& display) {
  displayRef = &display;
  lv_init();

  const uint32_t width = display.width();
  const uint32_t rows = 24;
  const uint32_t pixels = width * rows;
  buf1 = static_cast<lv_color_t*>(heap_caps_malloc(pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  buf2 = static_cast<lv_color_t*>(heap_caps_malloc(pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buf1 == nullptr || buf2 == nullptr) {
    buf1 = static_cast<lv_color_t*>(heap_caps_malloc(pixels * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    buf2 = static_cast<lv_color_t*>(heap_caps_malloc(pixels * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  if (buf1 == nullptr || buf2 == nullptr) return false;

  lv_disp_draw_buf_init(&drawBuf, buf1, buf2, pixels);
  lv_disp_drv_init(&dispDrv);
  dispDrv.hor_res = width;
  dispDrv.ver_res = display.height();
  dispDrv.flush_cb = flushDisplay;
  dispDrv.draw_buf = &drawBuf;
  lv_disp_drv_register(&dispDrv);

  createStyles();
  buildScreen();
  lastTickMs = millis();
  return true;
}

void tickVokieLvglUi() {
  const uint32_t now = millis();
  lv_tick_inc(now - lastTickMs);
  lastTickMs = now;
  lv_timer_handler();
}

void renderVokieLvglUi(const VokieLvglUiState& state, const bool force) {
  if (root == nullptr) return;
  if (!force && unchanged(state)) return;

  const uint32_t accent = statusAccent(state);
  lv_obj_set_style_border_color(card, lv_color_hex(accent), 0);
  lv_obj_set_style_shadow_color(halo, lv_color_hex(accent), 0);
  lv_obj_set_style_arc_color(pulseArc, lv_color_hex(accent), LV_PART_INDICATOR);
  lv_label_set_text(statusLabel, state.status.c_str());
  lv_label_set_text(subLabel, subtitleFor(state));
  lv_label_set_text(batteryLabel, state.batteryLine.c_str());
  lv_bar_set_value(batteryBar, constrain(state.batteryPercent, 0, 100), LV_ANIM_ON);

  const int pulse = state.recording ? 96 : (state.authenticated ? 68 : 42);
  lv_arc_set_value(pulseArc, pulse);
  if (state.recording) {
    lv_obj_set_style_bg_color(card, lv_color_hex(0x25110f), 0);
  } else {
    lv_obj_set_style_bg_color(card, lv_color_hex(0x101d18), 0);
  }

  remember(state);
  lv_timer_handler();
}
