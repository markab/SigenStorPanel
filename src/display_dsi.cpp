// EK79007 1024x600 IPS over 2-lane MIPI-DSI, wired to LVGL.
//
// The contract is display.h, shared with SigenStorPuck. What differs from the
// Puck's AMOLED backends:
//   - the panel scans out of a framebuffer in PSRAM that the DPI peripheral owns;
//     LVGL renders into a partial buffer and flush_cb copies each block into it
//     (GFX does the copy and the cache write-back);
//   - brightness is a PWM backlight, not a panel command;
//   - "asleep" is the backlight off. An IPS panel does not wear, so there is
//     nothing to gain from DISPOFF/SLPIN, and the framebuffer survives intact.
//
// NOT YET SEEN ON HARDWARE. Partial buffer vs direct mode on the framebuffer is
// the Phase 1 measurement in docs/PLAN.md; this is the simpler of the two.

#include "display.h"
#include "display_dsi.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>

#include "board_config.h"
#include "ui/ui_perf.h"

namespace {

Arduino_ESP32DSIPanel* s_dsi = nullptr;
Arduino_DSI_Display* s_panel = nullptr;

lv_disp_draw_buf_t s_draw_buf;
lv_disp_drv_t s_disp_drv;
lv_color_t* s_pixels = nullptr;
bool s_buffer_is_internal = false;
bool s_asleep = false;
volatile bool s_held_dark = false;
uint8_t s_brightness = PUCK_LCD_BRIGHTNESS;

void backlight_write(uint8_t level) {
  ledcWrite(PUCK_BACKLIGHT_PWM, s_held_dark ? 0 : level);
}

void flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* pixels) {
  const int32_t w = area->x2 - area->x1 + 1;
  const int32_t h = area->y2 - area->y1 + 1;
  const uint32_t panel_started = ui_perf_now_us();
  // Copies into the DPI framebuffer and writes the touched lines back out of the
  // cache, so the scan-out sees them. Rotation, if any, is applied inside GFX.
  s_panel->draw16bitRGBBitmap(area->x1, area->y1, reinterpret_cast<uint16_t*>(pixels), w, h);
  ui_perf_flush(0, static_cast<uint32_t>(w * h), 0, ui_perf_now_us() - panel_started,
                lv_disp_flush_is_last(drv));
  lv_disp_flush_ready(drv);
}

}  // namespace

bool display_begin(uint8_t rotation) {
  // A quarter turn would make this a 600x1024 portrait panel, and every screen
  // here is laid out landscape. Upside down is the only other way to hang it.
  uint8_t turn = rotation & 0x03;
  if (turn == 1 || turn == 3) {
    Serial.printf("[display] rotation %u is portrait, which this UI has no layout for; using 0\n",
                  static_cast<unsigned>(turn));
    turn = 0;
  }

  // Backlight supply first, but dark: the panel shows noise until its first frame.
  pinMode(PUCK_BACKLIGHT_POWER, OUTPUT);
  digitalWrite(PUCK_BACKLIGHT_POWER, HIGH);
  ledcAttach(PUCK_BACKLIGHT_PWM, PUCK_BACKLIGHT_PWM_HZ, 8);
  backlight_write(0);

  // The PHY LDO (channel 3, 2500 mV) is acquired inside Arduino_ESP32DSIPanel,
  // which hardcodes the same values as PUCK_DSI_PHY_LDO_*.
  s_dsi = new Arduino_ESP32DSIPanel(PUCK_DPI_HSYNC_PULSE, PUCK_DPI_HSYNC_BACK_PORCH,
                                    PUCK_DPI_HSYNC_FRONT_PORCH, PUCK_DPI_VSYNC_PULSE,
                                    PUCK_DPI_VSYNC_BACK_PORCH, PUCK_DPI_VSYNC_FRONT_PORCH,
                                    PUCK_DPI_CLOCK_MHZ * 1000000UL, PUCK_DSI_LANE_MBPS);
  // GFX ships Espressif's esp_lcd_ek79007 init sequence (Apache-2.0; see NOTICE).
  s_panel = new Arduino_DSI_Display(PUCK_LCD_WIDTH, PUCK_LCD_HEIGHT, s_dsi, turn, true,
                                    PUCK_LCD_RST, ek79007_init_operations,
                                    sizeof(ek79007_init_operations) / sizeof(lcd_init_cmd_t));
  if (!s_panel->begin()) {
    Serial.println("[display] EK79007 begin() failed: no DPI framebuffer");
    return false;
  }
  s_panel->fillScreen(RGB565_BLACK);

  const size_t pixel_count = static_cast<size_t>(PUCK_LCD_WIDTH) * PUCK_LVGL_BUFFER_LINES;
  const size_t bytes = pixel_count * sizeof(lv_color_t);

  // Internal RAM renders faster, but it is also what TLS needs, and the P4 has
  // more of it than the S3. Log which one we got so Phase 2's heap numbers can
  // be read against it.
  s_pixels = static_cast<lv_color_t*>(heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
  s_buffer_is_internal = s_pixels != nullptr;
  if (s_pixels == nullptr) {
    s_pixels = static_cast<lv_color_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
  }
  if (s_pixels == nullptr) {
    Serial.printf("[display] could not allocate a %u-byte draw buffer\n",
                  static_cast<unsigned>(bytes));
    return false;
  }

  lv_disp_draw_buf_init(&s_draw_buf, s_pixels, nullptr, pixel_count);

  lv_disp_drv_init(&s_disp_drv);
  s_disp_drv.hor_res = PUCK_LCD_WIDTH;
  s_disp_drv.ver_res = PUCK_LCD_HEIGHT;
  s_disp_drv.draw_buf = &s_draw_buf;
  s_disp_drv.flush_cb = flush_cb;
  // GFX turns the image; `rotated` is only for LVGL to turn touch to match.
  // A half turn is its own inverse, so none of the Puck's 90/270 swap applies.
  s_disp_drv.sw_rotate = 0;
  s_disp_drv.rotated = turn == 2 ? LV_DISP_ROT_180 : LV_DISP_ROT_NONE;
  lv_disp_drv_register(&s_disp_drv);

  Serial.printf("[display] EK79007 %dx%d up, rotation %u, %u-byte buffer (%u lines) in %s\n",
                PUCK_LCD_WIDTH, PUCK_LCD_HEIGHT, static_cast<unsigned>(turn),
                static_cast<unsigned>(bytes), static_cast<unsigned>(PUCK_LVGL_BUFFER_LINES),
                s_buffer_is_internal ? "internal DMA RAM" : "PSRAM");
  return true;
}

void display_set_brightness(uint8_t level) {
  s_brightness = level;
  if (s_panel != nullptr && !s_asleep) {
    backlight_write(level);
  }
}

void display_set_sleep(bool asleep) {
  if (s_panel == nullptr || asleep == s_asleep) {
    return;
  }
  s_asleep = asleep;
  // The framebuffer keeps scanning out underneath, so waking needs no repaint.
  backlight_write(asleep ? 0 : s_brightness);
  Serial.printf("[display] backlight %s\n", asleep ? "off" : "on");
}

bool display_asleep() {
  return s_asleep;
}

bool display_buffer_is_internal() {
  return s_buffer_is_internal;
}

void display_hold_dark(bool dark) {
  s_held_dark = dark;
  backlight_write(s_asleep ? 0 : s_brightness);
}

bool display_use_internal_buffer(bool internal) {
  if (s_pixels == nullptr || internal == s_buffer_is_internal) {
    return true;
  }
  const size_t pixel_count = static_cast<size_t>(PUCK_LCD_WIDTH) * PUCK_LVGL_BUFFER_LINES;
  const size_t bytes = pixel_count * sizeof(lv_color_t);
  auto* next = static_cast<lv_color_t*>(
      heap_caps_malloc(bytes, internal ? MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA : MALLOC_CAP_SPIRAM));
  if (next == nullptr) {
    Serial.printf("[display] no room to move the draw buffer to %s\n",
                  internal ? "internal RAM" : "PSRAM");
    return false;
  }
  // Safe between lv_timer_handler() calls: flush_cb copies synchronously, so no
  // flush is still reading the old buffer.
  lv_disp_draw_buf_init(&s_draw_buf, next, nullptr, pixel_count);
  heap_caps_free(s_pixels);
  s_pixels = next;
  s_buffer_is_internal = internal;
  Serial.printf("[display] draw buffer moved to %s\n", internal ? "internal DMA RAM" : "PSRAM");
  return true;
}
