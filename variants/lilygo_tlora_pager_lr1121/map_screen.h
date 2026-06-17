// Map screen for the LilyGo T-Pager (S3.6 VIEW_MAP).
//
// Phase 1: placeholder body inside the subscreen container — proves the
// carousel can open/close a Map-owned LVGL container.
// Phase 2+: tile raster, status strip, pan/zoom, GPS lock. See
// PLAN_MAP_VIEW.md for the staged plan.
#pragma once

#include <lvgl.h>

// Create the Map screen as a child of `parent` (UITask.cpp's
// s_subscreen_root). Hidden by default. Idempotent: a second call returns
// the existing root without rebuilding.
lv_obj_t* map_screen_create(lv_obj_t* parent);

// Visibility — called by UITask.cpp on tile enter/leave.
void map_screen_show();
void map_screen_hide();
