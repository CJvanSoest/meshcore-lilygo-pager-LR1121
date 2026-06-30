#include <lvgl.h>
#include <Arduino.h>
#include <stdio.h>
#include "map_screen.h"
#include "map_tiles.h"
#include "target.h"

static lv_obj_t* s_root          = nullptr;
static lv_obj_t* s_body          = nullptr;   // text shown when no tile is up
static lv_obj_t* s_status_strip  = nullptr;  // top 24 px
static lv_obj_t* s_tile_img_l    = nullptr;   // left tile in the 2x1 raster
static lv_obj_t* s_tile_img_r    = nullptr;   // right tile (X+1)
static char      s_path_l[map_tiles::PATH_BUF_MIN];
static char      s_path_r[map_tiles::PATH_BUF_MIN];

// Centre fallback: Den Haag, NL. UITask overrides this with saved Home on
// open, then with live GPS a few seconds later (map_screen_set_center).
static constexpr double DEFAULT_LAT_DEG = 52.080;
static constexpr double DEFAULT_LON_DEG = 4.310;
// The SD holds carto z16+z17 (legacy `tiles/<z>/<x>/<y>.png` layout). z16 is
// a good neighbourhood-scale default; resolve_tile_path falls back to the
// legacy schema so the "carto" source label is cosmetic.
static constexpr int    DEFAULT_ZOOM    = 16;
static constexpr const char* DEFAULT_SOURCE = "carto";

// Live centre — mutated by map_screen_set_center(). Starts at the fallback.
static double s_center_lat = DEFAULT_LAT_DEG;
static double s_center_lon = DEFAULT_LON_DEG;
static int    s_zoom       = DEFAULT_ZOOM;

// The SD card holds carto z16 + z17 only; clamp panning-zoom to that band.
static constexpr int MAP_ZOOM_MIN = 16;
static constexpr int MAP_ZOOM_MAX = 17;

// GPS status for the strip — provided by the firmware (target.cpp).
extern "C" int  ui_get_sat_count() __attribute__((weak));
extern "C" bool ui_get_gps_valid() __attribute__((weak));

// Try the MAPS.md primary path first, then fall back to the Ripple
// legacy schema; returns true if either exists on /sd and fills both
// the POSIX path (for fopen) and the LVGL drive-A path (for image src).
static bool resolve_tile_path(const char* source, int zoom, int tx, int ty,
                              char* lvgl_out, int lvgl_cap) {
  char posix_path[map_tiles::PATH_BUF_MIN];

  map_tiles::format_tile_path(posix_path, sizeof(posix_path),
                              "/sd", source, zoom, tx, ty);
  if (FILE* f = fopen(posix_path, "r")) {
    fclose(f);
    map_tiles::format_tile_path(lvgl_out, lvgl_cap,
                                "A:", source, zoom, tx, ty);
    return true;
  }
  map_tiles::format_tile_path_legacy(posix_path, sizeof(posix_path),
                                     "/sd", zoom, tx, ty);
  if (FILE* f = fopen(posix_path, "r")) {
    fclose(f);
    map_tiles::format_tile_path_legacy(lvgl_out, lvgl_cap,
                                       "A:", zoom, tx, ty);
    return true;
  }
  return false;
}

lv_obj_t* map_screen_create(lv_obj_t* parent) {
  if (s_root) return s_root;

  s_root = lv_obj_create(parent);
  lv_obj_remove_style_all(s_root);
  lv_obj_set_size(s_root, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(s_root, lv_color_hex(0x0e141b), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);

  // 2x1 raster (plan §2.1 Option A): two 256-px tiles placed so their
  // seam falls at the viewport's centre. Left tile right-edge = centre,
  // right tile left-edge = centre. Vertical sits a few px below the
  // status strip; overflow at top/bottom clips naturally (256 px image
  // on a 222 px viewport). Created first so the status strip below
  // draws on top of the tiles (LVGL renders siblings in tree order).
  s_tile_img_l = lv_image_create(s_root);
  lv_obj_align(s_tile_img_l, LV_ALIGN_CENTER, -128, 12);
  lv_obj_add_flag(s_tile_img_l, LV_OBJ_FLAG_HIDDEN);

  s_tile_img_r = lv_image_create(s_root);
  lv_obj_align(s_tile_img_r, LV_ALIGN_CENTER,  128, 12);
  lv_obj_add_flag(s_tile_img_r, LV_OBJ_FLAG_HIDDEN);

  // Fallback body — shown when SD is missing or the centre tile is not
  // on the card. Drawn under the status strip but above the tiles.
  s_body = lv_label_create(s_root);
  lv_label_set_text(s_body, "Map view\n\nMounting SD ...");
  lv_obj_set_style_text_color(s_body, lv_color_hex(0xc0c8d0), 0);
  lv_obj_set_width(s_body, lv_pct(96));
  lv_label_set_long_mode(s_body, LV_LABEL_LONG_WRAP);
  lv_obj_align(s_body, LV_ALIGN_TOP_LEFT, 6, 48);

  // Status strip — top 22 px, opaque dark bg so the orange text stays
  // legible against the tile raster. Created last so it draws on top.
  // Plan §5 will promote this to a 3-column layout (source / sat / bat).
  s_status_strip = lv_label_create(s_root);
  lv_label_set_text(s_status_strip, "MAP - carto  z=16");
  lv_obj_set_size(s_status_strip, lv_pct(100), 22);
  lv_obj_set_style_bg_color(s_status_strip, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(s_status_strip, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(s_status_strip, lv_color_hex(0xFAA61A), 0);
  lv_obj_set_style_pad_left(s_status_strip, 8, 0);
  lv_obj_set_style_pad_ver(s_status_strip, 4, 0);
  lv_obj_align(s_status_strip, LV_ALIGN_TOP_LEFT, 0, 0);

  // Controls hint — bottom strip, semi-transparent so the map stays visible.
  lv_obj_t* hint = lv_label_create(s_root);
  lv_label_set_text(hint, "WASD pan  Z/X zoom  long-press back");
  lv_obj_set_style_bg_color(hint, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(hint, LV_OPA_50, 0);
  lv_obj_set_style_text_color(hint, lv_color_hex(0xc0c8d0), 0);
  lv_obj_set_style_pad_hor(hint, 6, 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -2);
  return s_root;
}

// Render the 2x1 raster for the current centre (s_center_lat/lon, s_zoom).
// Assumes the root is already visible.
static void map_render() {
  const bool sd_ok = sd_init();
  if (!sd_ok) {
    if (s_tile_img_l) lv_obj_add_flag(s_tile_img_l, LV_OBJ_FLAG_HIDDEN);
    if (s_tile_img_r) lv_obj_add_flag(s_tile_img_r, LV_OBJ_FLAG_HIDDEN);
    if (s_body) {
      lv_obj_remove_flag(s_body, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(s_body,
        "Map view\n\n"
        "SD: mount FAILED.\n"
        "Insert card, then re-open the Map tile.");
    }
    return;
  }

  // Slippy-map the centre to (z, x, y).
  const auto tc = map_tiles::latlon_to_tile(s_center_lat, s_center_lon, s_zoom);
  const int tx_l = tc.tile_x;
  const int tx_r = map_tiles::wrap_tile_x(tc.tile_x + 1, s_zoom);
  const int ty   = tc.tile_y;
  Serial.printf("MAP: centre (%.4f, %.4f) z=%d -> tiles (%d,%d)+(%d,%d)\n",
                s_center_lat, s_center_lon, s_zoom, tx_l, ty, tx_r, ty);

  const bool ok_l = resolve_tile_path(DEFAULT_SOURCE, s_zoom, tx_l, ty,
                                      s_path_l, sizeof(s_path_l));
  const bool ok_r = resolve_tile_path(DEFAULT_SOURCE, s_zoom, tx_r, ty,
                                      s_path_r, sizeof(s_path_r));
  Serial.printf("MAP: left=%d right=%d\n", (int)ok_l, (int)ok_r);

  if (!ok_l && !ok_r) {
    if (s_tile_img_l) lv_obj_add_flag(s_tile_img_l, LV_OBJ_FLAG_HIDDEN);
    if (s_tile_img_r) lv_obj_add_flag(s_tile_img_r, LV_OBJ_FLAG_HIDDEN);
    if (s_body) {
      lv_obj_remove_flag(s_body, LV_OBJ_FLAG_HIDDEN);
      char msg[160];
      snprintf(msg, sizeof(msg),
               "Map view\n\nNo tiles for z=%d x=%d/%d y=%d.\n"
               "Card holds carto z16+z17.",
               s_zoom, tx_l, tx_r, ty);
      lv_label_set_text(s_body, msg);
    }
    return;
  }

  if (s_body) lv_obj_add_flag(s_body, LV_OBJ_FLAG_HIDDEN);

  if (s_tile_img_l) {
    if (ok_l) {
      lv_image_set_src(s_tile_img_l, s_path_l);
      lv_obj_remove_flag(s_tile_img_l, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(s_tile_img_l, LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (s_tile_img_r) {
    if (ok_r) {
      lv_image_set_src(s_tile_img_r, s_path_r);
      lv_obj_remove_flag(s_tile_img_r, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(s_tile_img_r, LV_OBJ_FLAG_HIDDEN);
    }
  }

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
  if (sats >= 0) snprintf(sbuf, sizeof(sbuf), "%s%d sat", fix ? "" : "~", sats);
  else           snprintf(sbuf, sizeof(sbuf), "-- sat");
  snprintf(buf, sizeof(buf), "z%d  %s  %.4f,%.4f",
           s_zoom, sbuf, s_center_lat, s_center_lon);
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

void map_screen_set_center(double lat_deg, double lon_deg) {
  if (lat_deg == 0.0 && lon_deg == 0.0) return;   // no fix / unset — ignore
  s_center_lat = lat_deg;
  s_center_lon = lon_deg;
  // Re-render only while the map is on screen.
  if (s_root && !lv_obj_has_flag(s_root, LV_OBJ_FLAG_HIDDEN)) map_render();
}

void map_screen_hide() {
  if (s_root) lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
}
