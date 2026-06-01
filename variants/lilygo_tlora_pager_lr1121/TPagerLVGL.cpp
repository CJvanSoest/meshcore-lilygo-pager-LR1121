// LVGL bridge for the T-Pager: hands draw-buffers off to the existing
// LovyanGFX panel (see TPagerST7796Display.h) and feeds encoder rotation
// + push-button events into an LVGL encoder indev so MeshCore screens
// can use the standard LVGL widgets / groups / focus model.
//
// Lifecycle: tpager_lvgl_begin() once after the LovyanGFX panel is up.
// Then call lv_timer_handler() periodically (e.g. every ~5 ms) from the
// main loop.

#include <Arduino.h>
#include <lvgl.h>
#include "TPagerST7796Display.h"
#include "TPagerLVGL.h"

// We render the full panel in two stripe buffers — a few rows tall each is
// plenty since LVGL streams them incrementally and ESP32-S3 has roomy
// internal RAM. Smaller buffers keep RAM pressure low at the cost of more
// flush calls; 40 lines * 480 wide * 2 bytes = ~37.5 KB per buffer.
#define DRAW_BUF_ROWS 40

extern LGFX_TPager *get_lvgl_panel();   // backed by TPagerST7796Display.h

static lv_display_t* s_display = NULL;
static lv_indev_t*   s_encoder = NULL;

static volatile int  s_enc_diff = 0;
static volatile bool s_enc_pressed = false;

static uint8_t s_buf1[DRAW_BUF_ROWS * 480 * 2];   // 16-bit colour
static uint8_t s_buf2[DRAW_BUF_ROWS * 480 * 2];

// LovyanGFX panel pointer (created lazily). We don't construct a separate
// device here — we piggy-back on the one DISPLAY_CLASS already owns.
static LGFX_TPager* s_panel = NULL;

static void tpager_lvgl_flush(lv_display_t* d, const lv_area_t* area, uint8_t* color_p) {
  if (s_panel == NULL) {
    lv_display_flush_ready(d);
    return;
  }
  // ST7796 wants RGB565 little-endian over SPI; LVGL gives us bytes in big
  // endian when LV_COLOR_16_SWAP=1 (and vice versa). We swap once to match
  // the panel — same trick as LilyGoLib's LV_Helper_v9.cpp.
  size_t pixels = lv_area_get_size(area);
  lv_draw_sw_rgb565_swap(color_p, pixels);

  int w = area->x2 - area->x1 + 1;
  int h = area->y2 - area->y1 + 1;
  s_panel->pushImage(area->x1, area->y1, w, h, (uint16_t*)color_p);
  lv_display_flush_ready(d);
}

static void tpager_lvgl_encoder_read(lv_indev_t* /*indev*/, lv_indev_data_t* data) {
  // Cap delta at ±1 per LVGL read so a flurry of physical detents doesn't
  // collapse into one big "jump-to-end" focus change. We drain one tick
  // per read and the rest stays buffered for the next read cycle (~30 ms
  // away thanks to LV_DEF_REFR_PERIOD), which feels natural.
  // Drop any backlog beyond ±4 — that's the visible width of the row, so
  // a fast spin can still advance across all visible tiles without ever
  // wrapping around the carousel in a single read.
  if (s_enc_diff > 4) s_enc_diff = 4;
  if (s_enc_diff < -4) s_enc_diff = -4;

  int16_t step = 0;
  if (s_enc_diff > 0) { step = 1; s_enc_diff--; }
  else if (s_enc_diff < 0) { step = -1; s_enc_diff++; }
  data->enc_diff = step;
  data->state = s_enc_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  // NOTE: continue_reading deliberately left false. With it set true LVGL
  // immediately drains the rest of the queue in one cycle, which feels
  // like a "jump to end" instead of a smooth scroll.
}

// Public input feeders — called from variant_loop or UITask poll routines.
void tpager_lvgl_encoder_delta(int delta)      { s_enc_diff += delta; }
void tpager_lvgl_encoder_pressed(bool pressed) { s_enc_pressed = pressed; }

static uint32_t tpager_lvgl_tick(void) { return (uint32_t)millis(); }

bool tpager_lvgl_begin(LGFX_TPager* panel) {
  s_panel = panel;
  lv_init();
  // LVGL's timer subsystem needs a monotonic ms source. Without this
  // lv_timer_handler() sees zero elapsed time and our indev read_cb
  // is never invoked.
  lv_tick_set_cb(tpager_lvgl_tick);

  s_display = lv_display_create(panel->width(), panel->height());
  if (s_display == NULL) return false;
  lv_display_set_user_data(s_display, panel);
  lv_display_set_flush_cb(s_display, tpager_lvgl_flush);
  lv_display_set_buffers(s_display, s_buf1, s_buf2, sizeof(s_buf1),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);

  s_encoder = lv_indev_create();
  lv_indev_set_type(s_encoder, LV_INDEV_TYPE_ENCODER);
  lv_indev_set_read_cb(s_encoder, tpager_lvgl_encoder_read);
  lv_indev_set_display(s_encoder, s_display);   // explicit — LilyGoLib does the same
  lv_indev_enable(s_encoder, true);

  return true;
}

lv_indev_t* tpager_lvgl_get_encoder() { return s_encoder; }
