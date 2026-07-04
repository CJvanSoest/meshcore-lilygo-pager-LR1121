// LVGL-based T-Pager UI. Phase 2: horizontal tile carousel rendered with
// LVGL widgets. Encoder rotates focus; clicking a tile opens a sub-screen
// (placeholder content for now). Long-press goes back to the carousel.

#include "UITask.h"
#include "TPagerLVGL.h"
#include "TPagerST7796Display.h"
#include "map_screen.h"
#include <lvgl.h>
#include <cstring>
#include <cstdio>              // fopen/fread/fwrite for SD chat-history store
#include <esp_heap_caps.h>     // heap_caps_calloc / MALLOC_CAP_SPIRAM (S3.6d)

bool sd_init();                // from target.cpp — mounts /sd (idempotent)

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
// S3.6 layout — communication tiles first, then map/settings/info.
// Index meaning is referenced by `enter_subscreen()` switch below; keep
// the switch in sync if you reorder.
static const TileDef TILES[] = {
  { LV_SYMBOL_WIFI,      "Radio"      },   // 0 — ((•))
  { LV_SYMBOL_BELL,      "Channels"   },   // 1 — group chats / broadcast feel
  { LV_SYMBOL_ENVELOPE,  "DM"         },   // 2 — direct messages (per-contact)
  { LV_SYMBOL_LIST,      "Contacts"   },   // 3 — favorites only (S3.6c)
  { LV_SYMBOL_EYE_OPEN,  "Discovered" },   // 4 — every advert heard (S3.6b)
  { LV_SYMBOL_UPLOAD,    "Advert"     },   // 5 — send self-advert (direct/flood)
  { LV_SYMBOL_GPS,       "Map"        },   // 6 — SD raster tiles
  { LV_SYMBOL_SETTINGS,  "Settings"   },   // 7 — device settings list
  { LV_SYMBOL_HOME,      "About"      },   // 8 — build info
};
static const int NUM_TILES = sizeof(TILES) / sizeof(TILES[0]);

// Named tile indices — referenced by enter_subscreen(), the unread badges
// and the map refresh. Keep in sync with TILES[] above if you reorder.
enum {
  TILE_RADIO = 0, TILE_CHANNELS, TILE_DM, TILE_CONTACTS, TILE_DISCOVERED,
  TILE_ADVERT, TILE_MAP, TILE_SETTINGS, TILE_ABOUT,
};

// ---------- UI state ---------------------------------------------------------

static lv_obj_t* s_root;
static lv_obj_t* s_header_label;
static lv_obj_t* s_battery_label;
static lv_obj_t* s_dc_label;          // duty-cycle % shown between name + battery
static lv_obj_t* s_back_hint = nullptr;  // bottom hint on every sub-screen (Map hides it)
static lv_obj_t* s_tile_row;
static lv_obj_t* s_tile_buttons[NUM_TILES];
static lv_obj_t* s_tile_badges[NUM_TILES] = {0};  // unread-count badge per tile (item 7)
static lv_obj_t* s_subscreen_root;
static lv_obj_t* s_subscreen_title;
static lv_obj_t* s_title_rule[2] = {0};   // separator rules around the title (item 3)
static lv_obj_t* s_subscreen_body;
static lv_obj_t* s_radio_list;
static lv_obj_t* s_channel_list;     // S3.4 channels list (Messages tile)
static lv_obj_t* s_contacts_list;    // S3.5 contacts list (Contacts tile)
static lv_obj_t* s_dm_list;          // S3.6d DM contacts list (DM tile)
static lv_group_t* s_dm_list_group;
static lv_obj_t* s_discovered_list;  // S3.6b discovered list (Discovered tile)
static lv_obj_t* s_disc_menu_popup;  // S3.6b click-popup (Add / Add fav / Cancel)
static lv_group_t* s_disc_menu_group;
static lv_obj_t* s_disc_menu_buttons[3]; // 0=Add, 1=Add fav, 2=Cancel
static int       s_disc_menu_pubidx = -1;

static lv_obj_t* s_con_menu_popup;   // S3.6c click-popup (Open DM / Unfav / Cancel)
static lv_group_t* s_con_menu_group;
static lv_obj_t* s_con_menu_buttons[3];
static uint8_t   s_con_menu_pub_key[32]; // pub_key of clicked contact row

// Channels-row click popup (Open chat / Delete channel / Cancel).
static lv_obj_t* s_ch_menu_popup;
static lv_group_t* s_ch_menu_group;
static lv_obj_t* s_ch_menu_buttons[3];
static int       s_ch_menu_idx = -1;     // populated channel index
static lv_group_t* s_group;          // carousel tiles
static lv_group_t* s_radio_group;    // Radio settings list rows
static lv_group_t* s_channel_group;  // Channels list rows
static lv_group_t* s_contacts_group; // Contacts list rows
static unsigned long s_contacts_next_refresh = 0;
static lv_group_t* s_discovered_group; // Discovered list rows
static unsigned long s_discovered_next_refresh = 0;

// ---- Map Home→GPS handoff (item 5) ----
#define MAP_GPS_SWITCH_MS 4000
static unsigned long s_map_gps_switch_at = 0;   // 0 = no handoff pending
static unsigned long s_map_status_next = 0;     // live sat-count refresh tick
// Continuous GPS follow (issue 3): once true the map re-centres on the live
// fix every ~2 s so the crosshair + tiles track movement. Cleared when the
// user manually pans/zooms; re-armed each time the Map tile is opened.
static bool          s_map_follow = false;
static unsigned long s_map_follow_next = 0;

// ---- Settings tile (item 8) ----
static lv_obj_t*   s_settings_list = nullptr;
static lv_group_t* s_settings_group = nullptr;
// Runtime-adjustable display sleep timeout (battery saver). Cycled from the
// Settings list; not persisted (resets to the compile-time default on boot).
static unsigned long s_screen_off_ms = AUTO_OFF_MILLIS;

// ---- Advert subscreen (item 2) — two big buttons: Direct / Flood ----
static lv_obj_t*   s_advert_root = nullptr;
static lv_group_t* s_advert_group = nullptr;

// ---- Toast popup (item 2) — centered label that auto-closes after 3 s ----
static lv_obj_t* s_toast_popup = nullptr;
static lv_obj_t* s_toast_label = nullptr;
static lv_timer_t* s_toast_timer = nullptr;

// ---- Unread counters (item 7) -----------------------------------------------
// Per-channel unread keyed by channel index; per-DM unread keyed by the first
// 8 bytes of the peer pub_key (same key the DM ring in main.cpp uses).
#define UI_MAX_CHANNELS 40
static uint16_t s_unread_ch[UI_MAX_CHANNELS] = {0};
#define UI_MAX_DM_THREADS 32
struct DmUnread { uint8_t pubkey8[8]; uint16_t count; bool used; };
static DmUnread s_unread_dm[UI_MAX_DM_THREADS] = {0};

static int dm_unread_find(const uint8_t* pk8, bool create) {
  int free_slot = -1;
  for (int i = 0; i < UI_MAX_DM_THREADS; i++) {
    if (s_unread_dm[i].used) {
      if (memcmp(s_unread_dm[i].pubkey8, pk8, 8) == 0) return i;
    } else if (free_slot < 0) {
      free_slot = i;
    }
  }
  if (create && free_slot >= 0) {
    memcpy(s_unread_dm[free_slot].pubkey8, pk8, 8);
    s_unread_dm[free_slot].count = 0;
    s_unread_dm[free_slot].used = true;
    return free_slot;
  }
  return -1;
}
static uint16_t dm_unread_get(const uint8_t* pk) {
  int i = dm_unread_find(pk, false);
  return i >= 0 ? s_unread_dm[i].count : 0;
}
static void dm_unread_bump(const uint8_t* pk) {
  int i = dm_unread_find(pk, true);
  if (i >= 0 && s_unread_dm[i].count < 0xFFFF) s_unread_dm[i].count++;
}
static void dm_unread_clear(const uint8_t* pk) {
  int i = dm_unread_find(pk, false);
  if (i >= 0) s_unread_dm[i].count = 0;
}
static uint32_t unread_total_dm() {
  uint32_t t = 0;
  for (int i = 0; i < UI_MAX_DM_THREADS; i++)
    if (s_unread_dm[i].used) t += s_unread_dm[i].count;
  return t;
}
static uint32_t unread_total_ch() {
  uint32_t t = 0;
  for (int i = 0; i < UI_MAX_CHANNELS; i++) t += s_unread_ch[i];
  return t;
}
// Set a tile's unread badge (a small label child of the tile button). Hidden
// when count == 0. s_tile_badges[] is filled in build_ui().
static void update_tile_badge(int tile, uint32_t count) {
  if (tile < 0 || tile >= NUM_TILES) return;
  lv_obj_t* badge = s_tile_badges[tile];
  if (!badge) return;
  if (count == 0) { lv_obj_add_flag(badge, LV_OBJ_FLAG_HIDDEN); return; }
  char buf[8];
  if (count > 99) snprintf(buf, sizeof(buf), "99+");
  else            snprintf(buf, sizeof(buf), "%lu", (unsigned long)count);
  lv_label_set_text(badge, buf);
  lv_obj_remove_flag(badge, LV_OBJ_FLAG_HIDDEN);
}
static void update_tile_badges() {
  update_tile_badge(TILE_CHANNELS, unread_total_ch());
  update_tile_badge(TILE_DM, unread_total_dm());
}
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
// Heap-allocated in PSRAM (S3.6d). 32 × ~104 bytes = ~3.3 KB; pushing
// this to DRAM with the DM ring buffer + click caches added in this
// sprint overflowed the segment by ~80 bytes. The chat-view code
// initializes it lazily; renders return safely if the alloc fails.
static ChatMsg* s_chat_ring = nullptr;
static bool ensure_chat_ring() {
  if (s_chat_ring) return true;
  s_chat_ring = (ChatMsg*)heap_caps_calloc(
      CHAT_RING_SIZE, sizeof(ChatMsg),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return s_chat_ring != nullptr;
}
static int s_chat_head = 0;     // next slot to write
static int s_chat_count = 0;    // populated entries (≤ CHAT_RING_SIZE)
static bool s_chat_open = false;
static int  s_chat_channel_idx = -1;
// S3.6d DM mode flips the chat-view widget set: history pulls from
// ui_get_dm_msg(s_dm_pubkey, ...), and Enter sends via ui_send_dm.
static bool s_dm_mode = false;
static uint8_t s_dm_pubkey[32] = {0};
static char    s_dm_peer_name[32] = {0};
static lv_obj_t* s_chat_scroll  = nullptr;  // scrollable wrapper around the spangroup
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
static char s_input_buf[64] = {0};  // keyboard test placeholder, no longer
                                    // wired to a tile — kept small to free
                                    // DRAM for S3.6d DM state.
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
static void chan_persist_load(uint8_t idx);
static bool chan_ring_has(uint8_t idx);
static void contacts_list_populate();
static void discovered_list_populate();
static void settings_list_populate();
static void disc_menu_show(int disc_idx);
static void disc_menu_close();
static void con_menu_show(const uint8_t* pub_key);
static void con_menu_close();
static void dm_list_populate();
static void dm_chat_view_open(const uint8_t* pub_key, const char* peer_name);
static void ch_menu_show(int populated_idx);
static void ch_menu_close();

// High-contrast focused state for popup buttons (disc / con / ch menus).
// Default LVGL theme gives a too-subtle outline that's hard to track on
// the ST7796 panel — applying orange bg + dark text matches the row
// focus style used by the list tiles.
static void style_popup_button(lv_obj_t* btn) {
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x1c2530), 0);
  lv_obj_set_style_text_color(btn, lv_color_hex(0xc0c8d0), 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0xFAA61A), LV_STATE_FOCUSED);
  lv_obj_set_style_text_color(btn, lv_color_hex(0x101418), LV_STATE_FOCUSED);
  lv_obj_set_style_border_width(btn, 0, 0);
  lv_obj_set_style_border_width(btn, 0, LV_STATE_FOCUSED);
}
// No per-row cache for the DM list — DRAM is tight (88%+) so the click
// handler refetches contact info on-demand via ui_get_dm_contact_info(idx).

extern "C" void tpager_power_off() __attribute__((weak));
extern "C" void ui_apply_radio_changes() __attribute__((weak));
// Companion writes the new default-scope name + derived HMAC key into
// NodePrefs and persists. Empty name = wildcard / no scope.
extern "C" void ui_apply_default_scope(const char* name) __attribute__((weak));
// Channels (S3.4) — companion enumerates / mutates the GroupChannel table.
extern "C" int  ui_get_channel_count() __attribute__((weak));
extern "C" bool ui_get_channel_name(int idx, char* buf, int buf_size) __attribute__((weak));
extern "C" bool ui_add_hashtag_channel(const char* name) __attribute__((weak));
extern "C" bool ui_delete_channel(int populated_idx) __attribute__((weak));
extern "C" bool ui_send_group_text(int channel_idx, const char* text) __attribute__((weak));
extern "C" bool ui_send_self_advert(bool flood) __attribute__((weak));
extern "C" void ui_set_node_name(const char* name) __attribute__((weak));
extern "C" const char* ui_get_node_name() __attribute__((weak));

// Contacts bridges (S3.5 — S3.6c filters to favorites; S3.6b adds the
// "Discovered" ring-buffer view as a sibling tile).
struct UiContact {
  char     name[32];
  uint8_t  type;
  int32_t  gps_lat;
  int32_t  gps_lon;
  uint32_t last_advert;
  int8_t   snr_q;
  int16_t  rssi;
  uint8_t  pub_key[32];     // S3.6c — needed for toggle-favorite + DM thread lookup
  uint8_t  flags;           // S3.6c — bit 0 = favorite
  uint8_t  path_len;        // S3.6b — hop count for Discovered rows (0 = direct)
};
extern "C" int  ui_get_contact_count() __attribute__((weak));
extern "C" bool ui_get_contact_info(int idx, UiContact* out) __attribute__((weak));
extern "C" bool ui_toggle_favorite(const uint8_t* pub_key) __attribute__((weak));
extern "C" int  ui_get_discovered_count() __attribute__((weak));
extern "C" bool ui_get_discovered_info(int idx, UiContact* out) __attribute__((weak));
extern "C" bool ui_add_discovered_to_contacts(int idx, bool favorite) __attribute__((weak));
// DM bridges (S3.6d).
struct UiDmMsg {
  char     text[120];
  uint8_t  from_me;
  uint32_t age_s;
};
extern "C" int  ui_get_dm_contact_count() __attribute__((weak));
extern "C" bool ui_get_dm_contact_info(int idx, UiContact* out) __attribute__((weak));
extern "C" bool ui_send_dm(const uint8_t* pub_key, const char* text) __attribute__((weak));
extern "C" int  ui_get_dm_msg_count(const uint8_t* pub_key) __attribute__((weak));
extern "C" bool ui_get_dm_msg(const uint8_t* pub_key, int idx, UiDmMsg* out) __attribute__((weak));
extern "C" void ui_get_self_loc(double* lat, double* lon) __attribute__((weak));
// Saved "home" location (item 8). Returns true if a home has been stored.
extern "C" bool ui_get_home_loc(double* lat, double* lon) __attribute__((weak));
// Restore a peer's DM history from SD into the ring (no-op if already loaded).
extern "C" void ui_load_dm_history(const uint8_t* pub_key) __attribute__((weak));
extern "C" void ui_set_home_loc(double lat, double lon) __attribute__((weak));
extern "C" uint32_t ui_get_now_epoch() __attribute__((weak));

// Reference location for distance-to-contact: live GPS if we have a fix,
// otherwise the saved Home (item 8). (0,0) if neither is available.
static void ui_ref_loc(double* lat, double* lon) {
  double glat = 0, glon = 0;
  if (ui_get_self_loc) ui_get_self_loc(&glat, &glon);
  if (glat || glon) { *lat = glat; *lon = glon; return; }
  double hlat = 0, hlon = 0;
  if (ui_get_home_loc && ui_get_home_loc(&hlat, &hlon)) { *lat = hlat; *lon = hlon; return; }
  *lat = 0; *lon = 0;
}
// Feed the Map view (map_screen.cpp) with node positions: every contact and
// discovered node that carries a fix. Contacts render as orange dots,
// repeaters (ADV type 2) as green squares. Layout matches MapMarker there.
struct UiMapMarker { double lat; double lon; uint8_t is_repeater; char name[24]; };
extern "C" int ui_get_map_markers(UiMapMarker* out, int max) {
  int n = 0;
  UiContact ci;
  if (ui_get_contact_count && ui_get_contact_info) {
    int cnt = ui_get_contact_count();
    for (int i = 0; i < cnt && n < max; i++) {
      if (!ui_get_contact_info(i, &ci)) continue;
      if (ci.gps_lat == 0 && ci.gps_lon == 0) continue;
      out[n].lat = ci.gps_lat / 1.0e6;
      out[n].lon = ci.gps_lon / 1.0e6;
      out[n].is_repeater = (ci.type == 2) ? 1 : 0;
      snprintf(out[n].name, sizeof(out[n].name), "%s", ci.name);
      n++;
    }
  }
  if (ui_get_discovered_count && ui_get_discovered_info) {
    int cnt = ui_get_discovered_count();
    for (int i = 0; i < cnt && n < max; i++) {
      if (!ui_get_discovered_info(i, &ci)) continue;
      if (ci.gps_lat == 0 && ci.gps_lon == 0) continue;
      out[n].lat = ci.gps_lat / 1.0e6;
      out[n].lon = ci.gps_lon / 1.0e6;
      out[n].is_repeater = (ci.type == 2) ? 1 : 0;
      snprintf(out[n].name, sizeof(out[n].name), "%s", ci.name);
      n++;
    }
  }
  return n;
}

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
  // Map navigation (item: map controls). While the Map tile is open the
  // QWERTY keys drive pan/zoom: W/A/S/D pan north/west/south/east, Z zooms
  // in, X zooms out. Any handled key cancels the pending auto-GPS recenter
  // so the view stays where the user put it.
  if (s_active_tile == TILE_MAP) {
    char k = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    bool handled = true;
    switch (k) {
      case 'w': map_screen_pan(0, -1); break;
      case 's': map_screen_pan(0, +1); break;
      case 'a': map_screen_pan(-1, 0); break;
      case 'd': map_screen_pan(+1, 0); break;
      case 'z': map_screen_zoom(+1); break;
      case 'x': map_screen_zoom(-1); break;
      default:  handled = false; break;
    }
    if (handled) { s_map_gps_switch_at = 0; s_map_follow = false; return; }
  }

  // Chat compose: when a chat view is open, the keyboard feeds the
  // compose buffer instead of any popup. Enter sends via the bridge;
  // '\b' pops; everything else (printable from the QWERTY + symbol
  // layer) appends. We accept anything that's not Enter/backspace so
  // letters, digits, symbols and even '#' (FN+Space) all work.
  if (s_chat_open && s_chat_compose) {
    if (c == '\n') {
      if (s_compose_len > 0) {
        bool ok = false;
        if (s_dm_mode) {
          if (ui_send_dm) ok = ui_send_dm(s_dm_pubkey, s_compose_buf);
          if (ok) chat_history_render();   // DM ring already has our own copy
        } else if (ui_send_group_text) {
          ok = ui_send_group_text(s_chat_channel_idx, s_compose_buf);
          if (ok) {
            // Show our own message in the history immediately. The wire
            // payload is "<sender>: <msg>" — mirror that locally with a
            // "(me)" prefix so it visually matches received messages.
            char local[120];
            snprintf(local, sizeof(local), "(me): %s", s_compose_buf);
            chat_ring_push((uint8_t)s_chat_channel_idx, 0, local);
            chat_history_render();
          }
        }
        (void)ok;
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
  // Text-input popup: freq (idx 0), scope (idx 7), node name (idx 8),
  // or new-channel (EDIT_IDX_NEW_CHANNEL). Char filter is per-row —
  // freq accepts digits + '.', scope + new-channel accept lowercase
  // letters + digits + '-', node name accepts mixed-case letters +
  // digits + '-' + '_'. All accept '\b' (pop) and '\n' (commit).
  if ((s_editing_idx == 0 || s_editing_idx == 7 || s_editing_idx == 8 ||
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
      } else if (s_editing_idx == 8) {  // node name — wider set
        allow = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_';
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
  // Title + its separator rules are shown by default; the Map case hides
  // them (the map has its own status strip).
  lv_obj_remove_flag(s_subscreen_title, LV_OBJ_FLAG_HIDDEN);
  if (s_title_rule[0]) lv_obj_remove_flag(s_title_rule[0], LV_OBJ_FLAG_HIDDEN);
  if (s_title_rule[1]) lv_obj_remove_flag(s_title_rule[1], LV_OBJ_FLAG_HIDDEN);

  // Default: simple body label visible, all per-tile containers hidden.
  lv_obj_remove_flag(s_subscreen_body, LV_OBJ_FLAG_HIDDEN);
  if (s_radio_list)      lv_obj_add_flag(s_radio_list,      LV_OBJ_FLAG_HIDDEN);
  if (s_channel_list)    lv_obj_add_flag(s_channel_list,    LV_OBJ_FLAG_HIDDEN);
  if (s_dm_list)         lv_obj_add_flag(s_dm_list,         LV_OBJ_FLAG_HIDDEN);
  if (s_contacts_list)   lv_obj_add_flag(s_contacts_list,   LV_OBJ_FLAG_HIDDEN);
  if (s_discovered_list) lv_obj_add_flag(s_discovered_list, LV_OBJ_FLAG_HIDDEN);
  if (s_advert_root)     lv_obj_add_flag(s_advert_root,     LV_OBJ_FLAG_HIDDEN);
  if (s_settings_list)   lv_obj_add_flag(s_settings_list,   LV_OBJ_FLAG_HIDDEN);
  if (s_disc_menu_popup) lv_obj_add_flag(s_disc_menu_popup, LV_OBJ_FLAG_HIDDEN);
  if (s_con_menu_popup)  lv_obj_add_flag(s_con_menu_popup,  LV_OBJ_FLAG_HIDDEN);
  if (s_ch_menu_popup)   lv_obj_add_flag(s_ch_menu_popup,   LV_OBJ_FLAG_HIDDEN);
  if (s_input_label)     lv_obj_add_flag(s_input_label,     LV_OBJ_FLAG_HIDDEN);
  if (s_chat_scroll)     lv_obj_add_flag(s_chat_scroll,     LV_OBJ_FLAG_HIDDEN);
  if (s_chat_compose)    lv_obj_add_flag(s_chat_compose,    LV_OBJ_FLAG_HIDDEN);
  if (s_chat_counter)    lv_obj_add_flag(s_chat_counter,    LV_OBJ_FLAG_HIDDEN);
  map_screen_hide();
  s_input_active = false;
  s_chat_open = false;
  s_chat_channel_idx = -1;

  // Default: encoder drives carousel group (will switch for Radio below).
  lv_indev_t* enc = tpager_lvgl_get_encoder();

  char body[160];
  body[0] = 0;
  switch (idx) {
    case 0: {  // Radio
      lv_obj_add_flag(s_subscreen_body, LV_OBJ_FLAG_HIDDEN);
      if (s_radio_list) lv_obj_remove_flag(s_radio_list, LV_OBJ_FLAG_HIDDEN);
      if (s_self && s_self->getPrefs()) radio_list_populate(s_self->getPrefs());
      if (enc && s_radio_group) lv_indev_set_group(enc, s_radio_group);
      return;
    }
    case 1: {  // Channels (S3.4)
      lv_obj_add_flag(s_subscreen_body, LV_OBJ_FLAG_HIDDEN);
      if (s_channel_list) lv_obj_remove_flag(s_channel_list, LV_OBJ_FLAG_HIDDEN);
      channels_list_populate(0);
      if (enc && s_channel_group) lv_indev_set_group(enc, s_channel_group);
      return;
    }
    case 2: {  // DM (S3.6d) — chat-type contacts, click to open thread
      lv_obj_add_flag(s_subscreen_body, LV_OBJ_FLAG_HIDDEN);
      if (s_dm_list) lv_obj_remove_flag(s_dm_list, LV_OBJ_FLAG_HIDDEN);
      dm_list_populate();
      if (enc && s_dm_list_group) lv_indev_set_group(enc, s_dm_list_group);
      return;
    }
    case 3: {  // Contacts → favorites-only (S3.6c)
      lv_obj_add_flag(s_subscreen_body, LV_OBJ_FLAG_HIDDEN);
      if (s_contacts_list) lv_obj_remove_flag(s_contacts_list, LV_OBJ_FLAG_HIDDEN);
      contacts_list_populate();
      s_contacts_next_refresh = millis() + 5000;
      if (enc && s_contacts_group) lv_indev_set_group(enc, s_contacts_group);
      return;
    }
    case 4: {  // Discovered (S3.6b) — every advert, even non-auto-added
      lv_obj_add_flag(s_subscreen_body, LV_OBJ_FLAG_HIDDEN);
      if (s_discovered_list) lv_obj_remove_flag(s_discovered_list, LV_OBJ_FLAG_HIDDEN);
      discovered_list_populate();
      s_discovered_next_refresh = millis() + 5000;
      if (enc && s_discovered_group) lv_indev_set_group(enc, s_discovered_group);
      return;
    }
    case 5: {  // Advert (item 2) — two buttons: Direct / Flood
      lv_obj_add_flag(s_subscreen_body, LV_OBJ_FLAG_HIDDEN);
      if (s_advert_root) lv_obj_remove_flag(s_advert_root, LV_OBJ_FLAG_HIDDEN);
      if (enc && s_advert_group) {
        lv_indev_set_group(enc, s_advert_group);
        lv_group_focus_obj(lv_obj_get_child(s_advert_root, 0));
      }
      return;
    }
    case 6: {  // Map (S3.6 — Phase 2 raster, status strip owns the top row)
      lv_obj_add_flag(s_subscreen_body, LV_OBJ_FLAG_HIDDEN);
      // Hide the per-subscreen Pager name + title + rules (the Map status
      // strip takes their place) and the bottom edit-hint (no editable
      // widgets in the Map view). Slide DC next to battery on the right so
      // it doesn't collide with the MAP text on the left.
      if (s_header_label) lv_obj_add_flag(s_header_label, LV_OBJ_FLAG_HIDDEN);
      if (s_back_hint)    lv_obj_add_flag(s_back_hint,    LV_OBJ_FLAG_HIDDEN);
      if (s_subscreen_title) lv_obj_add_flag(s_subscreen_title, LV_OBJ_FLAG_HIDDEN);
      if (s_title_rule[0]) lv_obj_add_flag(s_title_rule[0], LV_OBJ_FLAG_HIDDEN);
      if (s_title_rule[1]) lv_obj_add_flag(s_title_rule[1], LV_OBJ_FLAG_HIDDEN);
      if (s_dc_label && s_battery_label) {
        lv_obj_align_to(s_dc_label, s_battery_label,
                        LV_ALIGN_OUT_LEFT_MID, -8, 0);
      }
      // Centre on saved Home first (instant); fall back to live GPS if no
      // Home is set. The loop hands off to live GPS after MAP_GPS_SWITCH_MS.
      double clat = 0, clon = 0;
      bool have_center = false;
      if (ui_get_home_loc && ui_get_home_loc(&clat, &clon) && (clat || clon)) {
        have_center = true;
      } else if (ui_get_self_loc) {
        ui_get_self_loc(&clat, &clon);
        have_center = (clat || clon);
      }
      if (have_center) map_screen_set_center(clat, clon);
      map_screen_show();
      s_map_gps_switch_at = millis() + MAP_GPS_SWITCH_MS;
      s_map_follow = true;          // issue 3: track the live fix continuously
      s_map_follow_next = 0;
      return;
    }
    case 7: {  // Settings (item 8) — action-row list
      lv_obj_add_flag(s_subscreen_body, LV_OBJ_FLAG_HIDDEN);
      if (s_settings_list) lv_obj_remove_flag(s_settings_list, LV_OBJ_FLAG_HIDDEN);
      settings_list_populate();
      if (enc && s_settings_group) lv_indev_set_group(enc, s_settings_group);
      return;
    }
    case 8:
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
  if (s_input_label)     lv_obj_add_flag(s_input_label,     LV_OBJ_FLAG_HIDDEN);
  if (s_channel_list)    lv_obj_add_flag(s_channel_list,    LV_OBJ_FLAG_HIDDEN);
  if (s_dm_list)         lv_obj_add_flag(s_dm_list,         LV_OBJ_FLAG_HIDDEN);
  if (s_contacts_list)   lv_obj_add_flag(s_contacts_list,   LV_OBJ_FLAG_HIDDEN);
  if (s_discovered_list) lv_obj_add_flag(s_discovered_list, LV_OBJ_FLAG_HIDDEN);
  if (s_advert_root)     lv_obj_add_flag(s_advert_root,     LV_OBJ_FLAG_HIDDEN);
  if (s_settings_list)   lv_obj_add_flag(s_settings_list,   LV_OBJ_FLAG_HIDDEN);
  if (s_disc_menu_popup) lv_obj_add_flag(s_disc_menu_popup, LV_OBJ_FLAG_HIDDEN);
  if (s_con_menu_popup)  lv_obj_add_flag(s_con_menu_popup,  LV_OBJ_FLAG_HIDDEN);
  if (s_ch_menu_popup)   lv_obj_add_flag(s_ch_menu_popup,   LV_OBJ_FLAG_HIDDEN);
  if (s_chat_scroll)     lv_obj_add_flag(s_chat_scroll,     LV_OBJ_FLAG_HIDDEN);
  if (s_chat_compose)    lv_obj_add_flag(s_chat_compose,    LV_OBJ_FLAG_HIDDEN);
  if (s_chat_counter)    lv_obj_add_flag(s_chat_counter,    LV_OBJ_FLAG_HIDDEN);
  map_screen_hide();
  // Restore the labels + DC position the Map tile takes over.
  if (s_header_label) lv_obj_remove_flag(s_header_label, LV_OBJ_FLAG_HIDDEN);
  if (s_back_hint)    lv_obj_remove_flag(s_back_hint,    LV_OBJ_FLAG_HIDDEN);
  if (s_dc_label)     lv_obj_align(s_dc_label, LV_ALIGN_TOP_MID, 0, 4);
  // DM mode unset
  s_dm_mode = false;
  memset(s_dm_pubkey, 0, sizeof(s_dm_pubkey));
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
    "Name",       // node_name (UX round 2 — Heltec / others see this)
  };
  static const char* UNITS[]  = { " MHz", "", " kHz", "", " dBm", "", "", "", "" };
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
      case 8: snprintf(val, sizeof(val), "%s", p->node_name); break;
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
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);  // single-click (item 6)
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
  // 3-option popup: Open chat / Delete channel / Cancel. Skip the popup
  // for "Public" (slot 0) because deleting the built-in public channel
  // would just be re-created by MyMesh on next boot — confusing UX.
  if (idx == 0) {
    chat_view_open(idx);
    return;
  }
  ch_menu_show(idx);
}

static void ch_menu_close() {
  if (s_ch_menu_popup) lv_obj_add_flag(s_ch_menu_popup, LV_OBJ_FLAG_HIDDEN);
  s_ch_menu_idx = -1;
  lv_indev_t* enc = tpager_lvgl_get_encoder();
  if (enc && s_channel_group) {
    lv_group_set_editing(s_channel_group, false);
    lv_indev_set_group(enc, s_channel_group);
  }
}

static void ch_menu_show(int populated_idx) {
  if (!s_ch_menu_popup) return;
  s_ch_menu_idx = populated_idx;
  lv_obj_remove_flag(s_ch_menu_popup, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_ch_menu_popup);
  lv_indev_t* enc = tpager_lvgl_get_encoder();
  if (enc && s_ch_menu_group) {
    lv_indev_set_group(enc, s_ch_menu_group);
    if (s_ch_menu_buttons[0]) lv_group_focus_obj(s_ch_menu_buttons[0]);
  }
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
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);  // single-click (item 6)
    lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);

    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);

    // Per-channel unread count (item 7), right-aligned in the row.
    if (!is_add && data >= 0 && data < UI_MAX_CHANNELS && s_unread_ch[data] > 0) {
      lv_obj_t* cnt = lv_label_create(row);
      char cbuf[8];
      snprintf(cbuf, sizeof(cbuf), "%u", (unsigned)s_unread_ch[data]);
      lv_label_set_text(cnt, cbuf);
      lv_obj_set_style_bg_color(cnt, lv_color_hex(0xe05050), 0);
      lv_obj_set_style_bg_opa(cnt, LV_OPA_COVER, 0);
      lv_obj_set_style_text_color(cnt, lv_color_hex(0xffffff), 0);
      lv_obj_set_style_radius(cnt, 8, 0);
      lv_obj_set_style_pad_hor(cnt, 5, 0);
      lv_obj_align(cnt, LV_ALIGN_RIGHT_MID, -2, 0);
    }

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

// Helper — push one chat line into the history spangroup as two coloured
// segments: a green sender prefix (everything up to and including the
// first ':') and a light-grey message body (the rest plus a newline).
// Falls back to a single grey span if the line has no ':' separator.
static void chat_history_append_line(const char* line) {
  if (!s_chat_history || !line) return;
  const char* colon = strchr(line, ':');
  // Sender = "name: " (include the colon and trailing space) — green.
  lv_span_t* sp_who = lv_spangroup_new_span(s_chat_history);
  if (!sp_who) return;
  if (colon) {
    char who[40];
    size_t n = (size_t)(colon - line) + 2;   // include ": "
    if (n >= sizeof(who)) n = sizeof(who) - 1;
    memcpy(who, line, n);
    who[n] = 0;
    lv_span_set_text(sp_who, who);
  } else {
    lv_span_set_text(sp_who, line);
  }
  lv_style_set_text_color(&sp_who->style, lv_color_hex(0x55cc66));

  // Message body — light grey with @-mentions highlighted blue.
  if (!colon) {
    lv_span_t* sp_nl = lv_spangroup_new_span(s_chat_history);
    if (sp_nl) {
      lv_span_set_text(sp_nl, "\n");
      lv_style_set_text_color(&sp_nl->style, lv_color_hex(0xc0c8d0));
    }
    return;
  }
  const char* body = colon + 1;
  while (*body == ' ') body++;

  // Walk the body and split into alternating grey / blue spans. Mentions
  // start at '@' and run until the next non-name char (letters, digits,
  // '-' and '_' are part of the name; everything else breaks the token).
  auto is_name_char = [](char c) -> bool {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
  };

  char buf[160];
  int bi = 0;
  auto flush_grey = [&]() {
    if (bi == 0) return;
    buf[bi] = 0;
    lv_span_t* sp = lv_spangroup_new_span(s_chat_history);
    if (sp) {
      lv_span_set_text(sp, buf);
      lv_style_set_text_color(&sp->style, lv_color_hex(0xc0c8d0));
    }
    bi = 0;
  };

  for (const char* p = body; *p; p++) {
    if (*p == '@') {
      flush_grey();
      // Collect mention token including the '@'. MeshCore wire format is
      // `@[name]` (square brackets around the target's name); we also
      // accept the bare `@name` form as a fallback. Bracketed form runs
      // until the closing ']' (inclusive). Bare form runs until the
      // first non-name char.
      char mention[42];
      int mi = 0;
      mention[mi++] = '@';
      p++;
      if (*p == '[') {
        while (*p && mi < (int)sizeof(mention) - 1) {
          mention[mi++] = *p;
          if (*p == ']') { p++; break; }
          p++;
        }
      } else {
        while (*p && is_name_char(*p) && mi < (int)sizeof(mention) - 1) {
          mention[mi++] = *p++;
        }
      }
      mention[mi] = 0;
      lv_span_t* sp = lv_spangroup_new_span(s_chat_history);
      if (sp) {
        lv_span_set_text(sp, mention);
        lv_style_set_text_color(&sp->style, lv_color_hex(0x5ab8ff));
      }
      if (!*p) break;
      // The char that broke the token still needs to be emitted. Back up
      // one so the outer loop's p++ lands on it.
      p--;
      continue;
    }
    if (bi < (int)sizeof(buf) - 1) buf[bi++] = *p;
  }
  // Trailing newline goes in the last grey span so word-wrap is clean.
  if (bi < (int)sizeof(buf) - 1) buf[bi++] = '\n';
  flush_grey();
}

// Format ring-buffer entries for the currently-open chat into the
// history widget. Newest at the bottom. Each line gets two colored
// spans: green sender, grey body.
static void chat_history_render() {
  if (!s_chat_history) return;
  // Clear all existing spans before rebuilding.
  while (lv_spangroup_get_span_count(s_chat_history) > 0) {
    lv_span_t* sp = lv_spangroup_get_child(s_chat_history, 0);
    if (!sp) break;
    lv_spangroup_delete_span(s_chat_history, sp);
  }

  int rendered = 0;
  if (s_dm_mode) {
    if (ui_get_dm_msg_count && ui_get_dm_msg) {
      int n = ui_get_dm_msg_count(s_dm_pubkey);
      for (int k = 0; k < n; k++) {
        UiDmMsg m;
        if (!ui_get_dm_msg(s_dm_pubkey, k, &m)) break;
        const char* who = m.from_me ? "(me)" : s_dm_peer_name;
        char line[180];
        snprintf(line, sizeof(line), "%s: %s", who, m.text);
        chat_history_append_line(line);
        rendered++;
      }
    }
  } else if (s_chat_channel_idx >= 0 && s_chat_ring) {
    int start = (s_chat_head - s_chat_count + CHAT_RING_SIZE) % CHAT_RING_SIZE;
    for (int n = 0; n < s_chat_count; n++) {
      int i = (start + n) % CHAT_RING_SIZE;
      if (s_chat_ring[i].channel_idx != (uint8_t)s_chat_channel_idx) continue;
      chat_history_append_line(s_chat_ring[i].text);
      rendered++;
    }
  }
  if (rendered == 0) {
    lv_span_t* sp = lv_spangroup_new_span(s_chat_history);
    if (sp) {
      lv_span_set_text(sp, "(no messages yet)");
      lv_style_set_text_color(&sp->style, lv_color_hex(0x707880));
    }
  }
  // Pin to bottom — force a layout pass first so the spangroup's content
  // height (and thus the scroll extent) is up to date before scrolling.
  if (s_chat_scroll) {
    lv_spangroup_refr_mode(s_chat_history);
    lv_obj_update_layout(s_chat_scroll);
    lv_obj_scroll_to_y(s_chat_scroll, LV_COORD_MAX, LV_ANIM_OFF);
  }
}

// Open DM chat view with a contact (S3.6d). Mirror of chat_view_open
// for channels but flips s_dm_mode + sets s_dm_pubkey instead of
// s_chat_channel_idx.
static void dm_chat_view_open(const uint8_t* pub_key, const char* peer_name) {
  s_chat_open = true;
  s_chat_channel_idx = -1;
  s_dm_mode = true;
  memcpy(s_dm_pubkey, pub_key, 32);
  if (ui_load_dm_history) ui_load_dm_history(pub_key);  // restore from SD
  dm_unread_clear(pub_key);       // reading this thread clears its badge
  update_tile_badges();
  strncpy(s_dm_peer_name, peer_name ? peer_name : "?",
          sizeof(s_dm_peer_name) - 1);
  s_dm_peer_name[sizeof(s_dm_peer_name) - 1] = 0;
  s_compose_buf[0] = 0;
  s_compose_len = 0;
  // Hide the DM contacts list (and the Contacts/Discovered ones in case
  // the chat is opened from those — would otherwise paint behind the
  // chat history).
  if (s_dm_list)         lv_obj_add_flag(s_dm_list,         LV_OBJ_FLAG_HIDDEN);
  if (s_contacts_list)   lv_obj_add_flag(s_contacts_list,   LV_OBJ_FLAG_HIDDEN);
  if (s_discovered_list) lv_obj_add_flag(s_discovered_list, LV_OBJ_FLAG_HIDDEN);
  if (s_chat_scroll) lv_obj_remove_flag(s_chat_scroll, LV_OBJ_FLAG_HIDDEN);
  if (s_chat_compose) lv_obj_remove_flag(s_chat_compose, LV_OBJ_FLAG_HIDDEN);
  if (s_chat_counter) lv_obj_remove_flag(s_chat_counter, LV_OBJ_FLAG_HIDDEN);

  lv_label_set_text(s_subscreen_title, s_dm_peer_name);

  // Encoder follows the chat widget. The history label is scrollable;
  // chat compose is keyboard-driven so encoder rotation only scrolls
  // history (handled in UITask::loop).
  lv_indev_t* enc = tpager_lvgl_get_encoder();
  if (enc && s_channel_group) {
    // Reuse the channel chat's group (no widgets bound — empty group is
    // fine; loop() takes over scroll handling for both modes).
    lv_indev_set_group(enc, s_channel_group);
    lv_group_set_editing(s_channel_group, false);
  }

  chat_history_render();
  chat_compose_render();
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
  s_dm_mode = false;        // make sure render takes the channel branch
  memset(s_dm_pubkey, 0, sizeof(s_dm_pubkey));
  if (channel_idx >= 0 && channel_idx < UI_MAX_CHANNELS) {
    s_unread_ch[channel_idx] = 0;   // reading clears this channel's badge
    update_tile_badges();
  }
  // Restore persisted history from SD if the ring doesn't already hold it
  // (e.g. first open after a reboot).
  if (!chan_ring_has((uint8_t)channel_idx)) chan_persist_load((uint8_t)channel_idx);
  s_compose_buf[0] = 0;
  s_compose_len = 0;
  // Hide the channels list while the chat is active.
  if (s_channel_list) lv_obj_add_flag(s_channel_list, LV_OBJ_FLAG_HIDDEN);
  if (s_chat_scroll) lv_obj_remove_flag(s_chat_scroll, LV_OBJ_FLAG_HIDDEN);
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
  bool was_dm = s_dm_mode;
  s_chat_open = false;
  s_chat_channel_idx = -1;
  s_dm_mode = false;
  memset(s_dm_pubkey, 0, sizeof(s_dm_pubkey));
  if (s_chat_scroll) lv_obj_add_flag(s_chat_scroll, LV_OBJ_FLAG_HIDDEN);
  if (s_chat_compose) lv_obj_add_flag(s_chat_compose, LV_OBJ_FLAG_HIDDEN);
  if (s_chat_counter) lv_obj_add_flag(s_chat_counter, LV_OBJ_FLAG_HIDDEN);

  lv_indev_t* enc = tpager_lvgl_get_encoder();
  if (was_dm) {
    // Return to the DM contacts list inside the DM sub-screen.
    lv_label_set_text(s_subscreen_title, TILES[2].title);
    if (s_dm_list) lv_obj_remove_flag(s_dm_list, LV_OBJ_FLAG_HIDDEN);
    dm_list_populate();
    if (enc && s_dm_list_group) {
      lv_indev_set_group(enc, s_dm_list_group);
      lv_group_set_editing(s_dm_list_group, false);
    }
  } else {
    // Return to channels list within the Channels sub-screen.
    lv_label_set_text(s_subscreen_title, TILES[1].title);
    if (s_channel_list) lv_obj_remove_flag(s_channel_list, LV_OBJ_FLAG_HIDDEN);
    channels_list_populate(0);
    if (enc && s_channel_group) {
      lv_indev_set_group(enc, s_channel_group);
      lv_group_set_editing(s_channel_group, false);
    }
  }
}

// Append a message to the ring buffer. Older entries are evicted FIFO.
// ---- Channel chat-history SD persistence -----------------------------------
// One append-log per channel at /sd/ch_<idx>.dat. Each record is
// [uint8 len][len bytes of text]. Loaded back into the ring on chat open.
static bool s_chan_loading = false;   // suppress re-persist while loading

static void chan_persist_append(uint8_t idx, const char* text) {
  if (s_chan_loading || !text || !sd_init()) return;
  char path[24];
  snprintf(path, sizeof(path), "/sd/ch_%u.dat", (unsigned)idx);
  FILE* f = fopen(path, "ab");
  if (!f) return;
  size_t L = strlen(text);
  if (L > 95) L = 95;                 // matches ChatMsg.text[96]
  uint8_t len = (uint8_t)L;
  fwrite(&len, 1, 1, f);
  fwrite(text, 1, L, f);
  fclose(f);
}

static void chat_ring_push(uint8_t channel_idx, uint32_t timestamp, const char* text) {
  if (!ensure_chat_ring()) return;
  ChatMsg& slot = s_chat_ring[s_chat_head];
  slot.channel_idx = channel_idx;
  slot.timestamp = timestamp;
  size_t n = strlen(text);
  if (n >= sizeof(slot.text)) n = sizeof(slot.text) - 1;
  memcpy(slot.text, text, n);
  slot.text[n] = 0;
  s_chat_head = (s_chat_head + 1) % CHAT_RING_SIZE;
  if (s_chat_count < CHAT_RING_SIZE) s_chat_count++;
  chan_persist_append(channel_idx, text);
}

// Load a channel's persisted history into the ring (oldest-first), keeping
// only the last CHAT_RING_SIZE records. Called when opening a channel chat
// whose history isn't already in the ring (e.g. after reboot).
static void chan_persist_load(uint8_t idx) {
  if (!sd_init()) return;
  char path[24];
  snprintf(path, sizeof(path), "/sd/ch_%u.dat", (unsigned)idx);
  FILE* f = fopen(path, "rb");
  if (!f) return;
  static char ring[CHAT_RING_SIZE][96];
  int n = 0;
  uint8_t len;
  while (fread(&len, 1, 1, f) == 1) {
    if (len > 95) break;
    char buf[96];
    if (fread(buf, 1, len, f) != len) break;
    buf[len] = 0;
    memcpy(ring[n % CHAT_RING_SIZE], buf, len + 1);
    n++;
  }
  fclose(f);
  int start = n > CHAT_RING_SIZE ? n - CHAT_RING_SIZE : 0;
  s_chan_loading = true;
  for (int i = start; i < n; i++) chat_ring_push(idx, 0, ring[i % CHAT_RING_SIZE]);
  s_chan_loading = false;
}

// True if the ring already holds at least one message for this channel.
static bool chan_ring_has(uint8_t idx) {
  if (!s_chat_ring) return false;
  for (int i = 0; i < s_chat_count; i++)
    if (s_chat_ring[i].channel_idx == idx) return true;
  return false;
}

// Called by MyMesh::onChannelMessageRecv. Single-threaded with the LVGL
// rendering, so we can update the UI directly when the active chat
// matches.
extern "C" void ui_on_channel_message(int channel_idx, uint32_t timestamp, const char* text) {
  if (!text) return;
  chat_ring_push((uint8_t)channel_idx, timestamp, text);
  bool viewing = s_chat_open && !s_dm_mode && s_chat_channel_idx == channel_idx;
  if (viewing) {
    chat_history_render();
  } else if (channel_idx >= 0 && channel_idx < UI_MAX_CHANNELS) {
    if (s_unread_ch[channel_idx] < 0xFFFF) s_unread_ch[channel_idx]++;
    update_tile_badges();
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
  ui_ref_loc(&self_lat, &self_lon);   // GPS, else saved Home (item 8)
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
    lv_label_set_text(empty, "No favorites yet.\nOpen Discovered to add.");
    lv_obj_set_style_text_color(empty, lv_color_hex(0x707880), 0);
    return;
  }

  // Click handler for contact rows — opens a 3-option popup. The
  // handler only needs the pub_key, so we cache just that per row to
  // stay light on DRAM (UiContact is ~80 bytes; pub_key is 32).
  static auto contact_row_clicked = +[](lv_event_t* e) {
    const uint8_t* pk = (const uint8_t*)lv_event_get_user_data(e);
    if (!pk) return;
    con_menu_show(pk);
  };
  // 6-entry cap on the click-popup pub_key cache (DRAM is at 88%+ on
  // this build, can't afford a bigger static buffer). Above this you
  // can still see + scroll the rows, the popup just won't open — and
  // long-press still works to go back.
  static uint8_t s_con_row_pubkeys[6][32];

  for (int i = 0; i < count; i++) {
    UiContact ci;
    if (!ui_get_contact_info(i, &ci)) continue;
    if (i < (int)(sizeof(s_con_row_pubkeys)/sizeof(s_con_row_pubkeys[0]))) {
      memcpy(s_con_row_pubkeys[i], ci.pub_key, 32);
    }

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
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);  // single-click (item 6)
    lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                                LV_FLEX_ALIGN_CENTER,
                                LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_add_event_cb(row, back_long_press_event, LV_EVENT_LONG_PRESSED, NULL);
    if (i < (int)(sizeof(s_con_row_pubkeys)/sizeof(s_con_row_pubkeys[0]))) {
      lv_obj_add_event_cb(row, contact_row_clicked, LV_EVENT_CLICKED,
                          s_con_row_pubkeys[i]);
    }

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

// ---------- DM tile (S3.6d) -------------------------------------------------
//
// Lists chat-type contacts (any contact with ADV_TYPE_CHAT, regardless of
// favorite flag). Clicking a row opens the chat view in DM mode —
// history pulled from the per-contact-tagged ring buffer in main.cpp.

static void dm_row_clicked(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  UiContact ci;
  if (!ui_get_dm_contact_info || !ui_get_dm_contact_info(idx, &ci)) return;
  dm_chat_view_open(ci.pub_key, ci.name);
}

static void dm_list_populate() {
  if (!s_dm_list || !ui_get_dm_contact_count) return;
  lv_obj_clean(s_dm_list);
  if (s_dm_list_group) lv_group_remove_all_objs(s_dm_list_group);

  int count = ui_get_dm_contact_count();
  if (count == 0) {
    lv_obj_t* empty = lv_label_create(s_dm_list);
    lv_label_set_text(empty,
        "No DM contacts.\nOpen Discovered to add a chat node.");
    lv_obj_set_style_text_color(empty, lv_color_hex(0x707880), 0);
    return;
  }

  for (int i = 0; i < count; i++) {
    UiContact ci;
    if (!ui_get_dm_contact_info(i, &ci)) continue;

    lv_obj_t* row = lv_obj_create(s_dm_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 26);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1c2530), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x2b3742), LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(row, lv_color_hex(0xc0c8d0), 0);
    lv_obj_set_style_text_color(row, lv_color_hex(0xFAA61A), LV_STATE_FOCUSED);
    lv_obj_set_style_pad_hor(row, 6, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_margin_bottom(row, 1, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);  // single-click (item 6)
    lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(row, back_long_press_event, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(row, dm_row_clicked, LV_EVENT_CLICKED,
                        (void*)(intptr_t)i);

    uint16_t unread = dm_unread_get(ci.pub_key);

    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(lbl, lv_pct(unread > 0 ? 80 : 95));
    lv_label_set_text(lbl, ci.name);

    // Per-DM unread count (item 7), right-aligned in the row.
    if (unread > 0) {
      lv_obj_t* cnt = lv_label_create(row);
      char cbuf[8];
      snprintf(cbuf, sizeof(cbuf), "%u", (unsigned)unread);
      lv_label_set_text(cnt, cbuf);
      lv_obj_set_style_bg_color(cnt, lv_color_hex(0xe05050), 0);
      lv_obj_set_style_bg_opa(cnt, LV_OPA_COVER, 0);
      lv_obj_set_style_text_color(cnt, lv_color_hex(0xffffff), 0);
      lv_obj_set_style_radius(cnt, 8, 0);
      lv_obj_set_style_pad_hor(cnt, 5, 0);
      lv_obj_align(cnt, LV_ALIGN_RIGHT_MID, -2, 0);
    }

    if (s_dm_list_group) lv_group_add_obj(s_dm_list_group, row);
  }
}

// Helper for the receive-side hook in main.cpp: lets the UI refresh
// the open chat view's history when a new DM arrives for the current
// peer. Called from chat_history_render which checks s_chat_open
// + s_dm_mode + matching pubkey via a separate weak hook.
extern "C" void ui_refresh_open_dm(const uint8_t* pub_key) {
  bool viewing = s_chat_open && s_dm_mode &&
                 memcmp(s_dm_pubkey, pub_key, 32) == 0;
  if (viewing) {
    chat_history_render();
  } else {
    // New DM for a peer we're not currently looking at — bump its unread
    // counter (keyed by the first 8 bytes of the pub_key) + refresh badges.
    dm_unread_bump(pub_key);
    update_tile_badges();
  }
}

// ---------- Discovered tile (S3.6b) -----------------------------------------
//
// Same column layout as Contacts but driven by the ring buffer in
// companion_radio/main.cpp instead of contacts[]. Lists EVERY heard
// advert, including chats that the auto-add filter dropped. Click on a
// row opens a 3-button popup: [Add to contacts] / [Add as favorite] /
// [Cancel].

static void disc_row_clicked(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  disc_menu_show(idx);
}

// ---- Toast (item 2) — transient centered message, auto-closes after 3 s ----
static void toast_timer_cb(lv_timer_t* t) {
  if (s_toast_popup) lv_obj_add_flag(s_toast_popup, LV_OBJ_FLAG_HIDDEN);
  lv_timer_delete(t);
  s_toast_timer = nullptr;
}

static void ui_toast(const char* msg) {
  if (!s_toast_popup || !s_toast_label) return;
  lv_label_set_text(s_toast_label, msg);
  lv_obj_remove_flag(s_toast_popup, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_toast_popup);
  // Restart the 3 s one-shot if a previous toast is still up. The callback
  // deletes the timer itself (so no repeat-count auto-delete — that would
  // double-free).
  if (s_toast_timer) lv_timer_delete(s_toast_timer);
  s_toast_timer = lv_timer_create(toast_timer_cb, 3000, nullptr);
}

// Advert button click (item 2). user_data carries 0 for direct (zero-hop),
// 1 for flood. Fires the self-advert and shows a 3 s confirmation toast.
static void advert_btn_clicked(lv_event_t* e) {
  bool flood = (bool)(intptr_t)lv_event_get_user_data(e);
  bool ok = ui_send_self_advert ? ui_send_self_advert(flood) : false;
  if (ok) {
    ui_toast(flood ? "Advert verstuurd\n(flood)" : "Advert verstuurd\n(direct)");
  } else {
    ui_toast("Advert mislukt");
  }
}

static void discovered_list_populate() {
  if (!s_discovered_list || !ui_get_discovered_count) return;
  lv_obj_clean(s_discovered_list);
  if (s_discovered_group) lv_group_remove_all_objs(s_discovered_group);

  double self_lat = 0, self_lon = 0;
  ui_ref_loc(&self_lat, &self_lon);   // GPS, else saved Home (item 8)

  // (Send-advert moved to its own "Advert" carousel tile — item 2.)

  // Header (same widths as Contacts — column constants are shared).
  {
    lv_obj_t* hdr = lv_obj_create(s_discovered_list);
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
      {"hops", CON_COL_DIST},  {"age",  CON_COL_AGE },
    };
    for (auto& c : cols) {
      lv_obj_t* l = lv_label_create(hdr);
      lv_obj_set_width(l, c.w);
      lv_label_set_text(l, c.text);
    }
  }

  int count = ui_get_discovered_count();
  if (count == 0) {
    lv_obj_t* empty = lv_label_create(s_discovered_list);
    lv_label_set_text(empty, "(no adverts heard yet)");
    lv_obj_set_style_text_color(empty, lv_color_hex(0x707880), 0);
    return;
  }

  uint32_t now_ms = millis();
  for (int i = 0; i < count; i++) {
    UiContact ci;
    if (!ui_get_discovered_info(i, &ci)) continue;

    lv_obj_t* row = lv_obj_create(s_discovered_list);
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
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);  // single-click (item 6)
    lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                                LV_FLEX_ALIGN_CENTER,
                                LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_add_event_cb(row, back_long_press_event, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(row, disc_row_clicked, LV_EVENT_CLICKED,
                        (void*)(intptr_t)i);

    lv_obj_t* lbl_role = lv_label_create(row);
    lv_obj_set_width(lbl_role, CON_COL_ROLE);
    lv_label_set_text(lbl_role, role_short(ci.type));
    lv_obj_set_style_text_color(lbl_role, lv_color_hex(0xFAA61A), 0);

    lv_obj_t* lbl_name = lv_label_create(row);
    lv_obj_set_width(lbl_name, CON_COL_NAME);
    lv_label_set_long_mode(lbl_name, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(lbl_name, ci.name);

    lv_obj_t* lbl_snr = lv_label_create(row);
    lv_obj_set_width(lbl_snr, CON_COL_SNR);
    char snr_buf[12];
    snprintf(snr_buf, sizeof(snr_buf), "%+.1f", (float)ci.snr_q / 4.0f);
    lv_label_set_text(lbl_snr, snr_buf);

    lv_obj_t* lbl_rssi = lv_label_create(row);
    lv_obj_set_width(lbl_rssi, CON_COL_RSSI);
    char rssi_buf[12];
    snprintf(rssi_buf, sizeof(rssi_buf), "%d", (int)ci.rssi);
    lv_label_set_text(lbl_rssi, rssi_buf);

    // For Discovered we show hop count (0 = direct) instead of distance —
    // useful signal of mesh reach. Discovered entries usually lack GPS.
    lv_obj_t* lbl_hops = lv_label_create(row);
    lv_obj_set_width(lbl_hops, CON_COL_DIST);
    char hop_buf[8];
    snprintf(hop_buf, sizeof(hop_buf), "%u", (unsigned)ci.path_len);
    lv_label_set_text(lbl_hops, hop_buf);
    lv_obj_set_style_text_color(lbl_hops, lv_color_hex(0x80868f), 0);

    // Age based on last_advert which we store as "ms-since-boot / 1000"
    // for discovered entries (no NTP needed). Render as "Ns / Nm" etc.
    lv_obj_t* lbl_age = lv_label_create(row);
    lv_obj_set_width(lbl_age, CON_COL_AGE);
    char age_buf[12];
    uint32_t age_s = (now_ms / 1000) - ci.last_advert;
    if      (age_s < 60)    snprintf(age_buf, sizeof(age_buf), "%us", age_s);
    else if (age_s < 3600)  snprintf(age_buf, sizeof(age_buf), "%um", age_s / 60);
    else                    snprintf(age_buf, sizeof(age_buf), "%uh", age_s / 3600);
    lv_label_set_text(lbl_age, age_buf);
    lv_obj_set_style_text_color(lbl_age, lv_color_hex(0x80868f), 0);

    if (s_discovered_group) lv_group_add_obj(s_discovered_group, row);
  }
}

// 3-option popup for clicked Discovered row. Maintained as a single
// reused container with three buttons; show/hide instead of recreating
// to keep the focus/encoder bind cycle deterministic.
static void disc_menu_close() {
  if (s_disc_menu_popup) lv_obj_add_flag(s_disc_menu_popup, LV_OBJ_FLAG_HIDDEN);
  s_disc_menu_pubidx = -1;
  // Restore encoder to the Discovered list.
  lv_indev_t* enc = tpager_lvgl_get_encoder();
  if (enc && s_discovered_group) {
    lv_group_set_editing(s_discovered_group, false);
    lv_indev_set_group(enc, s_discovered_group);
  }
}

static void disc_menu_show(int disc_idx) {
  if (!s_disc_menu_popup) return;
  s_disc_menu_pubidx = disc_idx;
  lv_obj_remove_flag(s_disc_menu_popup, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_disc_menu_popup);
  lv_indev_t* enc = tpager_lvgl_get_encoder();
  if (enc && s_disc_menu_group) {
    lv_indev_set_group(enc, s_disc_menu_group);
    if (s_disc_menu_buttons[0]) lv_group_focus_obj(s_disc_menu_buttons[0]);
  }
}

// Contact-row popup (S3.6c) — Open DM (placeholder) / Unmark favorite /
// Cancel. Caller passes the contact's full pub_key so the unmark action
// can call ui_toggle_favorite() directly without re-iterating contacts[].
static void con_menu_close() {
  if (s_con_menu_popup) lv_obj_add_flag(s_con_menu_popup, LV_OBJ_FLAG_HIDDEN);
  memset(s_con_menu_pub_key, 0, sizeof(s_con_menu_pub_key));
  lv_indev_t* enc = tpager_lvgl_get_encoder();
  if (enc && s_contacts_group) {
    lv_group_set_editing(s_contacts_group, false);
    lv_indev_set_group(enc, s_contacts_group);
  }
}

static void con_menu_show(const uint8_t* pub_key) {
  if (!s_con_menu_popup || !pub_key) return;
  memcpy(s_con_menu_pub_key, pub_key, 32);
  lv_obj_remove_flag(s_con_menu_popup, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_con_menu_popup);
  lv_indev_t* enc = tpager_lvgl_get_encoder();
  if (enc && s_con_menu_group) {
    lv_indev_set_group(enc, s_con_menu_group);
    if (s_con_menu_buttons[0]) lv_group_focus_obj(s_con_menu_buttons[0]);
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
    case 8: {
      // Node name. Persisted via dedicated bridge that calls savePrefs.
      if (ui_set_node_name) ui_set_node_name(s_text_buf);
      break;
    }
    default: break;
  }

  int idx_was = s_editing_idx;
  // Skip the radio-params reapply for scope + name — both go through
  // their own savePrefs path and re-running radio_set_params is redundant.
  if (s_editing_idx != 7 && s_editing_idx != 8 && ui_apply_radio_changes)
    ui_apply_radio_changes();

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
  //   idx 8          → text-input (Name, mixed-case + digits + '-' + '_')
  bool is_spinbox  = (idx == 1 || idx == 3 || idx == 4);
  bool is_dropdown = (idx == 2 || idx == 6);
  bool is_text     = (idx == 0 || idx == 7 || idx == 8);
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
    } else if (idx == 7) {
      snprintf(s_text_buf, sizeof(s_text_buf), "%s", p->default_scope_name);
      lv_label_set_text(s_edit_title, "Region scope");
    } else {  // idx == 8, node name
      snprintf(s_text_buf, sizeof(s_text_buf), "%s", p->node_name);
      lv_label_set_text(s_edit_title, "Node name");
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

// ---------- Settings tile (item 8) ------------------------------------------
//
// A list of action rows. Home save/clear is the core feature (distance
// reference); the rest are shortcuts / a software screen-sleep saver.
// Backlight dimming + haptics need new AW9364/DRV2605 drivers not present
// in this firmware, so they are intentionally omitted for now.
enum {
  SET_SAVE_HOME = 0,
  SET_HOME_INFO,      // click = clear home
  SET_SCREEN_OFF,     // click = cycle timeout
  SET_TIME,           // display (UTC) — click refreshes
};

static void settings_row_clicked(lv_event_t* e) {
  int act = (int)(intptr_t)lv_event_get_user_data(e);
  switch (act) {
    case SET_SAVE_HOME: {
      double lat = 0, lon = 0;
      if (ui_get_self_loc) ui_get_self_loc(&lat, &lon);
      if ((lat || lon) && ui_set_home_loc) {
        ui_set_home_loc(lat, lon);
        ui_toast("Home opgeslagen");
      } else {
        ui_toast("Geen GPS-fix\nGeen home opgeslagen");
      }
      break;
    }
    case SET_HOME_INFO:
      if (ui_set_home_loc) { ui_set_home_loc(0, 0); ui_toast("Home gewist"); }
      break;
    case SET_SCREEN_OFF:
      if      (s_screen_off_ms <  60000UL)  s_screen_off_ms =  60000UL;
      else if (s_screen_off_ms < 120000UL)  s_screen_off_ms = 120000UL;
      else if (s_screen_off_ms < 300000UL)  s_screen_off_ms = 300000UL;
      else                                   s_screen_off_ms =  30000UL;
      break;
    default:
      break;
  }
  settings_list_populate();   // refresh dynamic labels
}

static void settings_list_populate() {
  if (!s_settings_list) return;
  lv_obj_clean(s_settings_list);
  if (s_settings_group) lv_group_remove_all_objs(s_settings_group);

  auto build_row = [&](const char* text, int act) -> lv_obj_t* {
    lv_obj_t* row = lv_obj_create(s_settings_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 26);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1c2530), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x2b3742), LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(row, lv_color_hex(0xc0c8d0), 0);
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
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);  // single-click (item 6)
    lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);
    lv_obj_add_event_cb(row, settings_row_clicked, LV_EVENT_CLICKED,
                        (void*)(intptr_t)act);
    lv_obj_add_event_cb(row, back_long_press_event, LV_EVENT_LONG_PRESSED, NULL);
    if (s_settings_group) lv_group_add_obj(s_settings_group, row);
    return row;
  };

  char buf[48];

  lv_obj_t* first = build_row("Save home = current GPS", SET_SAVE_HOME);

  double hlat = 0, hlon = 0;
  bool have_home = ui_get_home_loc && ui_get_home_loc(&hlat, &hlon);
  if (have_home) {
    snprintf(buf, sizeof(buf), "Home: %.4f, %.4f  (clear)", hlat, hlon);
  } else {
    snprintf(buf, sizeof(buf), "Home: (not set)");
  }
  build_row(buf, SET_HOME_INFO);

  snprintf(buf, sizeof(buf), "Screen off: %lus", s_screen_off_ms / 1000UL);
  build_row(buf, SET_SCREEN_OFF);

  uint32_t ep = ui_get_now_epoch ? ui_get_now_epoch() : 0;
  if (ep > 0) {
    snprintf(buf, sizeof(buf), "Time UTC: %02u:%02u:%02u",
             (unsigned)((ep / 3600) % 24), (unsigned)((ep / 60) % 60),
             (unsigned)(ep % 60));
  } else {
    snprintf(buf, sizeof(buf), "Time UTC: (no clock)");
  }
  build_row(buf, SET_TIME);

  lv_group_focus_obj(first);
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

    // Unread badge (item 7) — only on the Channels + DM tiles. Small red
    // pill pinned to the top-right corner of the tile, hidden until there
    // is unread traffic.
    if (i == TILE_CHANNELS || i == TILE_DM) {
      lv_obj_t* badge = lv_label_create(btn);
      lv_obj_add_flag(badge, LV_OBJ_FLAG_IGNORE_LAYOUT);  // absolute, not flex
      lv_label_set_text(badge, "");
      lv_obj_set_style_bg_color(badge, lv_color_hex(0xe05050), 0);
      lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
      lv_obj_set_style_text_color(badge, lv_color_hex(0xffffff), 0);
      lv_obj_set_style_radius(badge, 8, 0);
      lv_obj_set_style_pad_hor(badge, 5, 0);
      lv_obj_set_style_pad_ver(badge, 1, 0);
      lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -4, 4);
      lv_obj_add_flag(badge, LV_OBJ_FLAG_HIDDEN);
      s_tile_badges[i] = badge;
    }
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

  // Footer hint (the duplicated tile title stays removed; only this control
  // hint sits under the carousel).
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

  // Thin horizontal rules above and below the title (item 3) — improves
  // readability of "line 1 = node name / line 2 = tile title". Anchored to
  // the subscreen root so they render on every subscreen that shows a title.
  for (int yk = 0; yk < 2; yk++) {
    lv_obj_t* rule = lv_obj_create(s_subscreen_root);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, lv_pct(96), 2);
    lv_obj_align(rule, LV_ALIGN_TOP_LEFT, 6, yk == 0 ? 20 : 42);
    lv_obj_set_style_bg_color(rule, lv_color_hex(0x404a55), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    s_title_rule[yk] = rule;
  }

  s_subscreen_body = lv_label_create(s_subscreen_root);
  lv_label_set_text(s_subscreen_body, "");
  lv_obj_set_style_text_color(s_subscreen_body, lv_color_hex(0xc0c8d0), 0);
  lv_obj_set_width(s_subscreen_body, lv_pct(96));
  lv_label_set_long_mode(s_subscreen_body, LV_LABEL_LONG_WRAP);
  lv_obj_align(s_subscreen_body, LV_ALIGN_TOP_LEFT, 6, 48);

  // Map view container — built once, hidden until the Map tile opens.
  map_screen_create(s_subscreen_root);

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

  // DM list (S3.6d) — chat-type contacts. Click row → open chat thread.
  s_dm_list = lv_obj_create(s_subscreen_root);
  lv_obj_remove_style_all(s_dm_list);
  lv_obj_set_size(s_dm_list, lv_pct(96), lv_pct(70));
  lv_obj_align(s_dm_list, LV_ALIGN_TOP_LEFT, 6, 44);
  lv_obj_set_flex_flow(s_dm_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_bg_color(s_dm_list, lv_color_hex(0x0e141b), 0);
  lv_obj_set_style_pad_all(s_dm_list, 2, 0);
  lv_obj_set_scroll_dir(s_dm_list, LV_DIR_VER);
  lv_obj_add_flag(s_dm_list, LV_OBJ_FLAG_HIDDEN);
  s_dm_list_group = lv_group_create();

  // Discovered list (S3.6b) — same row pattern as channels/contacts list.
  // Driven by the ring buffer in companion_radio/main.cpp; populated on
  // each enter + every 5s while open. Listens to ui_on_advert_seen.
  s_discovered_list = lv_obj_create(s_subscreen_root);
  lv_obj_remove_style_all(s_discovered_list);
  lv_obj_set_size(s_discovered_list, lv_pct(96), lv_pct(70));
  lv_obj_align(s_discovered_list, LV_ALIGN_TOP_LEFT, 6, 44);
  lv_obj_set_flex_flow(s_discovered_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_bg_color(s_discovered_list, lv_color_hex(0x0e141b), 0);
  lv_obj_set_style_pad_all(s_discovered_list, 2, 0);
  lv_obj_set_scroll_dir(s_discovered_list, LV_DIR_VER);
  lv_obj_add_flag(s_discovered_list, LV_OBJ_FLAG_HIDDEN);

  s_discovered_group = lv_group_create();

  // Advert subscreen (item 2) — two big buttons: Direct / Flood. Built once,
  // hidden by default; shown by enter_subscreen(TILE_ADVERT).
  s_advert_root = lv_obj_create(s_subscreen_root);
  lv_obj_remove_style_all(s_advert_root);
  lv_obj_set_size(s_advert_root, lv_pct(96), lv_pct(70));
  lv_obj_align(s_advert_root, LV_ALIGN_TOP_LEFT, 6, 44);
  lv_obj_set_flex_flow(s_advert_root, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_advert_root, LV_FLEX_ALIGN_CENTER,
                                       LV_FLEX_ALIGN_CENTER,
                                       LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(s_advert_root, 16, 0);
  lv_obj_set_style_bg_color(s_advert_root, lv_color_hex(0x0e141b), 0);
  lv_obj_add_flag(s_advert_root, LV_OBJ_FLAG_HIDDEN);
  s_advert_group = lv_group_create();
  {
    static const struct { const char* text; bool flood; } advert_btns[] = {
      { "Direct advert", false },
      { "Flood advert",  true  },
    };
    for (auto& ab : advert_btns) {
      lv_obj_t* btn = lv_button_create(s_advert_root);
      lv_obj_set_size(btn, lv_pct(80), 48);
      style_popup_button(btn);
      lv_obj_t* lbl = lv_label_create(btn);
      lv_label_set_text(lbl, ab.text);
      lv_obj_center(lbl);
      lv_obj_add_event_cb(btn, advert_btn_clicked, LV_EVENT_CLICKED,
                          (void*)(intptr_t)(ab.flood ? 1 : 0));
      lv_obj_add_event_cb(btn, back_long_press_event, LV_EVENT_LONG_PRESSED, NULL);
      lv_group_add_obj(s_advert_group, btn);
    }
  }

  // Settings list (item 8) — action rows, same shape as the radio list.
  s_settings_list = lv_obj_create(s_subscreen_root);
  lv_obj_remove_style_all(s_settings_list);
  lv_obj_set_size(s_settings_list, lv_pct(96), lv_pct(70));
  lv_obj_align(s_settings_list, LV_ALIGN_TOP_LEFT, 6, 44);
  lv_obj_set_flex_flow(s_settings_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_bg_color(s_settings_list, lv_color_hex(0x0e141b), 0);
  lv_obj_set_style_pad_all(s_settings_list, 2, 0);
  lv_obj_set_scroll_dir(s_settings_list, LV_DIR_VER);
  lv_obj_add_flag(s_settings_list, LV_OBJ_FLAG_HIDDEN);
  s_settings_group = lv_group_create();

  // Chat-view widgets — history label (top, scrollable) + compose
  // label (bottom). Both hidden until chat_view_open() shows them.
  // Chat history is a spangroup so individual messages can mix colors
  // (green sender prefix + grey body). Spans are rebuilt by
  // chat_history_render on every change.
  // A spangroup itself is not scrollable (its spans aren't child objects,
  // so lv_obj sees no scrollable extent). Wrap it in a scrollable lv_obj
  // and scroll that with the encoder; the spangroup grows to its content.
  s_chat_scroll = lv_obj_create(s_subscreen_root);
  lv_obj_remove_style_all(s_chat_scroll);
  lv_obj_set_size(s_chat_scroll, lv_pct(96), lv_pct(60));
  lv_obj_align(s_chat_scroll, LV_ALIGN_TOP_LEFT, 6, 44);
  lv_obj_set_style_bg_color(s_chat_scroll, lv_color_hex(0x0e141b), 0);
  lv_obj_set_style_bg_opa(s_chat_scroll, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(s_chat_scroll, 0, 0);
  lv_obj_add_flag(s_chat_scroll, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(s_chat_scroll, LV_DIR_VER);
  lv_obj_add_flag(s_chat_scroll, LV_OBJ_FLAG_HIDDEN);

  s_chat_history = lv_spangroup_create(s_chat_scroll);
  lv_obj_remove_style_all(s_chat_history);
  lv_obj_set_width(s_chat_history, lv_pct(100));
  lv_obj_set_height(s_chat_history, LV_SIZE_CONTENT);
  lv_obj_set_pos(s_chat_history, 0, 0);
  lv_spangroup_set_mode(s_chat_history, LV_SPAN_MODE_BREAK);
  lv_obj_set_style_text_color(s_chat_history, lv_color_hex(0xc0c8d0), 0);

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
  // Same muted-grey as the DC label so the persistent top row reads as
  // one cohesive status strip instead of three competing colors.
  lv_obj_set_style_text_color(s_header_label, lv_color_hex(0x80868f), 0);
  lv_obj_align(s_header_label, LV_ALIGN_TOP_LEFT, 6, 4);

  s_dc_label = lv_label_create(scr);
  lv_label_set_text(s_dc_label, "");
  lv_obj_set_style_text_color(s_dc_label, lv_color_hex(0x80868f), 0);
  lv_obj_align(s_dc_label, LV_ALIGN_TOP_MID, 0, 4);

  s_battery_label = lv_label_create(scr);
  lv_label_set_text(s_battery_label, "");
  lv_obj_set_style_text_color(s_battery_label, lv_color_hex(0xc0c8d0), 0);
  lv_obj_align(s_battery_label, LV_ALIGN_TOP_RIGHT, -6, 4);

  // Toast popup (item 2) — small centered label, hidden until ui_toast().
  s_toast_popup = lv_obj_create(scr);
  lv_obj_set_size(s_toast_popup, lv_pct(70), LV_SIZE_CONTENT);
  lv_obj_center(s_toast_popup);
  lv_obj_set_style_bg_color(s_toast_popup, lv_color_hex(0x1c2530), 0);
  lv_obj_set_style_border_width(s_toast_popup, 2, 0);
  lv_obj_set_style_border_color(s_toast_popup, lv_color_hex(0xFAA61A), 0);
  lv_obj_set_style_radius(s_toast_popup, 8, 0);
  lv_obj_set_style_pad_all(s_toast_popup, 14, 0);
  lv_obj_clear_flag(s_toast_popup, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_toast_popup, LV_OBJ_FLAG_HIDDEN);
  s_toast_label = lv_label_create(s_toast_popup);
  lv_label_set_text(s_toast_label, "");
  lv_obj_set_style_text_color(s_toast_label, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_text_align(s_toast_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(s_toast_label);

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

  // Discovered click-popup (S3.6b). Reused container; hidden by default,
  // shown by disc_menu_show() with the row index baked in s_disc_menu_pubidx.
  s_disc_menu_popup = lv_obj_create(scr);
  lv_obj_set_size(s_disc_menu_popup, lv_pct(80), 130);
  lv_obj_center(s_disc_menu_popup);
  lv_obj_set_style_bg_color(s_disc_menu_popup, lv_color_hex(0x1c2530), 0);
  lv_obj_set_style_border_width(s_disc_menu_popup, 2, 0);
  lv_obj_set_style_border_color(s_disc_menu_popup, lv_color_hex(0xFAA61A), 0);
  lv_obj_set_style_radius(s_disc_menu_popup, 8, 0);
  lv_obj_set_style_text_color(s_disc_menu_popup, lv_color_hex(0xffffff), 0);
  lv_obj_set_flex_flow(s_disc_menu_popup, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_disc_menu_popup, LV_FLEX_ALIGN_SPACE_EVENLY,
                                          LV_FLEX_ALIGN_CENTER,
                                          LV_FLEX_ALIGN_CENTER);
  lv_obj_add_flag(s_disc_menu_popup, LV_OBJ_FLAG_HIDDEN);

  s_disc_menu_group = lv_group_create();
  static const char* btn_labels[3] = {
    "Add to contacts",
    "Add as favorite",
    "Cancel",
  };
  for (int b = 0; b < 3; b++) {
    s_disc_menu_buttons[b] = lv_button_create(s_disc_menu_popup);
    lv_obj_set_size(s_disc_menu_buttons[b], lv_pct(80), 26);
    style_popup_button(s_disc_menu_buttons[b]);
    lv_obj_t* lbl = lv_label_create(s_disc_menu_buttons[b]);
    lv_label_set_text(lbl, btn_labels[b]);
    lv_obj_center(lbl);
    lv_group_add_obj(s_disc_menu_group, s_disc_menu_buttons[b]);
    lv_obj_add_event_cb(s_disc_menu_buttons[b], [](lv_event_t* e) {
      lv_obj_t* tgt = (lv_obj_t*)lv_event_get_target(e);
      int which = -1;
      for (int i = 0; i < 3; i++) if (s_disc_menu_buttons[i] == tgt) which = i;
      if (which < 0 || s_disc_menu_pubidx < 0) { disc_menu_close(); return; }
      bool ok = false;
      if (which == 0 && ui_add_discovered_to_contacts) {
        ok = ui_add_discovered_to_contacts(s_disc_menu_pubidx, false);
      } else if (which == 1 && ui_add_discovered_to_contacts) {
        ok = ui_add_discovered_to_contacts(s_disc_menu_pubidx, true);
      } else {
        ok = true;  // Cancel
      }
      (void)ok;
      disc_menu_close();
      discovered_list_populate();   // reflect new state (e.g. row could be
                                    // marked already-added next time)
    }, LV_EVENT_CLICKED, nullptr);
  }

  // Contacts click-popup (S3.6c). Same shape as the discovered popup but
  // with Open-DM / Unmark-fav / Cancel actions. Open DM is a phase-d
  // placeholder for now.
  s_con_menu_popup = lv_obj_create(scr);
  lv_obj_set_size(s_con_menu_popup, lv_pct(80), 130);
  lv_obj_center(s_con_menu_popup);
  lv_obj_set_style_bg_color(s_con_menu_popup, lv_color_hex(0x1c2530), 0);
  lv_obj_set_style_border_width(s_con_menu_popup, 2, 0);
  lv_obj_set_style_border_color(s_con_menu_popup, lv_color_hex(0xFAA61A), 0);
  lv_obj_set_style_radius(s_con_menu_popup, 8, 0);
  lv_obj_set_style_text_color(s_con_menu_popup, lv_color_hex(0xffffff), 0);
  lv_obj_set_flex_flow(s_con_menu_popup, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_con_menu_popup, LV_FLEX_ALIGN_SPACE_EVENLY,
                                         LV_FLEX_ALIGN_CENTER,
                                         LV_FLEX_ALIGN_CENTER);
  lv_obj_add_flag(s_con_menu_popup, LV_OBJ_FLAG_HIDDEN);

  s_con_menu_group = lv_group_create();
  static const char* con_btn_labels[3] = {
    "Open DM",
    "Unmark favorite",
    "Cancel",
  };
  for (int b = 0; b < 3; b++) {
    s_con_menu_buttons[b] = lv_button_create(s_con_menu_popup);
    lv_obj_set_size(s_con_menu_buttons[b], lv_pct(80), 26);
    style_popup_button(s_con_menu_buttons[b]);
    lv_obj_t* lbl = lv_label_create(s_con_menu_buttons[b]);
    lv_label_set_text(lbl, con_btn_labels[b]);
    lv_obj_center(lbl);
    lv_group_add_obj(s_con_menu_group, s_con_menu_buttons[b]);
    lv_obj_add_event_cb(s_con_menu_buttons[b], [](lv_event_t* e) {
      lv_obj_t* tgt = (lv_obj_t*)lv_event_get_target(e);
      int which = -1;
      for (int i = 0; i < 3; i++) if (s_con_menu_buttons[i] == tgt) which = i;
      if (which == 0) {
        // Look up the contact name for the popup title and open DM.
        char peer_name[32] = "?";
        if (ui_get_contact_count && ui_get_contact_info) {
          int n = ui_get_contact_count();
          for (int i = 0; i < n; i++) {
            UiContact ci;
            if (!ui_get_contact_info(i, &ci)) continue;
            if (memcmp(ci.pub_key, s_con_menu_pub_key, 32) == 0) {
              strncpy(peer_name, ci.name, sizeof(peer_name) - 1);
              peer_name[sizeof(peer_name) - 1] = 0;
              break;
            }
          }
        }
        uint8_t pk[32];
        memcpy(pk, s_con_menu_pub_key, 32);
        con_menu_close();
        dm_chat_view_open(pk, peer_name);
        return;
      }
      if (which == 1 && ui_toggle_favorite) {
        ui_toggle_favorite(s_con_menu_pub_key);
      }
      con_menu_close();
      contacts_list_populate();
    }, LV_EVENT_CLICKED, nullptr);
  }

  // Channels click-popup (S3.6 round 2). Open chat / Delete channel /
  // Cancel. "Public" (slot 0) skips the popup — chat opens immediately
  // — because deleting it would just be re-created at next boot.
  s_ch_menu_popup = lv_obj_create(scr);
  lv_obj_set_size(s_ch_menu_popup, lv_pct(80), 130);
  lv_obj_center(s_ch_menu_popup);
  lv_obj_set_style_bg_color(s_ch_menu_popup, lv_color_hex(0x1c2530), 0);
  lv_obj_set_style_border_width(s_ch_menu_popup, 2, 0);
  lv_obj_set_style_border_color(s_ch_menu_popup, lv_color_hex(0xFAA61A), 0);
  lv_obj_set_style_radius(s_ch_menu_popup, 8, 0);
  lv_obj_set_style_text_color(s_ch_menu_popup, lv_color_hex(0xffffff), 0);
  lv_obj_set_flex_flow(s_ch_menu_popup, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_ch_menu_popup, LV_FLEX_ALIGN_SPACE_EVENLY,
                                        LV_FLEX_ALIGN_CENTER,
                                        LV_FLEX_ALIGN_CENTER);
  lv_obj_add_flag(s_ch_menu_popup, LV_OBJ_FLAG_HIDDEN);

  s_ch_menu_group = lv_group_create();
  static const char* ch_btn_labels[3] = {
    "Open chat",
    "Delete channel",
    "Cancel",
  };
  for (int b = 0; b < 3; b++) {
    s_ch_menu_buttons[b] = lv_button_create(s_ch_menu_popup);
    lv_obj_set_size(s_ch_menu_buttons[b], lv_pct(80), 26);
    style_popup_button(s_ch_menu_buttons[b]);
    lv_obj_t* lbl = lv_label_create(s_ch_menu_buttons[b]);
    lv_label_set_text(lbl, ch_btn_labels[b]);
    lv_obj_center(lbl);
    lv_group_add_obj(s_ch_menu_group, s_ch_menu_buttons[b]);
    lv_obj_add_event_cb(s_ch_menu_buttons[b], [](lv_event_t* e) {
      lv_obj_t* tgt = (lv_obj_t*)lv_event_get_target(e);
      int which = -1;
      for (int i = 0; i < 3; i++) if (s_ch_menu_buttons[i] == tgt) which = i;
      int idx = s_ch_menu_idx;
      if (which == 0) {                          // Open chat
        ch_menu_close();
        if (idx >= 0) chat_view_open(idx);
        return;
      }
      if (which == 1 && ui_delete_channel) {     // Delete channel
        ui_delete_channel(idx);
      }
      ch_menu_close();
      channels_list_populate(0);
    }, LV_EVENT_CLICKED, nullptr);
  }

  s_back_hint = lv_label_create(s_subscreen_root);
  lv_label_set_text(s_back_hint, "click: open   long-press: back");
  lv_obj_set_style_text_color(s_back_hint, lv_color_hex(0x707880), 0);
  lv_obj_align(s_back_hint, LV_ALIGN_BOTTOM_MID, 0, -4);

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
  _auto_off = millis() + s_screen_off_ms;
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
      if (s_chat_open && s_chat_scroll) {
        // In chat view the encoder scrolls the history wrapper (the
        // spangroup grows inside it). Rotate up = scroll up.
        lv_obj_scroll_by(s_chat_scroll, 0, 28, LV_ANIM_OFF);
      } else {
        tpager_lvgl_encoder_delta(+1 * sign);
      }
      s_quad_accum -= 4;
    }
    while (s_quad_accum <= -4) {
      if (s_chat_open && s_chat_scroll) {
        lv_obj_scroll_by(s_chat_scroll, 0, -28, LV_ANIM_OFF);   // rotate down = scroll down
      } else {
        tpager_lvgl_encoder_delta(-1 * sign);
      }
      s_quad_accum += 4;
    }

    _auto_off = millis() + s_screen_off_ms;
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
    _auto_off = millis() + s_screen_off_ms;
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
  // Tile indices: 3 = Contacts (favorites), 4 = Discovered.
  if (s_active_tile == 3 && s_contacts_list &&
      (long)(millis() - s_contacts_next_refresh) >= 0) {
    s_contacts_next_refresh = millis() + 5000;
    contacts_list_populate();
  }
  if (s_active_tile == 4 && s_discovered_list &&
      (long)(millis() - s_discovered_next_refresh) >= 0) {
    s_discovered_next_refresh = millis() + 5000;
    discovered_list_populate();
  }
  // Map (item 5 / issue 3): follow the live fix. After the initial dwell on
  // Home (s_map_gps_switch_at), re-centre on GPS every ~2 s so the crosshair
  // + tiles track movement. Manual pan/zoom clears s_map_follow so the view
  // stays put until the Map tile is re-opened.
  if (s_active_tile == TILE_MAP && s_map_follow &&
      (s_map_gps_switch_at == 0 || (long)(millis() - s_map_gps_switch_at) >= 0) &&
      (long)(millis() - s_map_follow_next) >= 0) {
    s_map_follow_next = millis() + 2000;
    s_map_gps_switch_at = 0;
    double glat = 0, glon = 0;
    if (ui_get_self_loc) ui_get_self_loc(&glat, &glon);
    if (glat || glon) map_screen_set_center(glat, glon);
  }
  // Live sat-count refresh on the Map status strip (no tile reload).
  if (s_active_tile == TILE_MAP &&
      (long)(millis() - s_map_status_next) >= 0) {
    s_map_status_next = millis() + 2000;
    map_screen_refresh_status();
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
      int used_ms = 0, max_s = 0;
      if (ui_get_duty_cycle_seconds) ui_get_duty_cycle_seconds(&used_ms, &max_s);
      char buf[40];
      // used in ms, max in s. Display used as X.Xs (one decimal) so a
      // single 80-200 ms advert visibly moves the counter.
      int used_s_whole = used_ms / 1000;
      int used_tenths  = (used_ms / 100) % 10;
      snprintf(buf, sizeof(buf), "DC %d.%d%% %d.%ds/%ds",
               t / 10, t % 10, used_s_whole, used_tenths, max_s);
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
    update_tile_badges();   // keep unread badges in sync (item 7)
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
