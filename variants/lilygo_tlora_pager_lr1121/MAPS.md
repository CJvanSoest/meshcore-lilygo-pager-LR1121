# Map tile sources for the T-Pager

The Map tile reader (planned S3.6) reads standard XYZ raster tiles from
the microSD card. Tiles are stored under `/tiles/<source>/<z>/<x>/<y>.png`
and the active source can be switched from the map view itself.

## Base set shipped with the firmware

These five sources are configured by default. Each gets its own folder
under `/tiles/` so multiple sources can coexist on the same SD card.

| Source folder | Provider | Style | License / etiquette |
|---|---|---|---|
| `osm` | OpenStreetMap | Standard street map | ODbL 1.0 — attribution required, ≤ 2 req/s when scraping |
| `pdok` | Dutch government (Kadaster) | Official BRT-Achtergrondkaart | Open data, attribution to "Kadaster" — best detail for NL |
| `opentopo` | OpenTopoMap.org | Topographic with contours | CC-BY-SA — good for outdoor / hiking |
| `cyclosm` | CyclOSM (osm.fr) | Cycling-focused | ODbL — many NL bike paths |
| `stamen` | Stadia Maps (Stamen Toner) | Black-and-white minimalist | CC-BY — clean look, low contrast on small displays |

### Tile-URL templates (for the download helper)

```
osm       https://tile.openstreetmap.org/{z}/{x}/{y}.png
pdok      https://service.pdok.nl/brt/achtergrondkaart/wmts/v2_0/standaard/EPSG:3857/{z}/{x}/{y}.png
opentopo  https://tile.opentopomap.org/{z}/{x}/{y}.png
cyclosm   https://a.tile-cyclosm.openstreetmap.fr/cyclosm/{z}/{x}/{y}.png
stamen    https://tiles.stadiamaps.com/tiles/stamen_toner/{z}/{x}/{y}.png
```

A Python helper (`tools/download_tiles.py`, planned with S3.6) takes a
bbox and a zoom range and saves to the right folders. It honours each
provider's rate-limits and skips already-downloaded tiles.

For NL with zoom 6–14 plan on roughly:

| Source | Approx size for full NL z6–14 |
|---|---|
| OSM      | ~80–100 MB |
| PDOK     | ~120 MB |
| OpenTopo | ~80 MB |
| CyclOSM  | ~70 MB |
| Stamen   | ~50 MB |
| **Total** | **~400 MB** (well under the 32 GB SD limit) |

---

## Alternatives for other countries

The base set covers NL well. For other regions or worldwide, here are
free providers in the same XYZ raster format, ready to drop into a new
folder under `/tiles/` and add to the source config-array.

### Country-specific national maps (free, government-issued)

| Country | Provider | Tile URL template |
|---|---|---|
| Germany | BKG TopPlusOpen | `https://sgx.geodatenzentrum.de/wmts_topplus_open/tile/1.0.0/web/default/WEBMERCATOR/{z}/{y}/{x}.png` |
| United Kingdom | Ordnance Survey OS Open Zoomstack | requires free OS Maps API key; tiles at `https://api.os.uk/maps/raster/v1/zxy/Outdoor_3857/{z}/{x}/{y}.png` |
| France | IGN Géoplateforme | `https://data.geopf.fr/wmts?...&LAYER=GEOGRAPHICALGRIDSYSTEMS.PLANIGNV2&TILEMATRIX={z}&TILEROW={y}&TILECOL={x}` |
| Switzerland | swisstopo | `https://wmts.geo.admin.ch/1.0.0/ch.swisstopo.pixelkarte-farbe/default/current/3857/{z}/{x}/{y}.jpeg` |
| Norway | Kartverket | `https://opencache.statkart.no/gatekeeper/gk/gk.open_gmaps?layers=topo4&zoom={z}&x={x}&y={y}` |
| Sweden | Lantmäteriet (Min Karta) | requires free Lantmäteriet token |
| Denmark | Datafordeleren / Dataforsyningen | requires free token |
| Belgium | NGI Cartoweb / GeoPunt | `https://geo.api.vlaanderen.be/GRB-basiskaart/wmts?...` |
| Austria | basemap.at | `https://maps.wien.gv.at/basemap/geolandbasemap/normal/google3857/{z}/{y}/{x}.png` |
| USA | USGS National Map | `https://basemap.nationalmap.gov/arcgis/rest/services/USGSTopo/MapServer/tile/{z}/{y}/{x}` |
| Canada | Government of Canada Open Maps | tiles via NRCan WMS endpoints |
| Italy | IGM portal | various endpoints, registration usually required |
| Japan | GSI (Kokudo Chiriin) | `https://cyberjapandata.gsi.go.jp/xyz/std/{z}/{x}/{y}.png` |

Check each provider's terms — most allow free non-commercial use with
attribution, some require a free API key, a few rate-limit heavily.

### Worldwide alternatives (no country focus)

| Provider | Style | Notes |
|---|---|---|
| OpenStreetMap.de | OSM rendered with German style | `https://tile.openstreetmap.de/{z}/{x}/{y}.png` |
| OSM Humanitarian | HOT-style, emphasises infrastructure | `https://tile-{a,b,c}.openstreetmap.fr/hot/{z}/{x}/{y}.png` |
| Esri World Imagery | Satellite | `https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}` — attribution required |
| Esri World Topo | Vector-style topo | `https://server.arcgisonline.com/ArcGIS/rest/services/World_Topo_Map/MapServer/tile/{z}/{y}/{x}` |
| CartoDB Positron / Dark Matter | Minimalist | free with attribution via Carto / Stadia |
| Thunderforest Outdoors / Transport | Cycling / transit | free tier with API key; paid past 150k tiles/month |
| Mapbox | Many styles | free tier ≤ 50k tile loads/month, requires API key |

### Notes & etiquette

* Most providers ask you not to use their public tile endpoints for
  bulk downloads. The helper script throttles to a few requests per
  second per source.
* PDOK explicitly allows scraping for offline use, no API key.
* If you intend to redistribute the tiles (e.g. share an SD image with
  somebody), check each provider's terms. ODbL tiles like OSM/PDOK
  generally permit redistribution with attribution.

---

## Switching sources in the map view (planned S3.6)

A header badge in the map view shows the current source ("OSM",
"PDOK", …). A single click on that badge cycles to the next source in
the config-array. The chosen source persists across reboots via
SPIFFS. Adding a new source = add an entry to `MAP_SOURCES[]` in the
variant code + a folder under `/tiles/`. No SD-side metadata required.

## Legacy fallback (Ripple Radio Europe tileset)

For backwards compatibility with the Ripple Radio Europe SD image, the
tile loader falls back to a source-less path when the primary path is
missing:

```
primary : /tiles/<source>/<z>/<x>/<y>.png
fallback: /tiles/<z>/<x>/<y>.png
```

Behaviour:
1. Tile loader tries `<primary>` first using the active source.
2. If that file is missing, it tries `<fallback>`.
3. Only if both are missing does the tile render as a grey rect.

This lets a SD card prepared for the Ripple firmware work on the Pager
without reorganising the folder tree, while new downloads via
`tools/download_tiles.py` go to the `<source>/` layout. When both
exist for the same `(z, x, y)`, the source-specific tile wins.

The header badge still shows the active source name (e.g. "OSM") even
when the rendered tile came from the fallback path — the badge
reflects the user-chosen source, not the actual file origin.
