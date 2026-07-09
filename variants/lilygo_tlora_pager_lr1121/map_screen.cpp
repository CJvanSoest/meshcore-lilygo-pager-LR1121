#include "map_screen.h"

#include "map_tiles.h"
#include "target.h"

#include <Arduino.h>
#include <lvgl.h>
#include <math.h>
#include <stdio.h>

// Active-theme colours, owned by UITask.cpp's palette. Only the backdrop and
// the "SD failed" fallback text follow the theme; the crosshair, node markers
// and the translucent status/hint strips stay fixed since they overlay the
// (always colourful) map tiles, not a themed surface.
extern "C" uint32_t tpager_pal_bg();
extern "C" uint32_t tpager_pal_text();

static lv_obj_t *s_root = nullptr;
static lv_obj_t *s_body = nullptr;         // text shown when no tile is up
static lv_obj_t *s_status_strip = nullptr; // top status bar

// 3x3 tile raster centred on the current lat/lon with sub-tile pixel offset,
// so the exact centre lands at the viewport centre (needed for an accurate
// position crosshair + node markers). Index = row*3 + col, centre = index 4.
static lv_obj_t *s_tiles[9] = { nullptr };
static char s_tile_path[9][map_tiles::PATH_BUF_MIN];
// Remember which (z, x, y) each slot currently shows so auto-follow can skip
// the expensive PNG re-decode when only the sub-tile offset changed.
static int s_tile_z[9] = { 0 };
static int s_tile_x[9] = { -1 };
static int s_tile_y[9] = { -1 };

// Position crosshair (own GPS location) — drawn at the viewport centre.
static lv_obj_t *s_cross_h = nullptr;
static lv_obj_t *s_cross_v = nullptr;
static lv_obj_t *s_cross_dot = nullptr;

// Node markers projected onto the map. Contacts + discovered nodes that carry
// a valid lat/lon, fetched via the weak ui_get_map_markers() bridge. Each
// carries its name so the map can label the nearby nodes.
struct MapMarker {
  double lat;
  double lon;
  uint8_t is_repeater;
  char name[24];
};
extern "C" int ui_get_map_markers(MapMarker *out, int max) __attribute__((weak));
static constexpr int MAX_MARKERS = 24;
static lv_obj_t *s_marker[MAX_MARKERS] = { nullptr };
static lv_obj_t *s_marker_label[MAX_MARKERS] = { nullptr };

// Centre fallback: Den Haag, NL. UITask overrides this with saved Home on
// open, then with live GPS a few seconds later (map_screen_set_center).
static constexpr double DEFAULT_LAT_DEG = 52.080;
static constexpr double DEFAULT_LON_DEG = 4.310;
// The SD holds carto z16+z17 (legacy `tiles/<z>/<x>/<y>.png` layout). z16 is
// a good neighbourhood-scale default; resolve_tile_path falls back to the
// legacy schema so the "carto" source label is cosmetic.
static constexpr int DEFAULT_ZOOM = 16;
static constexpr const char *DEFAULT_SOURCE = "carto";

// Live centre — mutated by map_screen_set_center(). Starts at the fallback.
static double s_center_lat = DEFAULT_LAT_DEG;
static double s_center_lon = DEFAULT_LON_DEG;
static int s_zoom = DEFAULT_ZOOM;

// Clamp panning-zoom to the band present on the SD card (z14..z17).
static constexpr int MAP_ZOOM_MIN = 14;
static constexpr int MAP_ZOOM_MAX = 17;

// GPS status for the strip — provided by the firmware (target.cpp).
extern "C" int ui_get_sat_count() __attribute__((weak));
extern "C" bool ui_get_gps_valid() __attribute__((weak));

// Try the MAPS.md primary path first, then fall back to the Ripple
// legacy schema; returns true if either exists on /sd and fills both
// the POSIX path (for fopen) and the LVGL drive-A path (for image src).
static bool resolve_tile_path(const char *source, int zoom, int tx, int ty, char *lvgl_out, int lvgl_cap) {
  char posix_path[map_tiles::PATH_BUF_MIN];

  map_tiles::format_tile_path(posix_path, sizeof(posix_path), "/sd", source, zoom, tx, ty);
  if (FILE *f = fopen(posix_path, "r")) {
    fclose(f);
    map_tiles::format_tile_path(lvgl_out, lvgl_cap, "A:", source, zoom, tx, ty);
    return true;
  }
  map_tiles::format_tile_path_legacy(posix_path, sizeof(posix_path), "/sd", zoom, tx, ty);
  if (FILE *f = fopen(posix_path, "r")) {
    fclose(f);
    map_tiles::format_tile_path_legacy(lvgl_out, lvgl_cap, "A:", zoom, tx, ty);
    return true;
  }
  return false;
}

lv_obj_t *map_screen_create(lv_obj_t *parent) {
  if (s_root) return s_root;

  s_root = lv_obj_create(parent);
  lv_obj_remove_style_all(s_root);
  lv_obj_set_size(s_root, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(s_root, lv_color_hex(tpager_pal_bg()), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);

  // 3x3 raster: nine 256-px tiles positioned by absolute pixel origin in
  // map_render() so the exact centre lat/lon sits at the viewport centre
  // regardless of where inside its tile it falls. Created first so the
  // crosshair, markers and status strip below draw on top (LVGL renders
  // siblings in tree order).
  for (int i = 0; i < 9; i++) {
    s_tiles[i] = lv_image_create(s_root);
    lv_obj_add_flag(s_tiles[i], LV_OBJ_FLAG_HIDDEN);
  }

  // Node markers (contacts = orange dot, repeaters = green square). Hidden
  // until map_render() projects them into view. Pre-allocated pool.
  for (int i = 0; i < MAX_MARKERS; i++) {
    s_marker[i] = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_marker[i]);
    lv_obj_set_size(s_marker[i], 8, 8);
    lv_obj_set_style_bg_opa(s_marker[i], LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_marker[i], 1, 0);
    lv_obj_set_style_border_color(s_marker[i], lv_color_hex(0x000000), 0);
    lv_obj_add_flag(s_marker[i], LV_OBJ_FLAG_HIDDEN);

    // Name label — small white text on a translucent black pill so it stays
    // readable over any tile colour. Placed next to its marker in
    // map_place_overlays().
    s_marker_label[i] = lv_label_create(s_root);
    lv_obj_set_style_text_color(s_marker_label[i], lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_marker_label[i], &lv_font_montserrat_12, 0);
    lv_obj_set_style_bg_color(s_marker_label[i], lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_marker_label[i], LV_OPA_60, 0);
    lv_obj_set_style_pad_hor(s_marker_label[i], 2, 0);
    lv_obj_set_style_pad_ver(s_marker_label[i], 1, 0);
    lv_obj_set_style_radius(s_marker_label[i], 2, 0);
    lv_obj_add_flag(s_marker_label[i], LV_OBJ_FLAG_HIDDEN);
  }

  // Fallback body — shown when SD is missing or the centre tile is not
  // on the card. Drawn under the status strip but above the tiles.
  s_body = lv_label_create(s_root);
  lv_label_set_text(s_body, "Map view\n\nMounting SD ...");
  lv_obj_set_style_text_color(s_body, lv_color_hex(tpager_pal_text()), 0);
  lv_obj_set_width(s_body, lv_pct(96));
  lv_label_set_long_mode(s_body, LV_LABEL_LONG_WRAP);
  lv_obj_align(s_body, LV_ALIGN_TOP_LEFT, 6, 48);

  // Position crosshair — marks the viewport centre (own GPS location once
  // the map follows the fix). White with a black outline via two crossing
  // bars + a centre dot so it stays visible over any tile colour. Created
  // after the tiles/markers so it draws on top.
  s_cross_h = lv_obj_create(s_root);
  lv_obj_remove_style_all(s_cross_h);
  lv_obj_set_size(s_cross_h, 22, 2);
  lv_obj_set_style_bg_color(s_cross_h, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_bg_opa(s_cross_h, LV_OPA_COVER, 0);
  lv_obj_set_style_outline_width(s_cross_h, 1, 0);
  lv_obj_set_style_outline_color(s_cross_h, lv_color_hex(0x000000), 0);
  lv_obj_add_flag(s_cross_h, LV_OBJ_FLAG_HIDDEN);

  s_cross_v = lv_obj_create(s_root);
  lv_obj_remove_style_all(s_cross_v);
  lv_obj_set_size(s_cross_v, 2, 22);
  lv_obj_set_style_bg_color(s_cross_v, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_bg_opa(s_cross_v, LV_OPA_COVER, 0);
  lv_obj_set_style_outline_width(s_cross_v, 1, 0);
  lv_obj_set_style_outline_color(s_cross_v, lv_color_hex(0x000000), 0);
  lv_obj_add_flag(s_cross_v, LV_OBJ_FLAG_HIDDEN);

  s_cross_dot = lv_obj_create(s_root);
  lv_obj_remove_style_all(s_cross_dot);
  lv_obj_set_size(s_cross_dot, 8, 8);
  lv_obj_set_style_radius(s_cross_dot, 4, 0);
  lv_obj_set_style_bg_color(s_cross_dot, lv_color_hex(0x2196F3), 0);
  lv_obj_set_style_bg_opa(s_cross_dot, LV_OPA_COVER, 0);
  lv_obj_set_style_outline_width(s_cross_dot, 1, 0);
  lv_obj_set_style_outline_color(s_cross_dot, lv_color_hex(0xFFFFFF), 0);
  lv_obj_add_flag(s_cross_dot, LV_OBJ_FLAG_HIDDEN);

  // Status strip — top bar, opaque black with large bold white text so the
  // zoom + satellite count stay readable in direct sunlight outdoors.
  s_status_strip = lv_label_create(s_root);
  lv_label_set_text(s_status_strip, "carto  z16");
  lv_obj_set_size(s_status_strip, lv_pct(100), 30);
  lv_obj_set_style_bg_color(s_status_strip, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(s_status_strip, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(s_status_strip, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(s_status_strip, &lv_font_montserrat_20, 0);
  lv_obj_set_style_pad_left(s_status_strip, 8, 0);
  lv_obj_set_style_pad_ver(s_status_strip, 5, 0);
  lv_obj_align(s_status_strip, LV_ALIGN_TOP_LEFT, 0, 0);

  // Controls hint — bottom strip, solid black + white so it stays readable
  // outdoors too.
  lv_obj_t *hint = lv_label_create(s_root);
  lv_label_set_text(hint, "WASD pan  Z/X zoom  long-press back");
  lv_obj_set_style_bg_color(hint, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(hint, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(hint, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_pad_hor(hint, 6, 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -2);
  return s_root;
}

// Project the node markers onto the current view and place the crosshair.
// Cheap (no PNG decode) so it can run on every auto-follow re-centre.
static void map_place_overlays(int cx, int cy, int W, int H, double c_world_x, double c_world_y) {
  // Crosshair at the exact viewport centre = own position once following GPS.
  if (s_cross_h) {
    lv_obj_set_pos(s_cross_h, cx - 11, cy - 1);
    lv_obj_remove_flag(s_cross_h, LV_OBJ_FLAG_HIDDEN);
  }
  if (s_cross_v) {
    lv_obj_set_pos(s_cross_v, cx - 1, cy - 11);
    lv_obj_remove_flag(s_cross_v, LV_OBJ_FLAG_HIDDEN);
  }
  if (s_cross_dot) {
    lv_obj_set_pos(s_cross_dot, cx - 4, cy - 4);
    lv_obj_remove_flag(s_cross_dot, LV_OBJ_FLAG_HIDDEN);
  }

  // Node markers, projected relative to the centre world pixel.
  int n = 0;
  MapMarker mk[MAX_MARKERS];
  if (ui_get_map_markers) {
    n = ui_get_map_markers(mk, MAX_MARKERS);
    if (n < 0) n = 0;
    if (n > MAX_MARKERS) n = MAX_MARKERS;
  }
  const double world_px = (double)(1 << s_zoom) * map_tiles::TILE_PX;
  for (int i = 0; i < MAX_MARKERS; i++) {
    if (i >= n) {
      if (s_marker[i]) lv_obj_add_flag(s_marker[i], LV_OBJ_FLAG_HIDDEN);
      if (s_marker_label[i]) lv_obj_add_flag(s_marker_label[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    const auto mc = map_tiles::latlon_to_tile(mk[i].lat, mk[i].lon, s_zoom);
    double mwx = (double)mc.tile_x * map_tiles::TILE_PX + mc.px_in_tile;
    double mwy = (double)mc.tile_y * map_tiles::TILE_PX + mc.py_in_tile;
    double dx = mwx - c_world_x;
    // Fold across the antimeridian (harmless for NL, correct globally).
    if (dx > world_px * 0.5) dx -= world_px;
    if (dx < -world_px * 0.5) dx += world_px;
    int sx = cx + (int)lround(dx);
    int sy = cy + (int)lround(mwy - c_world_y);
    // Keep markers off the top status bar / bottom hint and inside the view.
    if (sx < 4 || sx > W - 4 || sy < 34 || sy > H - 20) {
      lv_obj_add_flag(s_marker[i], LV_OBJ_FLAG_HIDDEN);
      if (s_marker_label[i]) lv_obj_add_flag(s_marker_label[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    if (mk[i].is_repeater) {
      lv_obj_set_style_radius(s_marker[i], 0, 0);                        // square
      lv_obj_set_style_bg_color(s_marker[i], lv_color_hex(0x36C24A), 0); // green
    } else {
      lv_obj_set_style_radius(s_marker[i], 4, 0);                        // dot
      lv_obj_set_style_bg_color(s_marker[i], lv_color_hex(0xFAA61A), 0); // orange
    }
    lv_obj_set_pos(s_marker[i], sx - 4, sy - 4);
    lv_obj_remove_flag(s_marker[i], LV_OBJ_FLAG_HIDDEN);

    // Name label next to the marker. Default to the right; flip to the left
    // when the marker sits near the right edge so the text stays on screen.
    if (s_marker_label[i]) {
      if (mk[i].name[0]) {
        lv_label_set_text(s_marker_label[i], mk[i].name);
        int lw = lv_obj_get_width(s_marker_label[i]);
        int lx = (sx + 8 + lw <= W - 2) ? sx + 8 : sx - 8 - lw;
        int ly = sy - 7;
        if (ly < 32) ly = 32;
        lv_obj_set_pos(s_marker_label[i], lx, ly);
        lv_obj_remove_flag(s_marker_label[i], LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(s_marker_label[i], LV_OBJ_FLAG_HIDDEN);
      }
    }
  }
}

// Render the 3x3 raster centred on (s_center_lat/lon, s_zoom) with sub-tile
// pixel offset so the exact centre lands at the viewport centre, then place
// the crosshair + node markers. Assumes the root is already visible.
static void map_render() {
  const bool sd_ok = sd_init();
  if (!sd_ok) {
    for (int i = 0; i < 9; i++)
      if (s_tiles[i]) lv_obj_add_flag(s_tiles[i], LV_OBJ_FLAG_HIDDEN);
    if (s_cross_h) lv_obj_add_flag(s_cross_h, LV_OBJ_FLAG_HIDDEN);
    if (s_cross_v) lv_obj_add_flag(s_cross_v, LV_OBJ_FLAG_HIDDEN);
    if (s_cross_dot) lv_obj_add_flag(s_cross_dot, LV_OBJ_FLAG_HIDDEN);
    if (s_body) {
      lv_obj_remove_flag(s_body, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(s_body, "Map view\n\n"
                                "SD: mount FAILED.\n"
                                "Insert card, then re-open the Map tile.");
    }
    return;
  }

  int W = lv_obj_get_width(s_root);
  int H = lv_obj_get_height(s_root);
  if (W <= 0) W = 480;
  if (H <= 0) H = 222;
  const int cx = W / 2;
  const int cy = H / 2;

  // Slippy-map the centre to (z, x, y) + sub-tile pixel offset.
  const auto tc = map_tiles::latlon_to_tile(s_center_lat, s_center_lon, s_zoom);
  const int TX = tc.tile_x;
  const int TY = tc.tile_y;
  const int origin_x = cx - tc.px_in_tile; // screen pos of centre-tile TL
  const int origin_y = cy - tc.py_in_tile;
  const double c_world_x = (double)TX * map_tiles::TILE_PX + tc.px_in_tile;
  const double c_world_y = (double)TY * map_tiles::TILE_PX + tc.py_in_tile;
  const int maxt = (1 << s_zoom) - 1;

  int shown = 0;
  for (int r = -1; r <= 1; r++) {
    for (int c = -1; c <= 1; c++) {
      const int idx = (r + 1) * 3 + (c + 1);
      lv_obj_t *img = s_tiles[idx];
      if (!img) continue;
      const int tx = map_tiles::wrap_tile_x(TX + c, s_zoom);
      const int ty = TY + r;
      if (ty < 0 || ty > maxt) {
        lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
        continue;
      }
      // Reload the PNG only when this slot now shows a different tile;
      // otherwise just reposition (cheap — no decode) for smooth following.
      if (s_tile_z[idx] != s_zoom || s_tile_x[idx] != tx || s_tile_y[idx] != ty) {
        if (resolve_tile_path(DEFAULT_SOURCE, s_zoom, tx, ty, s_tile_path[idx], sizeof(s_tile_path[idx]))) {
          lv_image_set_src(img, s_tile_path[idx]);
          s_tile_z[idx] = s_zoom;
          s_tile_x[idx] = tx;
          s_tile_y[idx] = ty;
        } else {
          s_tile_z[idx] = 0;
          s_tile_x[idx] = -1;
          s_tile_y[idx] = -1;
          lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
          continue;
        }
      }
      lv_obj_set_pos(img, origin_x + c * map_tiles::TILE_PX, origin_y + r * map_tiles::TILE_PX);
      lv_obj_remove_flag(img, LV_OBJ_FLAG_HIDDEN);
      shown++;
    }
  }
  Serial.printf("MAP: centre (%.4f, %.4f) z=%d tile(%d,%d) shown=%d\n", s_center_lat, s_center_lon, s_zoom,
                TX, TY, shown);

  if (shown == 0) {
    if (s_cross_h) lv_obj_add_flag(s_cross_h, LV_OBJ_FLAG_HIDDEN);
    if (s_cross_v) lv_obj_add_flag(s_cross_v, LV_OBJ_FLAG_HIDDEN);
    if (s_cross_dot) lv_obj_add_flag(s_cross_dot, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < MAX_MARKERS; i++) {
      if (s_marker[i]) lv_obj_add_flag(s_marker[i], LV_OBJ_FLAG_HIDDEN);
      if (s_marker_label[i]) lv_obj_add_flag(s_marker_label[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (s_body) {
      lv_obj_remove_flag(s_body, LV_OBJ_FLAG_HIDDEN);
      char msg[160];
      snprintf(msg, sizeof(msg),
               "Map view\n\nNo tiles for z=%d near tile x=%d y=%d.\n"
               "Card holds carto z14-z17.",
               s_zoom, TX, TY);
      lv_label_set_text(s_body, msg);
    }
    return;
  }

  if (s_body) lv_obj_add_flag(s_body, LV_OBJ_FLAG_HIDDEN);
  map_place_overlays(cx, cy, W, H, c_world_x, c_world_y);
  map_screen_refresh_status();
}

// Update only the status strip (zoom, satellite count + fix, centre
// coords) — no tile reload. Safe to call on a timer. "MAP" prefix dropped
// to make room for the sat counter.
void map_screen_refresh_status() {
  if (!s_status_strip) return;
  char buf[64];
  int sats = ui_get_sat_count ? ui_get_sat_count() : -1;
  bool fix = ui_get_gps_valid ? ui_get_gps_valid() : false;
  char sbuf[16];
  if (sats >= 0)
    snprintf(sbuf, sizeof(sbuf), "%s%d sat", fix ? "" : "~", sats);
  else
    snprintf(sbuf, sizeof(sbuf), "-- sat");
  snprintf(buf, sizeof(buf), "z%d  %s  %.4f,%.4f", s_zoom, sbuf, s_center_lat, s_center_lon);
  lv_label_set_text(s_status_strip, buf);
}

// Pan by whole tiles in tile space (dx/dy in tile units, +x = east,
// +y = south). Recomputes the centre lat/lon so the next render shifts.
void map_screen_pan(int dx, int dy) {
  const auto tc = map_tiles::latlon_to_tile(s_center_lat, s_center_lon, s_zoom);
  int ntx = map_tiles::wrap_tile_x(tc.tile_x + dx, s_zoom);
  int nty = tc.tile_y + dy;
  int maxt = (1 << s_zoom) - 1;
  if (nty < 0) nty = 0;
  if (nty > maxt) nty = maxt;
  // Aim at the centre of the new tile (+0.5,+0.5) so repeated pans don't
  // drift onto a tile boundary.
  double nw_lat, nw_lon, se_lat, se_lon;
  map_tiles::tile_to_latlon(ntx, nty, s_zoom, nw_lat, nw_lon);
  map_tiles::tile_to_latlon(ntx + 1, nty + 1, s_zoom, se_lat, se_lon);
  s_center_lat = (nw_lat + se_lat) * 0.5;
  s_center_lon = (nw_lon + se_lon) * 0.5;
  if (s_root && !lv_obj_has_flag(s_root, LV_OBJ_FLAG_HIDDEN)) map_render();
}

// Zoom by dz, clamped to the on-card band [16,17]. Centre is preserved.
void map_screen_zoom(int dz) {
  int nz = s_zoom + dz;
  if (nz < MAP_ZOOM_MIN) nz = MAP_ZOOM_MIN;
  if (nz > MAP_ZOOM_MAX) nz = MAP_ZOOM_MAX;
  if (nz == s_zoom) return;
  s_zoom = nz;
  if (s_root && !lv_obj_has_flag(s_root, LV_OBJ_FLAG_HIDDEN)) map_render();
}

void map_screen_show() {
  if (!s_root) return;
  lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
  Serial.println("MAP: open");
  map_render();
}

void map_screen_apply_theme() {
  if (s_root) lv_obj_set_style_bg_color(s_root, lv_color_hex(tpager_pal_bg()), LV_PART_MAIN);
  if (s_body) lv_obj_set_style_text_color(s_body, lv_color_hex(tpager_pal_text()), 0);
}

void map_screen_set_center(double lat_deg, double lon_deg) {
  if (lat_deg == 0.0 && lon_deg == 0.0) return; // no fix / unset — ignore
  s_center_lat = lat_deg;
  s_center_lon = lon_deg;
  // Re-render only while the map is on screen.
  if (s_root && !lv_obj_has_flag(s_root, LV_OBJ_FLAG_HIDDEN)) map_render();
}

void map_screen_hide() {
  if (s_root) lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
}
