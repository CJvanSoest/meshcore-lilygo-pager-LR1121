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
lv_obj_t *map_screen_create(lv_obj_t *parent);

// Visibility — called by UITask.cpp on tile enter/leave.
void map_screen_show();
void map_screen_hide();

// Set the map centre (decimal degrees). If the map is currently visible it
// re-renders immediately. UITask centres on saved Home on open, then hands
// off to live GPS after a few seconds. A (0,0) centre is ignored.
void map_screen_set_center(double lat_deg, double lon_deg);

// Manual navigation (keyboard-driven from UITask). Pan by whole tiles
// (dx east, dy south); zoom by dz (clamped to the on-card z16..z17 band).
void map_screen_pan(int dx, int dy);
void map_screen_zoom(int dz);

// Refresh only the status strip (sat count / coords) without reloading
// tiles — call on a timer while the Map view is open.
void map_screen_refresh_status();
