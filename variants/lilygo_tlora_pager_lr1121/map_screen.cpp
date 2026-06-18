#include <lvgl.h>
#include <Arduino.h>
#include <stdio.h>
#include "map_screen.h"
#include "map_tiles.h"
#include "target.h"

static lv_obj_t* s_root        = nullptr;
static lv_obj_t* s_body        = nullptr;   // text shown when no tile is up
static lv_obj_t* s_status_strip = nullptr;  // top 24 px
static lv_obj_t* s_tile_img    = nullptr;   // centre tile (lv_image)
static char      s_tile_lvgl_path[map_tiles::PATH_BUF_MIN];

// Centre fallback: Den Haag, NL. Phase 4 swaps this for the live GPS
// snapshot; Phase 6 swaps it for the NVS-persisted last centre.
static constexpr double DEFAULT_LAT_DEG = 52.080;
static constexpr double DEFAULT_LON_DEG = 4.310;
static constexpr int    DEFAULT_ZOOM    = 10;
static constexpr const char* DEFAULT_SOURCE = "osm";

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

  // Status strip — top 24 px, dark fill, light text. Plan §5 promotes
  // this to a 3-column layout (source/sat/battery); for Phase 2 we just
  // surface "MAP - <source>  z=<zoom>".
  s_status_strip = lv_label_create(s_root);
  lv_label_set_text(s_status_strip, "MAP - OSM  z=10");
  lv_obj_set_style_text_color(s_status_strip, lv_color_hex(0xFAA61A), 0);
  lv_obj_align(s_status_strip, LV_ALIGN_TOP_LEFT, 6, 4);

  // Centre tile — single 256x256 lv_image for now (plan §2.1 Option A =
  // 2x1 raster comes once 1-tile rendering is hardware-confirmed).
  s_tile_img = lv_image_create(s_root);
  lv_obj_align(s_tile_img, LV_ALIGN_CENTER, 0, 12);  // +12 px to clear the strip
  lv_obj_add_flag(s_tile_img, LV_OBJ_FLAG_HIDDEN);

  // Fallback body — shown when SD is missing or the centre tile is not
  // on the card.
  s_body = lv_label_create(s_root);
  lv_label_set_text(s_body, "Map view\n\nMounting SD ...");
  lv_obj_set_style_text_color(s_body, lv_color_hex(0xc0c8d0), 0);
  lv_obj_set_width(s_body, lv_pct(96));
  lv_label_set_long_mode(s_body, LV_LABEL_LONG_WRAP);
  lv_obj_align(s_body, LV_ALIGN_TOP_LEFT, 6, 48);
  return s_root;
}

void map_screen_show() {
  if (!s_root) return;
  lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);

  Serial.println("MAP: open");
  const bool sd_ok = sd_init();
  if (!sd_ok) {
    if (s_tile_img) lv_obj_add_flag(s_tile_img, LV_OBJ_FLAG_HIDDEN);
    if (s_body) {
      lv_obj_remove_flag(s_body, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(s_body,
        "Map view\n\n"
        "SD: mount FAILED.\n"
        "Insert card, then re-open the Map tile.");
    }
    return;
  }

  // Slippy-map the default centre to (z, x, y).
  const auto tc = map_tiles::latlon_to_tile(
      DEFAULT_LAT_DEG, DEFAULT_LON_DEG, DEFAULT_ZOOM);
  Serial.printf("MAP: centre (%.4f, %.4f) z=%d -> tile (%d,%d) px (%d,%d)\n",
                DEFAULT_LAT_DEG, DEFAULT_LON_DEG, DEFAULT_ZOOM,
                tc.tile_x, tc.tile_y, tc.px_in_tile, tc.py_in_tile);

  if (!resolve_tile_path(DEFAULT_SOURCE, DEFAULT_ZOOM,
                         tc.tile_x, tc.tile_y,
                         s_tile_lvgl_path, sizeof(s_tile_lvgl_path))) {
    Serial.printf("MAP: tile missing for z=%d x=%d y=%d\n",
                  DEFAULT_ZOOM, tc.tile_x, tc.tile_y);
    if (s_tile_img) lv_obj_add_flag(s_tile_img, LV_OBJ_FLAG_HIDDEN);
    if (s_body) {
      lv_obj_remove_flag(s_body, LV_OBJ_FLAG_HIDDEN);
      char msg[160];
      snprintf(msg, sizeof(msg),
               "Map view\n\nNo tile for z=%d x=%d y=%d.\n"
               "Run tools/download_tiles.py for %s.",
               DEFAULT_ZOOM, tc.tile_x, tc.tile_y, DEFAULT_SOURCE);
      lv_label_set_text(s_body, msg);
    }
    return;
  }
  Serial.printf("MAP: rendering %s\n", s_tile_lvgl_path);

  // Hide the placeholder body — the raster is what the user sees now.
  if (s_body) lv_obj_add_flag(s_body, LV_OBJ_FLAG_HIDDEN);

  if (s_tile_img) {
    lv_image_set_src(s_tile_img, s_tile_lvgl_path);
    lv_obj_remove_flag(s_tile_img, LV_OBJ_FLAG_HIDDEN);
  }
}

void map_screen_hide() {
  if (s_root) lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
}
