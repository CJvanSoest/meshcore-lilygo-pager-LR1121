// LVGL-based T-Pager UI. Phase 2: horizontal tile carousel rendered with
// LVGL widgets. Encoder rotates focus; clicking a tile opens a sub-screen
// (placeholder content for now). Long-press goes back to the carousel.

#include "UITask.h"
#include "TPagerLVGL.h"
#include "TPagerST7796Display.h"
#include <lvgl.h>
#include <cstring>

#ifndef AUTO_OFF_MILLIS
  #define AUTO_OFF_MILLIS  60000
#endif

#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION "v?"
#endif

// ---------- Tile definitions -------------------------------------------------

struct TileDef {
  const char* symbol;   // LV_SYMBOL_* glyph from the built-in icons font
  const char* title;
};
static const TileDef TILES[] = {
  { LV_SYMBOL_WIFI,      "Radio"    },   // ((•))
  { LV_SYMBOL_ENVELOPE,  "Messages" },   // envelope
  { LV_SYMBOL_GPS,       "Map"      },   // location pin
  { LV_SYMBOL_SETTINGS,  "Settings" },   // gear
  { LV_SYMBOL_LIST,      "Contacts" },   // bullet list
  { LV_SYMBOL_HOME,      "About"    },   // home/info
};
static const int NUM_TILES = sizeof(TILES) / sizeof(TILES[0]);

// ---------- UI state ---------------------------------------------------------

static lv_obj_t* s_root;
static lv_obj_t* s_header_label;
static lv_obj_t* s_battery_label;
static lv_obj_t* s_tile_row;
static lv_obj_t* s_tile_buttons[NUM_TILES];
static lv_obj_t* s_title_label;
static lv_obj_t* s_subscreen_root;
static lv_obj_t* s_subscreen_title;
static lv_obj_t* s_subscreen_body;
static lv_obj_t* s_radio_list;
static lv_group_t* s_group;          // carousel tiles
static lv_group_t* s_radio_group;    // Radio settings list rows
static lv_group_t* s_edit_group;     // edit popup widget
static lv_obj_t*   s_edit_popup;     // dark backdrop + content
static lv_obj_t*   s_edit_title;
static lv_obj_t*   s_edit_widget;    // spinbox / switch / etc.
static int s_active_tile = -1;
static int s_editing_idx = -1;       // which radio row is being edited (-1 = none)

// Forward declarations of helpers that touch NodePrefs (kept inside the
// file but referenced from event callbacks).
static void radio_list_populate(NodePrefs* p, int focus_row = 0);
static void radio_item_clicked(lv_event_t* e);
static void back_long_press_event(lv_event_t* e);
extern "C" void ui_apply_radio_changes() __attribute__((weak));

static int s_last_enc_a = HIGH;
static int s_prev_btn = HIGH;
static bool s_skip_next_click = false;
static unsigned long s_btn_press_at = 0;
static bool s_lvgl_ready = false;
static unsigned long s_next_tick = 0;
static unsigned long s_next_status = 0;

static UITask* s_self = nullptr;

// ---------- Style helpers ----------------------------------------------------

static lv_style_t style_tile;
static lv_style_t style_tile_focused;

static void init_styles() {
  lv_style_init(&style_tile);
  lv_style_set_radius(&style_tile, 8);
  lv_style_set_pad_all(&style_tile, 6);
  lv_style_set_border_width(&style_tile, 1);
  lv_style_set_border_color(&style_tile, lv_color_hex(0x404a55));
  lv_style_set_bg_color(&style_tile, lv_color_hex(0x1c2530));
  lv_style_set_text_color(&style_tile, lv_color_hex(0xc0c8d0));

  lv_style_init(&style_tile_focused);
  lv_style_set_border_width(&style_tile_focused, 3);
  lv_style_set_border_color(&style_tile_focused, lv_color_hex(0xFAA61A));
  lv_style_set_bg_color(&style_tile_focused, lv_color_hex(0x2b3742));
  lv_style_set_text_color(&style_tile_focused, lv_color_hex(0xFAA61A));
  // (no transform_zoom — it animated during scroll and produced a visible
  // diagonal skew. Thicker orange border is the focus indicator.)
}

// ---------- Sub-screen for a clicked tile -----------------------------------

static void enter_subscreen(int idx) {
  s_active_tile = idx;
  // Hide carousel, show subscreen container with content for the tile.
  lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(s_subscreen_root, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(s_subscreen_title, TILES[idx].title);

  // Default: simple body label visible, radio list hidden.
  lv_obj_remove_flag(s_subscreen_body, LV_OBJ_FLAG_HIDDEN);
  if (s_radio_list) lv_obj_add_flag(s_radio_list, LV_OBJ_FLAG_HIDDEN);

  // Default: encoder drives carousel group (will switch for Radio below).
  lv_indev_t* enc = tpager_lvgl_get_encoder();

  char body[160];
  body[0] = 0;
  switch (idx) {
    case 0: {  // Radio — populate the live settings list
      lv_obj_add_flag(s_subscreen_body, LV_OBJ_FLAG_HIDDEN);
      if (s_radio_list) lv_obj_remove_flag(s_radio_list, LV_OBJ_FLAG_HIDDEN);
      if (s_self && s_self->getPrefs()) radio_list_populate(s_self->getPrefs());
      if (enc && s_radio_group) lv_indev_set_group(enc, s_radio_group);
      return;   // skip the generic label assignment below
    }
    case 1:
      snprintf(body, sizeof(body), "Unread: %d\n\n(channel list: S3.4)",
               s_self ? s_self->getMsgCount() : 0);
      break;
    case 2:
      snprintf(body, sizeof(body), "GPS view\n\n(coords + sat count: S3.4)");
      break;
    case 3:
      snprintf(body, sizeof(body), "Settings\n\n(global editable list later)");
      break;
    case 4:
      snprintf(body, sizeof(body), "Contacts\n\n(heard nodes: S3.5)");
      break;
    case 5:
      snprintf(body, sizeof(body), "MeshCore T-Pager\n%s\n\nLVGL %d.%d.%d",
               FIRMWARE_VERSION, LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
      break;
  }
  lv_label_set_text(s_subscreen_body, body);
}

static void leave_subscreen() {
  // Restore carousel + its encoder group.
  lv_obj_add_flag(s_subscreen_root, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
  lv_indev_t* enc = tpager_lvgl_get_encoder();
  if (enc && s_group) lv_indev_set_group(enc, s_group);
  s_active_tile = -1;
}

// Populate the Radio sub-screen list with current NodePrefs values. Two
// labels per row (label column + right-aligned value column) so the
// values line up neatly under each other regardless of font width.
//
// 'focus_row' (default 0) is the row that gets the encoder focus after
// repopulation — pass the last-edited index when called from an apply
// path so the user stays on the row they just changed.
static void radio_list_populate(NodePrefs* p, int focus_row) {
  if (!s_radio_list || !p) return;

  lv_obj_clean(s_radio_list);
  if (s_radio_group) lv_group_remove_all_objs(s_radio_group);
  lv_obj_t* first_row = nullptr;
  lv_obj_t* target_row = nullptr;

  static const char* LABELS[] = {
    "Freq",       // MHz
    "SF",
    "BW",         // kHz
    "CR",
    "TX power",   // dBm
    "RX boost",
  };
  static const char* UNITS[]  = { " MHz", "", " kHz", "", " dBm", "" };
  char val[24];

  for (int i = 0; i < 6; i++) {
    switch (i) {
      case 0: snprintf(val, sizeof(val), "%.3f", p->freq); break;
      case 1: snprintf(val, sizeof(val), "%d", p->sf); break;
      case 2: snprintf(val, sizeof(val), "%.2f", p->bw); break;
      case 3: snprintf(val, sizeof(val), "%d", p->cr); break;
      case 4: snprintf(val, sizeof(val), "%d", p->tx_power_dbm); break;
      case 5: snprintf(val, sizeof(val), "%s", p->rx_boosted_gain ? "on" : "off"); break;
    }
    char value_text[32];
    snprintf(value_text, sizeof(value_text), "%s%s", val, UNITS[i]);

    lv_obj_t* row = lv_obj_create(s_radio_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 24);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1c2530), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x2b3742), LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(row, lv_color_hex(0xc0c8d0), 0);
    lv_obj_set_style_text_color(row, lv_color_hex(0xFAA61A), LV_STATE_FOCUSED);
    lv_obj_set_style_pad_hor(row, 8, 0);
    lv_obj_set_style_pad_ver(row, 2, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_margin_bottom(row, 2, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                LV_FLEX_ALIGN_CENTER,
                                LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICK_FOCUSABLE);

    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, LABELS[i]);

    lv_obj_t* vlb = lv_label_create(row);
    lv_label_set_text(vlb, value_text);

    lv_obj_add_event_cb(row, radio_item_clicked, LV_EVENT_CLICKED,
                        (void*)(intptr_t)i);
    lv_obj_add_event_cb(row, back_long_press_event, LV_EVENT_LONG_PRESSED, NULL);
    if (s_radio_group) lv_group_add_obj(s_radio_group, row);
    if (i == 0) first_row = row;
    if (i == focus_row) target_row = row;
  }
  // Ensure something is focused so the first click immediately registers
  // on the row under the cursor — and after a re-populate we want to land
  // back on the row the user just edited.
  lv_obj_t* focus_to = target_row ? target_row : first_row;
  if (focus_to) lv_group_focus_obj(focus_to);
}

// ---------- Edit popup -------------------------------------------------------

static void close_edit_popup_apply() {
  if (s_editing_idx < 0) return;
  NodePrefs* p = s_self ? s_self->getPrefs() : nullptr;
  if (!p) return;

  switch (s_editing_idx) {
    case 1: p->sf = lv_spinbox_get_value(s_edit_widget); break;
    case 3: p->cr = lv_spinbox_get_value(s_edit_widget); break;
    case 4: p->tx_power_dbm = (int8_t)lv_spinbox_get_value(s_edit_widget); break;
    default: break;
  }

  int idx_was = s_editing_idx;
  if (ui_apply_radio_changes) ui_apply_radio_changes();

  lv_obj_add_flag(s_edit_popup, LV_OBJ_FLAG_HIDDEN);
  lv_indev_t* enc = tpager_lvgl_get_encoder();
  if (s_edit_group) lv_group_set_editing(s_edit_group, false);
  if (s_radio_group) lv_group_set_editing(s_radio_group, false);
  if (enc && s_radio_group) lv_indev_set_group(enc, s_radio_group);
  s_editing_idx = -1;
  radio_list_populate(p, idx_was);   // restores focus on the row just edited
}

static void radio_item_clicked(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  NodePrefs* p = s_self ? s_self->getPrefs() : nullptr;
  if (!p) return;

  // RX boost (idx 5) — toggle inline, no popup.
  if (idx == 5) {
    p->rx_boosted_gain = !p->rx_boosted_gain;
    if (ui_apply_radio_changes) ui_apply_radio_changes();
    radio_list_populate(p, 5);
    return;
  }

  // Only the integer rows have a spinbox editor in S3.3 phase 2a — others
  // (freq, BW, region…) get their editors in subsequent phases.
  bool supported = (idx == 1 || idx == 3 || idx == 4);
  if (!supported) return;

  s_editing_idx = idx;
  lv_obj_remove_flag(s_edit_popup, LV_OBJ_FLAG_HIDDEN);

  if (s_edit_widget) { lv_obj_del(s_edit_widget); s_edit_widget = nullptr; }
  lv_group_remove_all_objs(s_edit_group);

  int min_v = 0, max_v = 100, init = 0, digits = 2;
  if (idx == 1) { min_v = 5;  max_v = 12; init = p->sf;           digits = 2; lv_label_set_text(s_edit_title, "Spreading Factor (SF)"); }
  if (idx == 3) { min_v = 5;  max_v = 8;  init = p->cr;           digits = 1; lv_label_set_text(s_edit_title, "Coding Rate (CR)"); }
  if (idx == 4) { min_v = -9; max_v = 22; init = p->tx_power_dbm; digits = 2; lv_label_set_text(s_edit_title, "TX Power (dBm)"); }

  s_edit_widget = lv_spinbox_create(s_edit_popup);
  lv_obj_set_width(s_edit_widget, 120);
  lv_spinbox_set_range(s_edit_widget, min_v, max_v);
  lv_spinbox_set_digit_format(s_edit_widget, digits, 0);
  lv_spinbox_set_value(s_edit_widget, init);
  // NOTE: leave the step at 1 (the default) — lv_spinbox_step_prev would
  // bump it to tens and the encoder would jump 5 → 12 → 5.
  lv_obj_align(s_edit_widget, LV_ALIGN_CENTER, 0, 0);

  lv_obj_add_event_cb(s_edit_widget, [](lv_event_t*) {
    close_edit_popup_apply();
  }, LV_EVENT_CLICKED, nullptr);

  lv_group_add_obj(s_edit_group, s_edit_widget);
  lv_indev_t* enc = tpager_lvgl_get_encoder();
  if (enc) {
    lv_indev_set_group(enc, s_edit_group);
    lv_group_focus_obj(s_edit_widget);
    lv_group_set_editing(s_edit_group, true);   // encoder rotates value
  }
}

static bool s_in_subscreen = false;

static void tile_clicked_event(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  Serial.printf("CLICK tile %d (skip=%d, in_sub=%d)\n", idx, s_skip_next_click, s_in_subscreen);
  if (s_skip_next_click) { s_skip_next_click = false; return; }
  enter_subscreen(idx);
  s_in_subscreen = true;
}

// LVGL fires CLICKED on release *after* LONG_PRESSED — we use the global
// s_skip_next_click flag (declared near the top of the file) so the
// carousel tile under the focus doesn't immediately re-open.
//
// Long-press on anything inside the sub-screen returns to the carousel.
static void back_long_press_event(lv_event_t* /*e*/) {
  Serial.printf("LONG_PRESS (in_sub=%d)\n", s_in_subscreen);
  if (s_in_subscreen) {
    leave_subscreen();
    s_in_subscreen = false;
    s_skip_next_click = true;
  }
}

// ---------- UI construction --------------------------------------------------

static void build_ui() {
  init_styles();

  lv_obj_t* scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x0e141b), LV_PART_MAIN);
  lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);

  // ---- Carousel root container ----
  s_root = lv_obj_create(scr);
  lv_obj_remove_style_all(s_root);
  lv_obj_set_size(s_root, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(s_root, lv_color_hex(0x0e141b), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);

  // Header — node name (left) + battery (right). We refresh content later.
  s_header_label = lv_label_create(s_root);
  lv_label_set_text(s_header_label, "T-Pager");
  lv_obj_set_style_text_color(s_header_label, lv_color_hex(0xFAA61A), 0);
  lv_obj_align(s_header_label, LV_ALIGN_TOP_LEFT, 6, 4);

  s_battery_label = lv_label_create(s_root);
  lv_label_set_text(s_battery_label, "");
  lv_obj_set_style_text_color(s_battery_label, lv_color_hex(0xc0c8d0), 0);
  lv_obj_align(s_battery_label, LV_ALIGN_TOP_RIGHT, -6, 4);

  // Tile row — flexbox with scroll snap so focused tile centres.
  s_tile_row = lv_obj_create(s_root);
  lv_obj_remove_style_all(s_tile_row);
  lv_obj_set_size(s_tile_row, lv_pct(100), 130);
  lv_obj_align(s_tile_row, LV_ALIGN_CENTER, 0, -6);
  lv_obj_set_flex_flow(s_tile_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(s_tile_row, LV_FLEX_ALIGN_CENTER,
                                    LV_FLEX_ALIGN_CENTER,
                                    LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(s_tile_row, 10, 0);
  lv_obj_set_scroll_dir(s_tile_row, LV_DIR_HOR);
  lv_obj_set_scroll_snap_x(s_tile_row, LV_SCROLL_SNAP_CENTER);
  lv_obj_add_flag(s_tile_row, LV_OBJ_FLAG_SCROLLABLE);

  // Tile buttons — sized for ~3 visible at a time on the 480-wide panel.
  for (int i = 0; i < NUM_TILES; i++) {
    lv_obj_t* btn = lv_button_create(s_tile_row);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 120, 120);
    lv_obj_add_style(btn, &style_tile, LV_STATE_DEFAULT);
    lv_obj_add_style(btn, &style_tile_focused, LV_STATE_FOCUSED);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER,
                                LV_FLEX_ALIGN_CENTER,
                                LV_FLEX_ALIGN_CENTER);

    lv_obj_t* icon = lv_label_create(btn);
    lv_label_set_text(icon, TILES[i].symbol);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, TILES[i].title);

    lv_obj_add_event_cb(btn, tile_clicked_event, LV_EVENT_CLICKED,
                        (void*)(intptr_t)i);

    s_tile_buttons[i] = btn;
  }

  // < > arrow hints below the row
  lv_obj_t* arrow_l = lv_label_create(s_root);
  lv_label_set_text(arrow_l, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_color(arrow_l, lv_color_hex(0x80868f), 0);
  lv_obj_align_to(arrow_l, s_tile_row, LV_ALIGN_OUT_LEFT_MID, -2, 0);

  lv_obj_t* arrow_r = lv_label_create(s_root);
  lv_label_set_text(arrow_r, LV_SYMBOL_RIGHT);
  lv_obj_set_style_text_color(arrow_r, lv_color_hex(0x80868f), 0);
  lv_obj_align_to(arrow_r, s_tile_row, LV_ALIGN_OUT_RIGHT_MID, 2, 0);

  s_title_label = lv_label_create(s_root);
  lv_label_set_text(s_title_label, TILES[0].title);
  lv_obj_set_style_text_color(s_title_label, lv_color_hex(0xffffff), 0);
  lv_obj_align_to(s_title_label, s_tile_row, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

  // Footer hint
  lv_obj_t* hint = lv_label_create(s_root);
  lv_label_set_text(hint, "rotate: scroll  click/dbl-click: open  long: back");
  lv_obj_set_style_text_color(hint, lv_color_hex(0x707880), 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);

  // ---- Sub-screen container (hidden by default) ----
  s_subscreen_root = lv_obj_create(scr);
  lv_obj_remove_style_all(s_subscreen_root);
  lv_obj_set_size(s_subscreen_root, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(s_subscreen_root, lv_color_hex(0x0e141b), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_subscreen_root, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_add_flag(s_subscreen_root, LV_OBJ_FLAG_HIDDEN);

  s_subscreen_title = lv_label_create(s_subscreen_root);
  lv_label_set_text(s_subscreen_title, "...");
  lv_obj_set_style_text_color(s_subscreen_title, lv_color_hex(0xFAA61A), 0);
  lv_obj_align(s_subscreen_title, LV_ALIGN_TOP_LEFT, 6, 4);

  s_subscreen_body = lv_label_create(s_subscreen_root);
  lv_label_set_text(s_subscreen_body, "");
  lv_obj_set_style_text_color(s_subscreen_body, lv_color_hex(0xc0c8d0), 0);
  lv_obj_set_width(s_subscreen_body, lv_pct(96));
  lv_label_set_long_mode(s_subscreen_body, LV_LABEL_LONG_WRAP);
  lv_obj_align(s_subscreen_body, LV_ALIGN_TOP_LEFT, 6, 28);

  // Radio settings list — plain container with custom 2-column rows so the
  // value column lines up. Built once, hidden by default, repopulated on
  // every sub-screen entry.
  s_radio_list = lv_obj_create(s_subscreen_root);
  lv_obj_remove_style_all(s_radio_list);
  lv_obj_set_size(s_radio_list, lv_pct(96), lv_pct(75));
  lv_obj_align(s_radio_list, LV_ALIGN_TOP_LEFT, 6, 24);
  lv_obj_set_flex_flow(s_radio_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_bg_color(s_radio_list, lv_color_hex(0x0e141b), 0);
  lv_obj_set_style_pad_all(s_radio_list, 2, 0);
  lv_obj_set_scroll_dir(s_radio_list, LV_DIR_VER);
  lv_obj_add_flag(s_radio_list, LV_OBJ_FLAG_HIDDEN);

  s_radio_group = lv_group_create();

  // Edit popup (overlay) — hidden by default, shown by radio_item_clicked.
  s_edit_popup = lv_obj_create(scr);
  lv_obj_set_size(s_edit_popup, lv_pct(80), 130);
  lv_obj_center(s_edit_popup);
  lv_obj_set_style_bg_color(s_edit_popup, lv_color_hex(0x1c2530), 0);
  lv_obj_set_style_border_width(s_edit_popup, 2, 0);
  lv_obj_set_style_border_color(s_edit_popup, lv_color_hex(0xFAA61A), 0);
  lv_obj_set_style_radius(s_edit_popup, 8, 0);
  lv_obj_set_style_text_color(s_edit_popup, lv_color_hex(0xffffff), 0);
  lv_obj_add_flag(s_edit_popup, LV_OBJ_FLAG_HIDDEN);

  s_edit_title = lv_label_create(s_edit_popup);
  lv_label_set_text(s_edit_title, "");
  lv_obj_set_style_text_color(s_edit_title, lv_color_hex(0xFAA61A), 0);
  lv_obj_align(s_edit_title, LV_ALIGN_TOP_MID, 0, 4);

  lv_obj_t* edit_hint = lv_label_create(s_edit_popup);
  lv_label_set_text(edit_hint, "rotate to change, click to save");
  lv_obj_set_style_text_color(edit_hint, lv_color_hex(0x707880), 0);
  lv_obj_align(edit_hint, LV_ALIGN_BOTTOM_MID, 0, -4);

  s_edit_group = lv_group_create();

  lv_obj_t* back_hint = lv_label_create(s_subscreen_root);
  lv_label_set_text(back_hint, LV_SYMBOL_LEFT "  long-press to return");
  lv_obj_set_style_text_color(back_hint, lv_color_hex(0x707880), 0);
  lv_obj_align(back_hint, LV_ALIGN_BOTTOM_MID, 0, -4);

  // ---- Encoder group ----
  s_group = lv_group_create();
  for (int i = 0; i < NUM_TILES; i++) {
    lv_group_add_obj(s_group, s_tile_buttons[i]);
  }
  lv_indev_t* enc = tpager_lvgl_get_encoder();
  if (enc) lv_indev_set_group(enc, s_group);
  lv_group_focus_obj(s_tile_buttons[0]);
}

static void focused_changed_cb(lv_event_t* e) {
  lv_obj_t* obj = (lv_obj_t*)lv_event_get_target(e);
  for (int i = 0; i < NUM_TILES; i++) {
    if (s_tile_buttons[i] == obj) {
      lv_label_set_text(s_title_label, TILES[i].title);
      lv_obj_scroll_to_view(obj, LV_ANIM_OFF);   // snap — no animation queue
      break;
    }
  }
}

// ---------- UITask methods ---------------------------------------------------

void UITask::begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* prefs) {
  _display = display;
  _sensors = sensors;
  _prefs = prefs;
  _auto_off = millis() + AUTO_OFF_MILLIS;
  s_self = this;

  if (_display) _display->turnOn();

  if (tpager_lvgl_begin(&tpager_lgfx_panel)) {
    s_lvgl_ready = true;
    build_ui();
    // Hook focus-changed events on each tile so the title label tracks
    // the focused tile.
    for (int i = 0; i < NUM_TILES; i++) {
      lv_obj_add_event_cb(s_tile_buttons[i], focused_changed_cb, LV_EVENT_FOCUSED, NULL);
    }
  }
}

void UITask::loop() {
#if defined(PIN_ENCODER_A) && defined(PIN_ENCODER_B)
  // Full quadrature state-table decoder. Each mechanical detent moves the
  // gray code through four transitions (00→01→11→10→00 for one direction).
  // Decoding both pin changes — not just an A-falling edge — rejects the
  // bounce that otherwise registers as "one step forward, one step back".
  // We accumulate quadrature ticks and only emit a logical step every 4
  // (= one detent).
  static const int8_t QUAD_TABLE[16] = {
     0, -1, +1,  0,
    +1,  0,  0, -1,
    -1,  0,  0, +1,
     0, +1, -1,  0
  };
  static uint8_t s_quad_prev = 3;   // both HIGH at rest
  static int8_t  s_quad_accum = 0;

  uint8_t curr = (digitalRead(PIN_ENCODER_A) << 1) | digitalRead(PIN_ENCODER_B);
  if (curr != s_quad_prev) {
    s_quad_accum += QUAD_TABLE[(s_quad_prev << 2) | curr];
    s_quad_prev = curr;

    // In a vertical list "rotate up" reading should move the focus up too —
    // invert the encoder delta while we're not on the horizontal carousel.
    int sign = s_in_subscreen ? -1 : 1;
    while (s_quad_accum >= 4)  { tpager_lvgl_encoder_delta(+1 * sign); s_quad_accum -= 4; }
    while (s_quad_accum <= -4) { tpager_lvgl_encoder_delta(-1 * sign); s_quad_accum += 4; }

    _auto_off = millis() + AUTO_OFF_MILLIS;
    if (_display && !_display->isOn()) _display->turnOn();
  }
#endif

#ifdef PIN_USER_BTN
  // Manual press/release tracking — LVGL's encoder indev does NOT raise
  // LV_EVENT_LONG_PRESSED on widgets (long-press is reserved for the
  // edit-mode transition), so we detect a held button here and call
  // leave_subscreen() directly.
  int btn = digitalRead(PIN_USER_BTN);
  if (btn != s_prev_btn) {
    if (btn == LOW) {
      s_btn_press_at = millis();
      tpager_lvgl_encoder_pressed(true);
    } else {
      unsigned long held = millis() - s_btn_press_at;
      tpager_lvgl_encoder_pressed(false);
      if (held >= 600) {
        if (s_editing_idx >= 0) {
          // Cancel edit popup without applying.
          lv_obj_add_flag(s_edit_popup, LV_OBJ_FLAG_HIDDEN);
          lv_indev_t* enc2 = tpager_lvgl_get_encoder();
          if (enc2 && s_radio_group) lv_indev_set_group(enc2, s_radio_group);
          s_editing_idx = -1;
          s_skip_next_click = true;
        } else if (s_in_subscreen) {
          leave_subscreen();
          s_in_subscreen = false;
          s_skip_next_click = true;
        }
      }
    }
    _auto_off = millis() + AUTO_OFF_MILLIS;
    if (_display && !_display->isOn()) _display->turnOn();
    s_prev_btn = btn;
  }
  // Drop the skip flag after 1 s to avoid blocking a future legitimate
  // click if LVGL didn't fire one to consume it.
  static unsigned long s_skip_set_at = 0;
  if (s_skip_next_click && s_skip_set_at == 0) s_skip_set_at = millis();
  if (s_skip_next_click && millis() - s_skip_set_at > 1000) {
    s_skip_next_click = false;
    s_skip_set_at = 0;
  }
  if (!s_skip_next_click) s_skip_set_at = 0;
#endif

  if (s_lvgl_ready && (long)(millis() - s_next_tick) >= 0) {
    s_next_tick = millis() + 5;
    lv_timer_handler();
  }

  // Periodically refresh battery + header data on the carousel + sub-screen.
  if ((long)(millis() - s_next_status) > 0) {
    s_next_status = millis() + 1000;
    if (s_battery_label) {
      uint16_t mv = getBattMilliVolts();
      char buf[16];
      snprintf(buf, sizeof(buf), "%u mV", mv);
      lv_label_set_text(s_battery_label, buf);
    }
    if (s_header_label && _prefs && _prefs->node_name[0]) {
      lv_label_set_text(s_header_label, _prefs->node_name);
    }
    // If sub-screen is showing Radio details (idx 0), refresh body
    if (s_in_subscreen && s_subscreen_body && _prefs) {
      // Quick-and-dirty: re-render the body if title matches "Radio".
      char title_now[32]; strncpy(title_now, lv_label_get_text(s_subscreen_title), sizeof(title_now));
      title_now[sizeof(title_now)-1] = 0;
      if (strcmp(title_now, "Radio") == 0) {
        char body[160];
        snprintf(body, sizeof(body),
                 "FREQ: %.3f MHz\nSF: %d   BW: %.2f\nCR: %d   TX: %d dBm\nTime UTC: %lu",
                 _prefs->freq, _prefs->sf, _prefs->bw, _prefs->cr,
                 _prefs->tx_power_dbm, (unsigned long)millis() / 1000);
        lv_label_set_text(s_subscreen_body, body);
      }
    }
  }

  if (_display && _display->isOn() && (long)(millis() - _auto_off) > 0) {
    _display->turnOff();
  }
}
