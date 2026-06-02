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
static lv_obj_t* s_dc_label;          // duty-cycle % shown between name + battery
static lv_obj_t* s_tile_row;
static lv_obj_t* s_tile_buttons[NUM_TILES];
static lv_obj_t* s_title_label;
static lv_obj_t* s_subscreen_root;
static lv_obj_t* s_subscreen_title;
static lv_obj_t* s_subscreen_body;
static lv_obj_t* s_radio_list;
static lv_obj_t* s_channel_list;     // S3.4 channels list (Messages tile)
static lv_obj_t* s_contacts_list;    // S3.5 contacts list (Contacts tile)
static lv_group_t* s_group;          // carousel tiles
static lv_group_t* s_radio_group;    // Radio settings list rows
static lv_group_t* s_channel_group;  // Channels list rows
static lv_group_t* s_contacts_group; // Contacts list rows
static unsigned long s_contacts_next_refresh = 0;
// Special s_editing_idx value used when the text-input popup is open for
// "add new channel". Out-of-range w.r.t. radio rows so existing
// per-row dispatch ignores it.
#define EDIT_IDX_NEW_CHANNEL  100

// Chat view (S3.4 step 2/3) — opened when the user clicks an existing
// channel row in the Messages tile. Single shared ring buffer keyed by
// channel_idx, plus a compose buffer that the keyboard fills.
struct ChatMsg {
  uint8_t channel_idx;
  uint32_t timestamp;   // unix seconds (0 if unknown)
  char text[96];        // includes "sender: " prefix from the wire
};
#define CHAT_RING_SIZE 32
static ChatMsg s_chat_ring[CHAT_RING_SIZE];
static int s_chat_head = 0;     // next slot to write
static int s_chat_count = 0;    // populated entries (≤ CHAT_RING_SIZE)
static bool s_chat_open = false;
static int  s_chat_channel_idx = -1;
static lv_obj_t* s_chat_history = nullptr;
static lv_obj_t* s_chat_compose = nullptr;
static lv_obj_t* s_chat_counter = nullptr;
// 121 leaves room for a NUL, so max payload is 120 chars. MeshCore's
// MAX_TEXT_LEN is 160 (10×CIPHER_BLOCK_SIZE) minus an 8-byte sender
// prefix `<name>: `, so ~120 is a safe upper bound matching the
// Tanmatsu UI.
static char s_compose_buf[121] = {0};
static int  s_compose_len = 0;
static lv_group_t* s_edit_group;     // edit popup widget
static lv_obj_t*   s_edit_popup;     // dark backdrop + content
static lv_obj_t*   s_edit_title;
static lv_obj_t*   s_edit_widget;    // spinbox / switch / etc.
static int s_active_tile = -1;
static int s_editing_idx = -1;       // which radio row is being edited (-1 = none)

// Input-test buffer for the Messages tile. Accumulates chars delivered
// from variant_loop -> ui_input_char while that tile is open, so the
// user can visually verify QWERTY + FN symbol layer + backspace without
// needing a working USB-serial monitor.
static char s_input_buf[160] = {0};
static int  s_input_len = 0;
static lv_obj_t* s_input_label = nullptr;
static bool s_input_active = false;

// Shared text-input buffer used by the Freq popup (digits + '.') AND
// the Scope popup (letters + digits + '-'). Allowed-char set is gated
// per-row in ui_input_char. '\b' pops, '\n' commits, encoder click on
// the OK widget also commits, encoder long-press cancels.
static char s_text_buf[32] = {0};
static int  s_text_len = 0;
static lv_obj_t* s_text_value_label = nullptr;

// Standard LoRa bandwidths (kHz). Cover the full SX12xx / LR11xx range so
// users can match repeaters using narrow profiles (e.g. NL "EU/UK Narrow"
// at 62.5 kHz) as well as the original wide MeshCore default (125 kHz).
static const float BW_VALUES[] = {
  7.81f, 10.42f, 15.63f, 20.83f, 31.25f, 41.67f, 62.5f, 125.0f, 250.0f, 500.0f
};
static const char* BW_OPTIONS =
  "7.81 kHz\n"
  "10.42 kHz\n"
  "15.63 kHz\n"
  "20.83 kHz\n"
  "31.25 kHz\n"
  "41.67 kHz\n"
  "62.5 kHz\n"
  "125 kHz\n"
  "250 kHz\n"
  "500 kHz";
static const int BW_COUNT = sizeof(BW_VALUES) / sizeof(BW_VALUES[0]);

// Path-hash mode → on-wire path-hash byte count (`mode + 1`). Modes 0..2
// are valid; mode 3 is reserved (CommonCLI.cpp constrains to 0..2).
static const char* PATH_HASH_OPTIONS = "1 byte (default)\n2 bytes\n3 bytes";

static int bw_value_to_index(float bw) {
  int best = 0;
  float best_d = 1e9f;
  for (int i = 0; i < BW_COUNT; i++) {
    float d = bw > BW_VALUES[i] ? bw - BW_VALUES[i] : BW_VALUES[i] - bw;
    if (d < best_d) { best_d = d; best = i; }
  }
  return best;
}

// Forward declarations of helpers that touch NodePrefs (kept inside the
// file but referenced from event callbacks).
static void radio_list_populate(NodePrefs* p, int focus_row = 0);
static void radio_item_clicked(lv_event_t* e);
static void back_long_press_event(lv_event_t* e);
static void close_edit_popup_apply();   // referenced from ui_input_char (Enter commits)
static void channels_list_populate(int focus_row = 0);
static void open_add_channel_popup();
static void chat_view_open(int channel_idx);
static void chat_view_close();
static void chat_history_render();
static void chat_compose_render();
static void chat_ring_push(uint8_t channel_idx, uint32_t timestamp, const char* text);
static void contacts_list_populate();

extern "C" void tpager_power_off() __attribute__((weak));
extern "C" void ui_apply_radio_changes() __attribute__((weak));
// Companion writes the new default-scope name + derived HMAC key into
// NodePrefs and persists. Empty name = wildcard / no scope.
extern "C" void ui_apply_default_scope(const char* name) __attribute__((weak));
// Channels (S3.4) — companion enumerates / mutates the GroupChannel table.
extern "C" int  ui_get_channel_count() __attribute__((weak));
extern "C" bool ui_get_channel_name(int idx, char* buf, int buf_size) __attribute__((weak));
extern "C" bool ui_add_hashtag_channel(const char* name) __attribute__((weak));
extern "C" bool ui_send_group_text(int channel_idx, const char* text) __attribute__((weak));

// Contacts bridges (S3.5)
struct UiContact {
  char     name[32];
  uint8_t  type;
  int32_t  gps_lat;
  int32_t  gps_lon;
  uint32_t last_advert;
  int8_t   snr_q;
  int16_t  rssi;
};
extern "C" int  ui_get_contact_count() __attribute__((weak));
extern "C" bool ui_get_contact_info(int idx, UiContact* out) __attribute__((weak));
extern "C" void ui_get_self_loc(double* lat, double* lon) __attribute__((weak));
extern "C" uint32_t ui_get_now_epoch() __attribute__((weak));
// Tenths-of-percent (0..1000) of the duty-cycle quota that has been
// consumed in the current hour-window. 0 = idle, 1000 = throttled.
extern "C" int  ui_get_duty_cycle_used_tenths() __attribute__((weak));
// Companion fills (used_seconds, max_seconds) of the current DC window.
extern "C" void ui_get_duty_cycle_seconds(int* used_s, int* max_s) __attribute__((weak));
// Battery state-of-charge from the BQ27220, 0..100. Negative = read error.
extern "C" int  tpager_battery_percent() __attribute__((weak));

// Defined in target.cpp — drains the TCA8418 FIFO and emits chars via
// ui_input_char(). The upstream companion main loop doesn't know about
// our variant, so we drive the keyboard scan from UITask::loop instead.
extern "C" void variant_loop();
// Last raw TCA8418 event (low 7 bits = key, bit 7 = press). Returned
// once then cleared — lets the freq popup show keys that don't map to
// any kb_keymap slot, so we can identify unknown special keys.
extern "C" uint8_t tpager_kb_last_raw();

// Sink for chars produced by the TCA8418 keyboard (variant_loop in
// target.cpp). Routes to whichever screen is currently capturing input:
//   * freq-editor popup  (s_editing_idx == 0)  — digits + '.' + backspace
//   * Messages tester    (s_input_active)      — anything goes
extern "C" void ui_input_char(char c) {
  // Chat compose: when a chat view is open, the keyboard feeds the
  // compose buffer instead of any popup. Enter sends via the bridge;
  // '\b' pops; everything else (printable from the QWERTY + symbol
  // layer) appends. We accept anything that's not Enter/backspace so
  // letters, digits, symbols and even '#' (FN+Space) all work.
  if (s_chat_open && s_chat_compose) {
    if (c == '\n') {
      if (s_compose_len > 0 && ui_send_group_text) {
        if (ui_send_group_text(s_chat_channel_idx, s_compose_buf)) {
          // Show our own message in the history immediately. The wire
          // payload is "<sender>: <msg>" — mirror that locally with a
          // "(me)" prefix so it visually matches received messages.
          char local[120];
          snprintf(local, sizeof(local), "(me): %s", s_compose_buf);
          chat_ring_push((uint8_t)s_chat_channel_idx, 0, local);
          chat_history_render();
        }
      }
      s_compose_buf[0] = 0;
      s_compose_len = 0;
    } else if (c == '\b') {
      if (s_compose_len > 0) { s_compose_len--; s_compose_buf[s_compose_len] = 0; }
    } else {
      if (s_compose_len < (int)sizeof(s_compose_buf) - 1) {
        s_compose_buf[s_compose_len++] = c;
        s_compose_buf[s_compose_len] = 0;
      }
    }
    chat_compose_render();
    return;
  }
  // Text-input popup: freq (idx 0), scope (idx 7), or new-channel
  // (EDIT_IDX_NEW_CHANNEL). Char filter is per-row — freq accepts
  // digits + '.', scope and new-channel accept lowercase letters +
  // digits + '-'. All accept '\b' (pop) and '\n' (commit).
  if ((s_editing_idx == 0 || s_editing_idx == 7 ||
       s_editing_idx == EDIT_IDX_NEW_CHANNEL) && s_text_value_label) {
    if (c == '\n') {
      close_edit_popup_apply();
      return;
    } else if (c == '\b') {
      if (s_text_len > 0) { s_text_len--; s_text_buf[s_text_len] = 0; }
    } else {
      bool allow;
      if (s_editing_idx == 0) {
        allow = (c >= '0' && c <= '9') || c == '.';
      } else {  // scope or new-channel
        allow = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
      }
      if (!allow) return;   // reject other chars silently
      if (s_text_len < (int)sizeof(s_text_buf) - 1) {
        s_text_buf[s_text_len++] = c;
        s_text_buf[s_text_len] = 0;
      }
    }
    lv_label_set_text(s_text_value_label, s_text_len == 0 ? "_" : s_text_buf);
    return;
  }
  // Messages input tester (S3.4 placeholder).
  if (!s_input_active || !s_input_label) return;
  if (c == '\b') {
    if (s_input_len > 0) {
      s_input_len--;
      s_input_buf[s_input_len] = 0;
    }
  } else if (s_input_len < (int)sizeof(s_input_buf) - 1) {
    s_input_buf[s_input_len++] = c;
    s_input_buf[s_input_len] = 0;
  }
  lv_label_set_text(s_input_label, s_input_len == 0
    ? "(type to test — FN + key = symbol layer, backspace deletes)"
    : s_input_buf);
}

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

  // Default: simple body label visible, all per-tile containers hidden.
  lv_obj_remove_flag(s_subscreen_body, LV_OBJ_FLAG_HIDDEN);
  if (s_radio_list)    lv_obj_add_flag(s_radio_list,    LV_OBJ_FLAG_HIDDEN);
  if (s_channel_list)  lv_obj_add_flag(s_channel_list,  LV_OBJ_FLAG_HIDDEN);
  if (s_contacts_list) lv_obj_add_flag(s_contacts_list, LV_OBJ_FLAG_HIDDEN);
  if (s_input_label)   lv_obj_add_flag(s_input_label,   LV_OBJ_FLAG_HIDDEN);
  if (s_chat_history)  lv_obj_add_flag(s_chat_history,  LV_OBJ_FLAG_HIDDEN);
  if (s_chat_compose)  lv_obj_add_flag(s_chat_compose,  LV_OBJ_FLAG_HIDDEN);
  if (s_chat_counter)  lv_obj_add_flag(s_chat_counter,  LV_OBJ_FLAG_HIDDEN);
  s_input_active = false;
  s_chat_open = false;
  s_chat_channel_idx = -1;

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
    case 1: {  // Messages → S3.4 channels list
      lv_obj_add_flag(s_subscreen_body, LV_OBJ_FLAG_HIDDEN);
      if (s_channel_list) lv_obj_remove_flag(s_channel_list, LV_OBJ_FLAG_HIDDEN);
      channels_list_populate(0);
      if (enc && s_channel_group) lv_indev_set_group(enc, s_channel_group);
      return;
    }
    case 2:
      snprintf(body, sizeof(body), "GPS view\n\n(coords + sat count: S3.4)");
      break;
    case 3:
      snprintf(body, sizeof(body), "Settings\n\n(global editable list later)");
      break;
    case 4: {  // Contacts → S3.5 list of heard nodes
      lv_obj_add_flag(s_subscreen_body, LV_OBJ_FLAG_HIDDEN);
      if (s_contacts_list) lv_obj_remove_flag(s_contacts_list, LV_OBJ_FLAG_HIDDEN);
      contacts_list_populate();
      s_contacts_next_refresh = millis() + 5000;
      if (enc && s_contacts_group) lv_indev_set_group(enc, s_contacts_group);
      return;
    }
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
  // Tear down per-tile widgets so they don't bleed into other sub-screens.
  s_input_active = false;
  s_chat_open = false;
  s_chat_channel_idx = -1;
  if (s_input_label)    lv_obj_add_flag(s_input_label,    LV_OBJ_FLAG_HIDDEN);
  if (s_channel_list)   lv_obj_add_flag(s_channel_list,   LV_OBJ_FLAG_HIDDEN);
  if (s_contacts_list)  lv_obj_add_flag(s_contacts_list,  LV_OBJ_FLAG_HIDDEN);
  if (s_chat_history)   lv_obj_add_flag(s_chat_history,   LV_OBJ_FLAG_HIDDEN);
  if (s_chat_compose)   lv_obj_add_flag(s_chat_compose,   LV_OBJ_FLAG_HIDDEN);
  if (s_chat_counter)   lv_obj_add_flag(s_chat_counter,   LV_OBJ_FLAG_HIDDEN);
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
    "Path hash",  // bytes (mode+1)
    "Scope",      // default region-scope name (e.g. nl-utrecht)
  };
  static const char* UNITS[]  = { " MHz", "", " kHz", "", " dBm", "", "", "" };
  const int ROW_COUNT = sizeof(LABELS) / sizeof(LABELS[0]);
  char val[24];

  for (int i = 0; i < ROW_COUNT; i++) {
    switch (i) {
      case 0: snprintf(val, sizeof(val), "%.3f", p->freq); break;
      case 1: snprintf(val, sizeof(val), "%d", p->sf); break;
      case 2: snprintf(val, sizeof(val), "%.2f", p->bw); break;
      case 3: snprintf(val, sizeof(val), "%d", p->cr); break;
      case 4: snprintf(val, sizeof(val), "%d", p->tx_power_dbm); break;
      case 5: snprintf(val, sizeof(val), "%s", p->rx_boosted_gain ? "on" : "off"); break;
      case 6: snprintf(val, sizeof(val), "%d B", p->path_hash_mode + 1); break;
      case 7: {
        if (p->default_scope_name[0]) snprintf(val, sizeof(val), "%s", p->default_scope_name);
        else snprintf(val, sizeof(val), "(none)");
        break;
      }
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
    // Thin bottom separator for readability between rows. Width-1 only on
    // the bottom side so the focused-state bg pill still looks isolated.
    lv_obj_set_style_border_color(row, lv_color_hex(0x303a47), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                LV_FLEX_ALIGN_CENTER,
                                LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    // The radio list is taller than the visible viewport (7 rows × ~26 px
    // each ≈ 182 px vs ~166 px of container). LVGL doesn't auto-scroll on
    // focus by default — add this flag so the focused row is always
    // scrolled into view, otherwise the bottom rows (Path hash) stay
    // off-screen and the user can't see what's focused.
    lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);

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

// ---------- Channels list (Messages tile, S3.4 step 1) ----------------------

// Row 0 is always "+ Add channel". Subsequent rows mirror the channels
// reported by ui_get_channel_count / ui_get_channel_name. Clicking a row
// either opens the add popup or (later, step 2) the chat view.

static void channel_row_clicked(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0) {
    open_add_channel_popup();
    return;
  }
  chat_view_open(idx);
}

static void channels_list_populate(int focus_row) {
  if (!s_channel_list) return;
  lv_obj_clean(s_channel_list);
  if (s_channel_group) lv_group_remove_all_objs(s_channel_group);

  auto build_row = [&](const char* text, int data, bool is_add) -> lv_obj_t* {
    lv_obj_t* row = lv_obj_create(s_channel_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 24);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1c2530), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x2b3742), LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(row,
        is_add ? lv_color_hex(0xFAA61A) : lv_color_hex(0xc0c8d0), 0);
    lv_obj_set_style_text_color(row, lv_color_hex(0xFAA61A), LV_STATE_FOCUSED);
    lv_obj_set_style_pad_hor(row, 8, 0);
    lv_obj_set_style_pad_ver(row, 2, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_margin_bottom(row, 2, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x303a47), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);

    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);

    lv_obj_add_event_cb(row, channel_row_clicked, LV_EVENT_CLICKED,
                        (void*)(intptr_t)data);
    lv_obj_add_event_cb(row, back_long_press_event, LV_EVENT_LONG_PRESSED, NULL);
    if (s_channel_group) lv_group_add_obj(s_channel_group, row);
    return row;
  };

  lv_obj_t* first = build_row("+ Add channel", -1, true);

  int count = ui_get_channel_count ? ui_get_channel_count() : 0;
  char name[40];
  lv_obj_t* target = nullptr;
  for (int i = 0; i < count; i++) {
    if (ui_get_channel_name && ui_get_channel_name(i, name, sizeof(name))) {
      char with_hash[42];
      // "Public" is the only non-hashtag channel (hardcoded PSK). Show
      // it as-is. Everything else gets a "#" prefix for readability.
      if (strcmp(name, "Public") == 0) {
        snprintf(with_hash, sizeof(with_hash), "%s", name);
      } else {
        snprintf(with_hash, sizeof(with_hash), "#%s", name);
      }
      lv_obj_t* r = build_row(with_hash, i, false);
      if (i + 1 == focus_row) target = r;
    }
  }
  lv_group_focus_obj(target ? target : first);
}

static void open_add_channel_popup() {
  s_editing_idx = EDIT_IDX_NEW_CHANNEL;
  lv_obj_remove_flag(s_edit_popup, LV_OBJ_FLAG_HIDDEN);
  if (s_edit_widget) { lv_obj_del(s_edit_widget); s_edit_widget = nullptr; }
  if (s_text_value_label) { lv_obj_del(s_text_value_label); s_text_value_label = nullptr; }
  lv_group_remove_all_objs(s_edit_group);

  s_text_buf[0] = 0;
  s_text_len = 0;
  lv_label_set_text(s_edit_title, "New channel (hashtag)");

  s_text_value_label = lv_label_create(s_edit_popup);
  lv_obj_set_style_text_color(s_text_value_label, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_text_font(s_text_value_label, &lv_font_montserrat_24, 0);
  lv_label_set_text(s_text_value_label, "_");
  lv_obj_align(s_text_value_label, LV_ALIGN_CENTER, 0, -12);

  s_edit_widget = lv_button_create(s_edit_popup);
  lv_obj_set_size(s_edit_widget, 60, 24);
  lv_obj_align(s_edit_widget, LV_ALIGN_BOTTOM_MID, 0, -20);
  lv_obj_t* okl = lv_label_create(s_edit_widget);
  lv_label_set_text(okl, "OK");
  lv_obj_center(okl);
  lv_obj_add_event_cb(s_edit_widget, [](lv_event_t*) {
    close_edit_popup_apply();
  }, LV_EVENT_CLICKED, nullptr);

  lv_group_add_obj(s_edit_group, s_edit_widget);
  lv_indev_t* enc = tpager_lvgl_get_encoder();
  if (enc) {
    lv_indev_set_group(enc, s_edit_group);
    lv_group_focus_obj(s_edit_widget);
    lv_group_set_editing(s_edit_group, true);
  }
}

// ---------- Chat view (S3.4 step 2/3) ---------------------------------------

// Format ring-buffer entries for the currently-open channel into the
// history label. Newest at the bottom, oldest first. Each line is
// either the raw "sender: text" payload from the wire or, for our own
// sent messages, just "(me): text".
static void chat_history_render() {
  if (!s_chat_history || s_chat_channel_idx < 0) return;
  char out[1200];
  int oi = 0;
  out[0] = 0;
  // Walk the ring oldest-to-newest. s_chat_head points at the next slot
  // to write; s_chat_count says how many entries are populated.
  int start = (s_chat_head - s_chat_count + CHAT_RING_SIZE) % CHAT_RING_SIZE;
  for (int n = 0; n < s_chat_count; n++) {
    int i = (start + n) % CHAT_RING_SIZE;
    if (s_chat_ring[i].channel_idx != (uint8_t)s_chat_channel_idx) continue;
    int rem = (int)sizeof(out) - oi - 2;
    if (rem <= 0) break;
    int w = snprintf(&out[oi], rem, "%s\n", s_chat_ring[i].text);
    if (w < 0) break;
    oi += (w > rem) ? rem : w;
  }
  if (oi == 0) {
    lv_label_set_text(s_chat_history, "(no messages yet)");
  } else {
    if (oi > 0 && out[oi-1] == '\n') out[oi-1] = 0;
    lv_label_set_text(s_chat_history, out);
  }
  lv_obj_scroll_to_y(s_chat_history, INT16_MAX, LV_ANIM_OFF);   // pin to bottom
}

static void chat_compose_render() {
  if (!s_chat_compose) return;
  // Tail-visibility: show only the end of the buffer so the cursor is
  // always on screen. A leading … signals truncation. The character
  // counter lives on its own widget above the compose box so it
  // doesn't crowd the text.
  const int max_chars = (int)sizeof(s_compose_buf) - 1;
  const int visible = 48;
  const char* tail = s_compose_buf;
  bool truncated = false;
  if (s_compose_len > visible) {
    tail = s_compose_buf + (s_compose_len - visible);
    truncated = true;
  }
  char buf[160];
  if (s_compose_len == 0) {
    snprintf(buf, sizeof(buf), "> _");
  } else {
    snprintf(buf, sizeof(buf), "> %s%s_", truncated ? "…" : "", tail);
  }
  lv_label_set_text(s_chat_compose, buf);

  if (s_chat_counter) {
    char cbuf[16];
    snprintf(cbuf, sizeof(cbuf), "%d/%d", s_compose_len, max_chars);
    lv_label_set_text(s_chat_counter, cbuf);
    // Tint red when within 10 chars of the limit.
    uint32_t col = (s_compose_len > max_chars - 10) ? 0xe05050 : 0x707880;
    lv_obj_set_style_text_color(s_chat_counter, lv_color_hex(col), 0);
  }
}

static void chat_view_open(int channel_idx) {
  s_chat_open = true;
  s_chat_channel_idx = channel_idx;
  s_compose_buf[0] = 0;
  s_compose_len = 0;
  // Hide the channels list while the chat is active.
  if (s_channel_list) lv_obj_add_flag(s_channel_list, LV_OBJ_FLAG_HIDDEN);
  if (s_chat_history) lv_obj_remove_flag(s_chat_history, LV_OBJ_FLAG_HIDDEN);
  if (s_chat_compose) lv_obj_remove_flag(s_chat_compose, LV_OBJ_FLAG_HIDDEN);
  if (s_chat_counter) lv_obj_remove_flag(s_chat_counter, LV_OBJ_FLAG_HIDDEN);

  // Title shows which channel we're chatting in.
  char title[40];
  char chname[40];
  if (ui_get_channel_name && ui_get_channel_name(channel_idx, chname, sizeof(chname))) {
    if (strcmp(chname, "Public") == 0) {
      snprintf(title, sizeof(title), "Public");
    } else {
      snprintf(title, sizeof(title), "#%s", chname);
    }
  } else {
    snprintf(title, sizeof(title), "chat #%d", channel_idx);
  }
  lv_label_set_text(s_subscreen_title, title);

  chat_history_render();
  chat_compose_render();
}

static void chat_view_close() {
  s_chat_open = false;
  s_chat_channel_idx = -1;
  if (s_chat_history) lv_obj_add_flag(s_chat_history, LV_OBJ_FLAG_HIDDEN);
  if (s_chat_compose) lv_obj_add_flag(s_chat_compose, LV_OBJ_FLAG_HIDDEN);
  if (s_chat_counter) lv_obj_add_flag(s_chat_counter, LV_OBJ_FLAG_HIDDEN);
  // Return to channels list within the Messages sub-screen.
  lv_label_set_text(s_subscreen_title, TILES[1].title);
  if (s_channel_list) lv_obj_remove_flag(s_channel_list, LV_OBJ_FLAG_HIDDEN);
  channels_list_populate(0);
  lv_indev_t* enc = tpager_lvgl_get_encoder();
  if (enc && s_channel_group) {
    lv_indev_set_group(enc, s_channel_group);
    lv_group_set_editing(s_channel_group, false);
  }
}

// Append a message to the ring buffer. Older entries are evicted FIFO.
static void chat_ring_push(uint8_t channel_idx, uint32_t timestamp, const char* text) {
  ChatMsg& slot = s_chat_ring[s_chat_head];
  slot.channel_idx = channel_idx;
  slot.timestamp = timestamp;
  size_t n = strlen(text);
  if (n >= sizeof(slot.text)) n = sizeof(slot.text) - 1;
  memcpy(slot.text, text, n);
  slot.text[n] = 0;
  s_chat_head = (s_chat_head + 1) % CHAT_RING_SIZE;
  if (s_chat_count < CHAT_RING_SIZE) s_chat_count++;
}

// Called by MyMesh::onChannelMessageRecv. Single-threaded with the LVGL
// rendering, so we can update the UI directly when the active chat
// matches.
extern "C" void ui_on_channel_message(int channel_idx, uint32_t timestamp, const char* text) {
  if (!text) return;
  chat_ring_push((uint8_t)channel_idx, timestamp, text);
  if (s_chat_open && s_chat_channel_idx == channel_idx) {
    chat_history_render();
  }
}

// ---------- Contacts tile (S3.5) --------------------------------------------

// Role labels — 4-letter lowercase to match the width of "chat" (the
// shortest full word among ADV_TYPE_* values). Keeps the column width
// uniform across all four roles.
static const char* role_short(uint8_t type) {
  switch (type) {
    case 1: return "chat";   // Chat / Client
    case 2: return "rptr";   // Repeater
    case 3: return "room";   // Room
    case 4: return "sens";   // Sensor
    default: return "?";
  }
}

// Pixel widths chosen to fit the labels in the default Montserrat-14
// LVGL font on a 460-px content area. Name column takes the biggest
// slice; everything else is just wide enough for the worst-case value.
#define CON_COL_ROLE  40
#define CON_COL_NAME  180
#define CON_COL_SNR   42
#define CON_COL_RSSI  44
#define CON_COL_DIST  50
#define CON_COL_AGE   34

// Haversine — returns great-circle distance in km between two
// (lat, lon) pairs (degrees). 0 if either side has no fix.
static float haversine_km(double lat1, double lon1, double lat2, double lon2) {
  if (lat1 == 0 && lon1 == 0) return 0;
  if (lat2 == 0 && lon2 == 0) return 0;
  static const double R = 6371.0;
  double dLat = (lat2 - lat1) * 0.01745329252;  // π/180
  double dLon = (lon2 - lon1) * 0.01745329252;
  double a = sin(dLat / 2);
  a = a * a;
  double cl1 = cos(lat1 * 0.01745329252);
  double cl2 = cos(lat2 * 0.01745329252);
  double b = sin(dLon / 2);
  a += cl1 * cl2 * b * b;
  double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
  return (float)(R * c);
}

// Format a "Xs / Xm / Xh / Xd" relative-time string.
static void rel_time_fmt(uint32_t ts, uint32_t now, char* out, int n) {
  if (ts == 0 || now == 0 || ts > now) { snprintf(out, n, "-"); return; }
  uint32_t s = now - ts;
  if (s < 60)       snprintf(out, n, "%us", s);
  else if (s < 3600) snprintf(out, n, "%um", s / 60);
  else if (s < 86400) snprintf(out, n, "%uh", s / 3600);
  else               snprintf(out, n, "%ud", s / 86400);
}

static void contacts_list_populate() {
  if (!s_contacts_list || !ui_get_contact_count) return;
  lv_obj_clean(s_contacts_list);
  if (s_contacts_group) lv_group_remove_all_objs(s_contacts_group);

  double self_lat = 0, self_lon = 0;
  if (ui_get_self_loc) ui_get_self_loc(&self_lat, &self_lon);
  uint32_t now = ui_get_now_epoch ? ui_get_now_epoch() : 0;

  // Header row — same column widths as the data rows below. Non-
  // focusable so the encoder skips it.
  {
    lv_obj_t* hdr = lv_obj_create(s_contacts_list);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, lv_pct(100), 18);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(hdr, lv_color_hex(0x707880), 0);
    lv_obj_set_style_pad_hor(hdr, 6, 0);
    lv_obj_set_style_margin_bottom(hdr, 1, 0);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START,
                                LV_FLEX_ALIGN_CENTER,
                                LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr, 6, 0);
    static const struct { const char* text; int w; } cols[] = {
      {"role", CON_COL_ROLE}, {"name", CON_COL_NAME},
      {"SNR",  CON_COL_SNR },  {"RSSI", CON_COL_RSSI},
      {"dist", CON_COL_DIST},  {"age",  CON_COL_AGE },
    };
    for (auto& c : cols) {
      lv_obj_t* l = lv_label_create(hdr);
      lv_obj_set_width(l, c.w);
      lv_label_set_text(l, c.text);
    }
  }

  int count = ui_get_contact_count();
  if (count == 0) {
    lv_obj_t* empty = lv_label_create(s_contacts_list);
    lv_label_set_text(empty, "(no contacts heard yet)");
    lv_obj_set_style_text_color(empty, lv_color_hex(0x707880), 0);
    return;
  }

  for (int i = 0; i < count; i++) {
    UiContact ci;
    if (!ui_get_contact_info(i, &ci)) continue;

    lv_obj_t* row = lv_obj_create(s_contacts_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 22);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1c2530), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x2b3742), LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(row, lv_color_hex(0xc0c8d0), 0);
    lv_obj_set_style_text_color(row, lv_color_hex(0xFAA61A), LV_STATE_FOCUSED);
    lv_obj_set_style_pad_hor(row, 6, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_margin_bottom(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x303a47), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                                LV_FLEX_ALIGN_CENTER,
                                LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_add_event_cb(row, back_long_press_event, LV_EVENT_LONG_PRESSED, NULL);

    // Role label (first column). Lowercase 4-letter word so all roles
    // share the column width regardless of which one is shown.
    lv_obj_t* lbl_role = lv_label_create(row);
    lv_obj_set_width(lbl_role, CON_COL_ROLE);
    lv_label_set_text(lbl_role, role_short(ci.type));
    lv_obj_set_style_text_color(lbl_role, lv_color_hex(0xFAA61A), 0);

    // Name — auto-scrolls if it doesn't fit the column. Takes the
    // bulk of the row width since names like NL-MET-PLANTAGE-RPT are
    // common.
    lv_obj_t* lbl_name = lv_label_create(row);
    lv_obj_set_width(lbl_name, CON_COL_NAME);
    lv_label_set_long_mode(lbl_name, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(lbl_name, ci.name);

    // SNR (one decimal). Show `-` if we haven't heard from them yet
    // this session (snr_q == -128 sentinel).
    lv_obj_t* lbl_snr = lv_label_create(row);
    lv_obj_set_width(lbl_snr, CON_COL_SNR);
    char snr_buf[12];
    if (ci.snr_q == (int8_t)-128) snprintf(snr_buf, sizeof(snr_buf), "-");
    else {
      float snr = (float)ci.snr_q / 4.0f;
      snprintf(snr_buf, sizeof(snr_buf), "%+.1f", snr);
    }
    lv_label_set_text(lbl_snr, snr_buf);

    // RSSI in dBm — sentinel rssi==0 means we have no measurement
    // for this contact yet (real values are negative).
    lv_obj_t* lbl_rssi = lv_label_create(row);
    lv_obj_set_width(lbl_rssi, CON_COL_RSSI);
    char rssi_buf[12];
    if (ci.rssi == 0) snprintf(rssi_buf, sizeof(rssi_buf), "-");
    else              snprintf(rssi_buf, sizeof(rssi_buf), "%d", (int)ci.rssi);
    lv_label_set_text(lbl_rssi, rssi_buf);

    // Distance — needs both fixes; falls back to `-`.
    lv_obj_t* lbl_dist = lv_label_create(row);
    lv_obj_set_width(lbl_dist, CON_COL_DIST);
    double clat = ci.gps_lat / 1.0e6;
    double clon = ci.gps_lon / 1.0e6;
    float km = haversine_km(self_lat, self_lon, clat, clon);
    char dist_buf[16];
    if (km <= 0)        snprintf(dist_buf, sizeof(dist_buf), "-");
    else if (km < 1.0f) snprintf(dist_buf, sizeof(dist_buf), "%dm", (int)(km * 1000));
    else if (km < 10.0f) snprintf(dist_buf, sizeof(dist_buf), "%.1fk", km);
    else                 snprintf(dist_buf, sizeof(dist_buf), "%dk", (int)km);
    lv_label_set_text(lbl_dist, dist_buf);
    lv_obj_set_style_text_color(lbl_dist, lv_color_hex(0x80868f), 0);

    // Last advert (relative).
    lv_obj_t* lbl_age = lv_label_create(row);
    lv_obj_set_width(lbl_age, CON_COL_AGE);
    char age_buf[12];
    rel_time_fmt(ci.last_advert, now, age_buf, sizeof(age_buf));
    lv_label_set_text(lbl_age, age_buf);
    lv_obj_set_style_text_color(lbl_age, lv_color_hex(0x80868f), 0);

    if (s_contacts_group) lv_group_add_obj(s_contacts_group, row);
  }
}

// ---------- Edit popup -------------------------------------------------------

static void close_edit_popup_apply() {
  if (s_editing_idx < 0) return;

  // Special case: adding a channel doesn't touch NodePrefs at all —
  // it goes through the dedicated channel-add bridge and returns the
  // user to the channels list. Order matters: clear editing state on
  // both groups BEFORE rebuilding the channels list (otherwise the
  // group still thinks it's editing the now-destroyed OK button and
  // the encoder feels "locked").
  if (s_editing_idx == EDIT_IDX_NEW_CHANNEL) {
    if (s_text_len > 0 && ui_add_hashtag_channel) {
      ui_add_hashtag_channel(s_text_buf);
    }
    lv_obj_add_flag(s_edit_popup, LV_OBJ_FLAG_HIDDEN);
    if (s_edit_group)    lv_group_set_editing(s_edit_group, false);
    if (s_channel_group) lv_group_set_editing(s_channel_group, false);
    s_editing_idx = -1;
    channels_list_populate(1);   // rebuilds rows; focuses new entry
    lv_indev_t* enc = tpager_lvgl_get_encoder();
    if (enc && s_channel_group) {
      lv_indev_set_group(enc, s_channel_group);
      lv_group_set_editing(s_channel_group, false);
    }
    s_skip_next_click = true;     // swallow the click that just committed
    return;
  }

  NodePrefs* p = s_self ? s_self->getPrefs() : nullptr;
  if (!p) return;

  switch (s_editing_idx) {
    case 0: {
      // Freq from text buffer. Validate before assigning so a stray
      // empty/invalid input doesn't park the radio at 0 MHz.
      float v = atof(s_text_buf);
      if (v >= 400.0f && v <= 960.0f) {
        p->freq = v;
      }
      break;
    }
    case 1: p->sf = lv_spinbox_get_value(s_edit_widget); break;
    case 2: {
      int sel = lv_dropdown_get_selected(s_edit_widget);
      if (sel >= 0 && sel < BW_COUNT) p->bw = BW_VALUES[sel];
      break;
    }
    case 3: p->cr = lv_spinbox_get_value(s_edit_widget); break;
    case 4: p->tx_power_dbm = (int8_t)lv_spinbox_get_value(s_edit_widget); break;
    case 6: {
      int sel = lv_dropdown_get_selected(s_edit_widget);
      if (sel >= 0 && sel <= 2) p->path_hash_mode = (uint8_t)sel;
      break;
    }
    case 7: {
      // Scope rewrite goes through its own bridge — it has to derive
      // the HMAC key from the name AND persist, which ui_apply_radio_changes
      // (radio-params only) does not do.
      if (ui_apply_default_scope) ui_apply_default_scope(s_text_buf);
      break;
    }
    default: break;
  }

  int idx_was = s_editing_idx;
  // Skip the radio-params reapply for scope — already persisted by the
  // dedicated bridge, and re-running radio_set_params would be redundant.
  if (s_editing_idx != 7 && ui_apply_radio_changes) ui_apply_radio_changes();

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

  // Editor matrix per row:
  //   idx 0          → text-input (Freq, typed via FN+digits + '.')
  //   idx 1 / 3 / 4  → spinbox (SF, CR, TX power)
  //   idx 2          → dropdown over standard LoRa BW values
  //   idx 6          → dropdown over path-hash modes (1 / 2 / 3 bytes)
  //   idx 7          → text-input (Scope, letters + digits + '-')
  bool is_spinbox  = (idx == 1 || idx == 3 || idx == 4);
  bool is_dropdown = (idx == 2 || idx == 6);
  bool is_text     = (idx == 0 || idx == 7);
  if (!is_spinbox && !is_dropdown && !is_text) return;

  s_editing_idx = idx;
  lv_obj_remove_flag(s_edit_popup, LV_OBJ_FLAG_HIDDEN);

  if (s_edit_widget) { lv_obj_del(s_edit_widget); s_edit_widget = nullptr; }
  if (s_text_value_label) { lv_obj_del(s_text_value_label); s_text_value_label = nullptr; }
  lv_group_remove_all_objs(s_edit_group);

  if (is_text) {
    // Pre-fill the buffer with the current value so users see what
    // they're editing instead of an empty field.
    if (idx == 0) {
      snprintf(s_text_buf, sizeof(s_text_buf), "%.3f", p->freq);
      lv_label_set_text(s_edit_title, "Frequency (MHz)");
    } else {  // idx == 7, scope
      snprintf(s_text_buf, sizeof(s_text_buf), "%s", p->default_scope_name);
      lv_label_set_text(s_edit_title, "Region scope");
    }
    s_text_len = strlen(s_text_buf);

    s_text_value_label = lv_label_create(s_edit_popup);
    lv_obj_set_style_text_color(s_text_value_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_text_value_label, &lv_font_montserrat_24, 0);
    lv_label_set_text(s_text_value_label, s_text_buf);
    lv_obj_align(s_text_value_label, LV_ALIGN_CENTER, 0, -12);


    // Focusable OK button so encoder-click commits. Enter on the
    // keyboard also commits via ui_input_char below.
    s_edit_widget = lv_button_create(s_edit_popup);
    lv_obj_set_size(s_edit_widget, 60, 24);
    lv_obj_align(s_edit_widget, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_t* okl = lv_label_create(s_edit_widget);
    lv_label_set_text(okl, "OK");
    lv_obj_center(okl);

    lv_obj_add_event_cb(s_edit_widget, [](lv_event_t*) {
      close_edit_popup_apply();
    }, LV_EVENT_CLICKED, nullptr);
  } else if (is_spinbox) {
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
  } else {  // dropdown
    const char* opts = nullptr;
    int sel = 0;
    if (idx == 2) {
      opts = BW_OPTIONS;
      sel = bw_value_to_index(p->bw);
      lv_label_set_text(s_edit_title, "Bandwidth (kHz)");
    } else { // idx == 6
      opts = PATH_HASH_OPTIONS;
      sel = (int)p->path_hash_mode;
      if (sel < 0 || sel > 2) sel = 0;
      lv_label_set_text(s_edit_title, "Path-hash size");
    }

    s_edit_widget = lv_dropdown_create(s_edit_popup);
    lv_dropdown_set_options(s_edit_widget, opts);
    lv_dropdown_set_selected(s_edit_widget, sel);
    lv_obj_set_width(s_edit_widget, 200);
    lv_obj_align(s_edit_widget, LV_ALIGN_CENTER, 0, 0);

    // Dropdown emits VALUE_CHANGED when the user commits a selection. We
    // close the popup on commit so the click that selects an option also
    // applies it — matches the spinbox UX (rotate to change, click to save).
    lv_obj_add_event_cb(s_edit_widget, [](lv_event_t*) {
      close_edit_popup_apply();
    }, LV_EVENT_VALUE_CHANGED, nullptr);
  }

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
  lv_label_set_text(hint, "rotate: scroll   click: open   long: back   3s: power off");
  lv_obj_set_style_text_color(hint, lv_color_hex(0x707880), 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);

  // ---- Sub-screen container (hidden by default) ----
  s_subscreen_root = lv_obj_create(scr);
  lv_obj_remove_style_all(s_subscreen_root);
  lv_obj_set_size(s_subscreen_root, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(s_subscreen_root, lv_color_hex(0x0e141b), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_subscreen_root, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_add_flag(s_subscreen_root, LV_OBJ_FLAG_HIDDEN);

  // Subscreen content sits below the persistent header (y=0..20 reserved),
  // so all sub-screen widgets start at y >= 22 to avoid overlapping it.
  s_subscreen_title = lv_label_create(s_subscreen_root);
  lv_label_set_text(s_subscreen_title, "...");
  lv_obj_set_style_text_color(s_subscreen_title, lv_color_hex(0xFAA61A), 0);
  lv_obj_align(s_subscreen_title, LV_ALIGN_TOP_LEFT, 6, 24);

  s_subscreen_body = lv_label_create(s_subscreen_root);
  lv_label_set_text(s_subscreen_body, "");
  lv_obj_set_style_text_color(s_subscreen_body, lv_color_hex(0xc0c8d0), 0);
  lv_obj_set_width(s_subscreen_body, lv_pct(96));
  lv_label_set_long_mode(s_subscreen_body, LV_LABEL_LONG_WRAP);
  lv_obj_align(s_subscreen_body, LV_ALIGN_TOP_LEFT, 6, 48);

  // Radio settings list — plain container with custom 2-column rows so the
  // value column lines up. Built once, hidden by default, repopulated on
  // every sub-screen entry.
  s_radio_list = lv_obj_create(s_subscreen_root);
  lv_obj_remove_style_all(s_radio_list);
  lv_obj_set_size(s_radio_list, lv_pct(96), lv_pct(70));
  lv_obj_align(s_radio_list, LV_ALIGN_TOP_LEFT, 6, 44);
  lv_obj_set_flex_flow(s_radio_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_bg_color(s_radio_list, lv_color_hex(0x0e141b), 0);
  lv_obj_set_style_pad_all(s_radio_list, 2, 0);
  lv_obj_set_scroll_dir(s_radio_list, LV_DIR_VER);
  lv_obj_add_flag(s_radio_list, LV_OBJ_FLAG_HIDDEN);

  s_radio_group = lv_group_create();

  // Channels list (Messages tile, S3.4) — same row pattern as the radio
  // list, built once and hidden by default. channels_list_populate fills
  // it on demand. Sized identically so the header area stays consistent
  // between tiles.
  s_channel_list = lv_obj_create(s_subscreen_root);
  lv_obj_remove_style_all(s_channel_list);
  lv_obj_set_size(s_channel_list, lv_pct(96), lv_pct(70));
  lv_obj_align(s_channel_list, LV_ALIGN_TOP_LEFT, 6, 44);
  lv_obj_set_flex_flow(s_channel_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_bg_color(s_channel_list, lv_color_hex(0x0e141b), 0);
  lv_obj_set_style_pad_all(s_channel_list, 2, 0);
  lv_obj_set_scroll_dir(s_channel_list, LV_DIR_VER);
  lv_obj_add_flag(s_channel_list, LV_OBJ_FLAG_HIDDEN);

  s_channel_group = lv_group_create();

  // Contacts list (S3.5) — same row pattern as channels list. Built
  // once, hidden by default. Refilled on each enter from
  // ui_get_contact_count + ui_get_contact_info.
  s_contacts_list = lv_obj_create(s_subscreen_root);
  lv_obj_remove_style_all(s_contacts_list);
  lv_obj_set_size(s_contacts_list, lv_pct(96), lv_pct(70));
  lv_obj_align(s_contacts_list, LV_ALIGN_TOP_LEFT, 6, 44);
  lv_obj_set_flex_flow(s_contacts_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_bg_color(s_contacts_list, lv_color_hex(0x0e141b), 0);
  lv_obj_set_style_pad_all(s_contacts_list, 2, 0);
  lv_obj_set_scroll_dir(s_contacts_list, LV_DIR_VER);
  lv_obj_add_flag(s_contacts_list, LV_OBJ_FLAG_HIDDEN);

  s_contacts_group = lv_group_create();

  // Chat-view widgets — history label (top, scrollable) + compose
  // label (bottom). Both hidden until chat_view_open() shows them.
  s_chat_history = lv_label_create(s_subscreen_root);
  lv_obj_remove_style_all(s_chat_history);
  lv_obj_set_size(s_chat_history, lv_pct(96), lv_pct(60));
  lv_obj_align(s_chat_history, LV_ALIGN_TOP_LEFT, 6, 44);
  lv_label_set_long_mode(s_chat_history, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_color(s_chat_history, lv_color_hex(0xc0c8d0), 0);
  lv_obj_set_style_bg_color(s_chat_history, lv_color_hex(0x0e141b), 0);
  lv_obj_add_flag(s_chat_history, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(s_chat_history, LV_DIR_VER);
  lv_obj_add_flag(s_chat_history, LV_OBJ_FLAG_HIDDEN);

  s_chat_compose = lv_label_create(s_subscreen_root);
  lv_obj_set_size(s_chat_compose, lv_pct(96), 24);
  lv_obj_align(s_chat_compose, LV_ALIGN_BOTTOM_LEFT, 6, -4);
  lv_label_set_long_mode(s_chat_compose, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_color(s_chat_compose, lv_color_hex(0xFAA61A), 0);
  lv_obj_set_style_bg_color(s_chat_compose, lv_color_hex(0x1c2530), 0);
  lv_obj_set_style_bg_opa(s_chat_compose, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_hor(s_chat_compose, 6, 0);
  lv_obj_set_style_radius(s_chat_compose, 4, 0);
  lv_obj_add_flag(s_chat_compose, LV_OBJ_FLAG_HIDDEN);

  // Character-count indicator just above the compose box — kept on its
  // own widget so the message text and the counter never share a line.
  s_chat_counter = lv_label_create(s_subscreen_root);
  lv_label_set_text(s_chat_counter, "");
  lv_obj_set_style_text_color(s_chat_counter, lv_color_hex(0x707880), 0);
  lv_obj_align(s_chat_counter, LV_ALIGN_BOTTOM_RIGHT, -8, -30);
  lv_obj_add_flag(s_chat_counter, LV_OBJ_FLAG_HIDDEN);

  // ---- Persistent header (always visible) ----
  // Parented to the screen itself and built AFTER s_root + s_subscreen_root
  // so it paints on top of both — node name, DC usage and battery stay
  // visible whether the user is on the carousel or inside a tile.
  // The edit popup is created after this block, so popups still overlay
  // the header (we don't want the header eating popup pixels).
  s_header_label = lv_label_create(scr);
  lv_label_set_text(s_header_label, "T-Pager");
  lv_obj_set_style_text_color(s_header_label, lv_color_hex(0xFAA61A), 0);
  lv_obj_align(s_header_label, LV_ALIGN_TOP_LEFT, 6, 4);

  s_dc_label = lv_label_create(scr);
  lv_label_set_text(s_dc_label, "");
  lv_obj_set_style_text_color(s_dc_label, lv_color_hex(0x80868f), 0);
  lv_obj_align(s_dc_label, LV_ALIGN_TOP_MID, 0, 4);

  s_battery_label = lv_label_create(scr);
  lv_label_set_text(s_battery_label, "");
  lv_obj_set_style_text_color(s_battery_label, lv_color_hex(0xc0c8d0), 0);
  lv_obj_align(s_battery_label, LV_ALIGN_TOP_RIGHT, -6, 4);

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
  lv_label_set_text(back_hint, "click or dbl-click: edit   long-press: back");
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
  // Drive the TCA8418 keyboard scanner — upstream main.cpp doesn't call
  // any variant hook, so we do it here. variant_loop() is cheap when the
  // FIFO is empty and self-initialises on first call.
  variant_loop();


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
    while (s_quad_accum >= 4)  {
      if (s_chat_open && s_chat_history) {
        // In chat view encoder = scroll the history label (LVGL otherwise
        // moves focus, but there's no useful group here).
        lv_obj_scroll_by(s_chat_history, 0, -20, LV_ANIM_OFF);
      } else {
        tpager_lvgl_encoder_delta(+1 * sign);
      }
      s_quad_accum -= 4;
    }
    while (s_quad_accum <= -4) {
      if (s_chat_open && s_chat_history) {
        lv_obj_scroll_by(s_chat_history, 0, +20, LV_ANIM_OFF);
      } else {
        tpager_lvgl_encoder_delta(-1 * sign);
      }
      s_quad_accum += 4;
    }

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
      // Very long press (≥3 s) from anywhere triggers a clean power-off
      // via the BQ25896 BATFET_DIS bit — matches the LilyGo / Ripple
      // factory behaviour when the dedicated power button is held.
      if (held >= 3000 && tpager_power_off) {
        Serial.println("VERY_LONG_PRESS → tpager_power_off()");
        tpager_power_off();
      } else if (held >= 600) {
        if (s_editing_idx >= 0) {
          // Cancel edit popup without applying. For dropdown editors we
          // must (a) close the expanded option list — otherwise it stays
          // floating above the now-hidden popup and intercepts encoder
          // events — and (b) drop the group out of editing mode, so the
          // next encoder rotation drives the row-focus group instead of
          // a destroyed widget. Without these two the encoder appears to
          // "hang" after a cancel.
          if (s_edit_widget && (s_editing_idx == 2 || s_editing_idx == 6)) {
            lv_dropdown_close(s_edit_widget);
          }
          // Route the encoder back to whichever list-group the popup
          // was opened from: channels group for NEW_CHANNEL, otherwise
          // the radio settings group.
          lv_group_t* back_group = (s_editing_idx == EDIT_IDX_NEW_CHANNEL)
                                   ? s_channel_group : s_radio_group;
          lv_obj_add_flag(s_edit_popup, LV_OBJ_FLAG_HIDDEN);
          if (s_edit_group) lv_group_set_editing(s_edit_group, false);
          if (back_group)   lv_group_set_editing(back_group, false);
          lv_indev_t* enc2 = tpager_lvgl_get_encoder();
          if (enc2 && back_group) lv_indev_set_group(enc2, back_group);
          s_editing_idx = -1;
          s_skip_next_click = true;
        } else if (s_chat_open) {
          // One level back: chat → channels list (stays inside Messages).
          chat_view_close();
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

  // Periodic refresh of the contacts list so the "last seen" column
  // and any newly-heard nodes appear without needing to leave/re-enter.
  if (s_active_tile == 4 && s_contacts_list &&
      (long)(millis() - s_contacts_next_refresh) >= 0) {
    s_contacts_next_refresh = millis() + 5000;
    contacts_list_populate();
  }

  // Periodically refresh battery + header data on the carousel + sub-screen.
  if ((long)(millis() - s_next_status) > 0) {
    s_next_status = millis() + 1000;
    if (s_battery_label) {
      char buf[16];
      int p = tpager_battery_percent ? tpager_battery_percent() : -1;
      if (p >= 0) {
        snprintf(buf, sizeof(buf), "%d%%", p);
      } else {
        snprintf(buf, sizeof(buf), "%u mV", getBattMilliVolts());
      }
      lv_label_set_text(s_battery_label, buf);
    }
    if (s_dc_label && ui_get_duty_cycle_used_tenths) {
      int t = ui_get_duty_cycle_used_tenths();   // 0..1000 (0.0%..100.0%)
      int used_s = 0, max_s = 0;
      if (ui_get_duty_cycle_seconds) ui_get_duty_cycle_seconds(&used_s, &max_s);
      char buf[32];
      snprintf(buf, sizeof(buf), "DC %d.%d%% %ds/%ds",
               t / 10, t % 10, used_s, max_s);
      lv_label_set_text(s_dc_label, buf);
      // Tint: amber over 70% used, red over 90%, otherwise low-contrast.
      uint32_t col = (t > 900) ? 0xe05050
                   : (t > 700) ? 0xFAA61A
                                : 0x80868f;
      lv_obj_set_style_text_color(s_dc_label, lv_color_hex(col), 0);
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
