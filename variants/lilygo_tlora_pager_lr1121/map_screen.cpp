#include <lvgl.h>
#include <Arduino.h>
#include "map_screen.h"
#include "target.h"

static lv_obj_t* s_root = nullptr;
static lv_obj_t* s_body = nullptr;

lv_obj_t* map_screen_create(lv_obj_t* parent) {
  if (s_root) return s_root;

  s_root = lv_obj_create(parent);
  lv_obj_remove_style_all(s_root);
  lv_obj_set_size(s_root, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(s_root, lv_color_hex(0x0e141b), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);

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

  // Lazy SD bringup: first open mounts /sd, subsequent opens hit the
  // s_sd_mounted short-circuit inside sd_init() and just refresh the
  // status label.
  Serial.println("MAP: open — attempting SD mount");
  const bool sd_ok = sd_init();
  if (s_body) {
    if (sd_ok) {
      lv_label_set_text(s_body,
        "Map view\n\n"
        "SD: mounted at /sd\n"
        "Phase 2 tile raster lands next.\n"
        "Source: OSM (default)\n"
        "Zoom: 10  Centre: 52.080, 4.310");
    } else {
      lv_label_set_text(s_body,
        "Map view\n\n"
        "SD: mount FAILED.\n"
        "Insert card, then re-open the Map tile.");
    }
  }
}

void map_screen_hide() {
  if (s_root) lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
}
