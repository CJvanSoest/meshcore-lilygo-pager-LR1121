#include <lvgl.h>
#include "map_screen.h"

static lv_obj_t* s_root = nullptr;

lv_obj_t* map_screen_create(lv_obj_t* parent) {
  if (s_root) return s_root;

  s_root = lv_obj_create(parent);
  lv_obj_remove_style_all(s_root);
  lv_obj_set_size(s_root, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(s_root, lv_color_hex(0x0e141b), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t* body = lv_label_create(s_root);
  lv_label_set_text(body,
    "Map view\n\n"
    "Tile raster comes in Phase 2.\n"
    "Source: OSM (default)\n"
    "Zoom: 10  Centre: 52.080, 4.310");
  lv_obj_set_style_text_color(body, lv_color_hex(0xc0c8d0), 0);
  lv_obj_set_width(body, lv_pct(96));
  lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
  lv_obj_align(body, LV_ALIGN_TOP_LEFT, 6, 48);
  return s_root;
}

void map_screen_show() {
  if (s_root) lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
}

void map_screen_hide() {
  if (s_root) lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
}
