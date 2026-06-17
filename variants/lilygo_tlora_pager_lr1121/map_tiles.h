// Slippy-map tile math for the T-Pager VIEW_MAP. Pure code — no LVGL,
// no SD, no FreeRTOS. Ported from the Tanmatsu meshcore-settings
// reference (`main/map.c`); the math is unchanged so a tile fetched by
// either device for the same (z, x, y) resolves to the same lat/lon
// patch.
//
// Phase 2 wires this into map_screen.cpp's tile-cache + raster path.
#pragma once

#include <stdint.h>

namespace map_tiles {

// One slippy-map tile is 256 px square. Option A (PLAN_MAP_VIEW §2.1).
inline constexpr int TILE_PX = 256;

// Zoom bounds. MIN matches the Ripple Europe tileset's lower edge that
// the Tanmatsu side already used; MAX is street-name scale.
inline constexpr int ZOOM_MIN = 6;
inline constexpr int ZOOM_MAX = 14;

// Web-Mercator pole clamp. Lat outside this range is mathematically
// outside the projection — the formula diverges at ±90°.
inline constexpr double LAT_LIMIT_DEG = 85.05112877980659;

// (tile_x, tile_y) plus the sub-tile pixel where the requested lat/lon
// lands. px/py are in [0, TILE_PX).
struct TileCoord {
  int tile_x;
  int tile_y;
  int px_in_tile;
  int py_in_tile;
};

// (lat°, lon°, zoom) → TileCoord. Latitude is clamped to ±LAT_LIMIT_DEG
// before mapping. Tile (x, y) may be negative or past 2^zoom-1 if the
// caller passes a lon outside [-180, 180]; use wrap_tile_x() to fold.
TileCoord latlon_to_tile(double lat_deg, double lon_deg, int zoom);

// Inverse: north-west corner lat/lon of tile (x, y) at the given zoom.
void tile_to_latlon(int tile_x, int tile_y, int zoom,
                    double& lat_deg_out, double& lon_deg_out);

// Wrap tile_x modulo 2^zoom so panning across the antimeridian works.
// Always returns a value in [0, 2^zoom).
int wrap_tile_x(int tile_x, int zoom);

// Path formatters. `out` must hold at least PATH_BUF_MIN chars.
// Returns the number of chars written excluding the terminator, or -1
// on overflow. `prefix` is typically "/sd" or LVGL's "S:".
inline constexpr int PATH_BUF_MIN = 64;

// Primary path per MAPS.md: <prefix>/tiles/<source>/<z>/<x>/<y>.png
int format_tile_path(char* out, int cap, const char* prefix,
                     const char* source, int zoom, int tile_x, int tile_y);

// Legacy fallback: <prefix>/tiles/<z>/<x>/<y>.png (Ripple Radio Europe
// tileset has this layout — no <source> directory).
int format_tile_path_legacy(char* out, int cap, const char* prefix,
                            int zoom, int tile_x, int tile_y);

}  // namespace map_tiles
