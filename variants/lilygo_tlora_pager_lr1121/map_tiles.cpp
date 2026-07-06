#include "map_tiles.h"

#include <math.h>
#include <stdio.h>

namespace map_tiles {

TileCoord latlon_to_tile(double lat_deg, double lon_deg, int zoom) {
  if (lat_deg > LAT_LIMIT_DEG) lat_deg = LAT_LIMIT_DEG;
  if (lat_deg < -LAT_LIMIT_DEG) lat_deg = -LAT_LIMIT_DEG;

  const double n = (double)(1 << zoom);
  const double lat_rad = lat_deg * M_PI / 180.0;
  const double xf = (lon_deg + 180.0) / 360.0 * n;
  const double yf = (1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / M_PI) / 2.0 * n;

  const int xi = (int)floor(xf);
  const int yi = (int)floor(yf);
  return TileCoord{
    xi,
    yi,
    (int)((xf - xi) * TILE_PX),
    (int)((yf - yi) * TILE_PX),
  };
}

void tile_to_latlon(int tile_x, int tile_y, int zoom, double &lat_deg_out, double &lon_deg_out) {
  const double n = (double)(1 << zoom);
  lon_deg_out = (double)tile_x / n * 360.0 - 180.0;
  const double yn = M_PI * (1.0 - 2.0 * (double)tile_y / n);
  lat_deg_out = atan(sinh(yn)) * 180.0 / M_PI;
}

int wrap_tile_x(int tile_x, int zoom) {
  const int span = 1 << zoom;
  int r = tile_x % span;
  if (r < 0) r += span;
  return r;
}

int format_tile_path(char *out, int cap, const char *prefix, const char *source, int zoom, int tile_x,
                     int tile_y) {
  const int n = snprintf(out, cap, "%s/tiles/%s/%d/%d/%d.png", prefix, source, zoom, tile_x, tile_y);
  return (n < 0 || n >= cap) ? -1 : n;
}

int format_tile_path_legacy(char *out, int cap, const char *prefix, int zoom, int tile_x, int tile_y) {
  const int n = snprintf(out, cap, "%s/tiles/%d/%d/%d.png", prefix, zoom, tile_x, tile_y);
  return (n < 0 || n >= cap) ? -1 : n;
}

} // namespace map_tiles
