#pragma once
#include <lvgl.h>

class LGFX_TPager;

bool tpager_lvgl_begin(LGFX_TPager *panel);
void tpager_lvgl_encoder_delta(int delta);
void tpager_lvgl_encoder_pressed(bool pressed);
lv_indev_t *tpager_lvgl_get_encoder();

// Raw encoder mode for the Set-time field editor (see TPagerLVGL.cpp).
void tpager_lvgl_encoder_set_raw(bool on);
int tpager_lvgl_encoder_take_delta();
bool tpager_lvgl_encoder_take_click();
