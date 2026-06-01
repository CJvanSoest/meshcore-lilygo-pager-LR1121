#pragma once
#include <lvgl.h>

class LGFX_TPager;

bool         tpager_lvgl_begin(LGFX_TPager* panel);
void         tpager_lvgl_encoder_delta(int delta);
void         tpager_lvgl_encoder_pressed(bool pressed);
lv_indev_t*  tpager_lvgl_get_encoder();
