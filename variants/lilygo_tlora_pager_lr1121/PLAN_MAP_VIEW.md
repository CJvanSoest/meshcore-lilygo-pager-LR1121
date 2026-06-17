# Plan: LilyGo Pager — VIEW_MAP implementation

Status: planning · Target branch: `feat/pager-map-view` (off
`feature/lilygo-tlora-pager-lr1121` on Gitea `CJ/meshcore`) · Scope:
S3.6 from the variant TODO. Read [`MAPS.md`](MAPS.md) first — the
tile-source strategy (osm/pdok/opentopo/cyclosm/stamen) and SD layout
are settled there; this plan covers only the firmware implementation.

## 1. Context the goal-runner needs

- **Repo / cwd:** `~/stack/Projects/LilygoPager/MeshCore`
- **Build env:** `T_LoRa_Pager_LR1121_companion_radio_usb`
  (`platformio.ini` in this folder). Replaces the stock
  `examples/companion_radio/ui-new/UITask.cpp` with this folder's own
  `UITask.cpp`, so all map UI lives here.
- **Display:** ST7796, **480 × 222 landscape** (per
  `TPagerST7796Display.h:87`). Sprite dimensions are 480 × 222; a single
  256 × 256 OSM tile is *wider than half* the screen and *taller than
  the full screen* — see Phase 0 §2.1 for the layout decision.
- **PNG decoder:** LVGL **already has** `LV_USE_LODEPNG = 1` in
  `lv_conf.h:774`. No vendored decoder needed; we hand a `lv_image_dsc_t`
  pointing at the on-SD PNG (file: protocol) and LVGL streams + decodes.
- **PSRAM:** 8 MB confirmed via `[BOOT] psramFound=1 psram_size=8386295`.
  Enough for a 6-8 tile RGB565 cache (~1 MB) plus the LVGL display
  buffer.
- **SD layout** (per `MAPS.md`): `/tiles/<source>/<z>/<x>/<y>.png` where
  `<source>` is one of the five base providers. The Tanmatsu badge uses
  a different path (`/maps/<profile>/tiles/...`); see §5 for whether to
  share an SD between devices.
- **GPS:** MicroNMEA already feeds `SensorManager.node_lat / node_lon`
  per the variant `target.cpp` plumbing — reuse it, no new I2C driver.
- **Map tile already in the carousel** as a placeholder per the
  S3.6 layout (`Radio → Channels → DM → Contacts(★) → Discovered →
  Map → Settings → About`) — Phase 1 just wires its click handler.
- **Reference implementation:** the working Tanmatsu VIEW_MAP at
  `~/stack/Projects/Tanmatsu/meshcore-settings/main/{map.c,map.h,render_map.c,gps_task.c}`
  — the slippy-map math, LRU cache shape, profile-switcher and lock-to-
  position logic are 1-to-1 portable; only the rendering (PAX vs. LVGL)
  and the SD path scheme differ.

## 2. Pre-flight research (Phase 0)

Spikes — no commits land on the feature branch until they answer the
questions below.

### 2.1 Layout: which tile-px scheme on a 480 × 222 viewport?

Option A — **native 256 px tiles**, 2 × 1 horizontal raster with the
centre split between two adjacent X tiles. Status strip eats ~30 px →
192 px tall visible map area means a single tile fills the vertical
direction and ~20 px gets clipped top + bottom. Cleanest port from the
Tanmatsu code, no extra resampling.

Option B — **128 px down-scaled tiles**, 4 × 2 raster with 128 vertical
px for status. LVGL scales on the fly via `lv_image_set_scale_(x|y)`.
Halves the cache footprint per tile (~32 KB RGB565) but loses ~half the
detail. Acceptable if street labels are still legible.

**Decision pin:** start with Option A. Re-render at 128 only if A's
edge clipping looks bad in practice.

### 2.2 LVGL PNG decode latency on ESP32-PSRAM

Spike: open one 256 × 256 PNG from `/sd/tiles/osm/14/8388/5406.png` and
time `lv_image_decoder_open` + `lv_image_decoder_read_line` × 256 with
`micros()`. Target: **< 200 ms per tile** on the ESP32 @ 240 MHz so a
3-tile cache miss during a single pan-frame finishes within ~600 ms.
If it's slower than 500 ms, pre-convert tiles to LVGL's raw `lv_image_dsc_t`
binary at the NAS-render step (cuts decode to memcpy).

### 2.3 Encoder + click semantics inside the map view

Per the variant `README.md` navigation contract:

- Encoder rotation: scrolls/changes value
- Click: opens / applies / toggles
- Long-press (≥ 600 ms): goes back
- ≥ 3 s encoder hold: powers the device down (don't override)

Spike: confirm the encoder event API used by the existing tiles
(Discovered, Channels) and pick one of the two pan-and-zoom mappings:

| Mapping | Encoder | Click |
|---|---|---|
| **A** | rotate = zoom in/out, click = toggle source | TCA8418 arrow keys pan |
| **B** | rotate = pan along axis under the cursor, hold + rotate = zoom | click = source picker popup |

Default: **A** (matches existing tile-carousel mental model of "rotate
= magnitude, click = open"). Document the alternative for follow-up if
testing shows it's awkward.

### 2.4 SD read throughput from LVGL on this Pager

Spike: time `fread` of a 30 KB tile via the existing SD wrapper. ESP32
SD-SPI typically ~1-2 MB/s. We need ≥ 5 tiles/sec to keep panning fluid,
i.e. ≤ 200 ms per tile end-to-end (read + decode + blit). If the SD
path is the bottleneck, pre-convert to raw RGB565 binaries — same
mitigation as §2.2.

## 3. Architecture overview

```
                 ┌──────────────────────────────────────┐
                 │       Map screen (LVGL)              │
                 │  ┌─────────────────────────────────┐ │
                 │  │ map_screen.cpp                  │ │
                 │  │  - lv_obj canvas + raster       │ │
                 │  │  - encoder/click handlers       │ │
                 │  │  - status strip (top, ~24 px)   │ │
                 │  └────────┬────────────────────────┘ │
                 └───────────┼──────────────────────────┘
            ┌────────────────┴─────────────────┐
            ▼                                  ▼
   ┌──────────────────┐                ┌─────────────────────┐
   │ map_tiles.cpp    │                │ map_gps.cpp         │
   │ - slippy math    │                │ - reuse              │
   │ - LRU tile cache │                │   SensorManager      │
   │   (≤ 8 RGB565,   │                │   node_lat/lon       │
   │   PSRAM)         │                │ - profile presets    │
   │ - LVGL decoder   │                │   (carro/bike/foot)  │
   │   wrapping       │                │ - publish lat/lon    │
   │   /sd/tiles/...  │                │   under mutex        │
   └──────────────────┘                └─────────────────────┘
```

Persistent prefs (extend `NodePrefs.h`):

| Key (≤16 chars NVS) | Type | Purpose | Default |
|---|---|---|---|
| `map.lat_e6` | i32 | last viewed centre lat × 1e6 | 0 (use GPS) |
| `map.lon_e6` | i32 | last viewed centre lon × 1e6 | 0 (use GPS) |
| `map.zoom`   | u8  | last zoom level | 10 |
| `map.lock`   | u8  | lock-to-position on open | 1 |
| `map.source` | u8  | 0=osm 1=pdok 2=opentopo 3=cyclosm 4=stamen | 0 |
| `map.gps_int_s` | u8 | GPS poll interval seconds | 10 |

## 4. Phased implementation

### Phase 0 — research spikes (no commits to feature branch)

Outputs:
- `DEVLOG.md` entry with PNG-decode timing, SD throughput, encoder
  mapping decision (A or B), final tile-layout choice (A or B).

### Phase 1 — Plumbing (empty Map screen)

Files: `UITask.cpp` (this folder), new `map_screen.{cpp,h}`.

- Add `MAP_TILE_INDEX` to the tile-carousel enum (already a placeholder
  in the layout — see memory note "Map → Settings → About").
- On click, create a sub-screen with `lv_obj_create`, label "Map view —
  tile raster comes in Phase 2".
- Long-press (≥ 600 ms) returns to carousel via existing back gesture.
- Build + flash; confirm the Map tile opens a labelled empty screen
  and a back-gesture returns to the carousel.

**Acceptance:** the Map tile opens a labelled placeholder screen; back-
gesture returns; nothing else regressed (Channels / DM / Contacts still
work).

### Phase 2 — Tile rendering (static centre, no GPS)

Files: new `map_tiles.{cpp,h}` + `map_screen.cpp` from Phase 1.

- `map_tiles`: implement `slippy_latlon_to_tile()` and the inverse,
  copy-port from `~/stack/Projects/Tanmatsu/meshcore-settings/main/map.c`.
- LRU cache holding up to **8 RGB565 tiles** in PSRAM
  (`heap_caps_malloc(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`).
- Tile path: `/sd/tiles/<source>/<z>/<x>/<y>.png` (matches `MAPS.md`).
  Active source = `MAP_SOURCE_OSM` for now; Phase 5 wires the picker.
- LVGL `lv_image_decoder_open(LV_IMG_SRC_FILE, "S:/tiles/...")` to stream
  the PNG (LV_USE_LODEPNG already on). Decoded RGBA8 → RGB565 once into
  the cache slot, then `lv_image_set_src(image, &cache[slot].lv_dsc)`.
- Status strip 24 px tall at the top: text "Map · OSM" (real values land
  in Phase 5).
- Initial centre = stored NVS or fallback Den Haag (52.080, 4.310) at
  zoom 10. No pan/zoom keys yet.
- Missing or malformed tile → grey rect placeholder.

**Acceptance:** opening the Map tile shows the Den Haag area at zoom 10
with the chosen layout option from Phase 0 §2.1, with the OSM
attribution visible somewhere in the strip (`(c) OpenStreetMap`).

### Phase 3 — Pan + zoom

Files: `map_screen.cpp` only.

- Mapping A (encoder = zoom): rotate CW → zoom in, CCW → zoom out,
  clamp `[6, 14]`. TCA8418 arrow keys pan by 1/4 tile in each direction
  (1/4 × 256 ≈ 64 px ≈ one viewport-width nudge).
- Pan-clamp longitude to wrap, latitude to ±85.05°.
- Persist NVS centre + zoom debounced 2 s after the last input.

**Acceptance:** rotating zooms, arrow keys pan; the last view restores
after a reboot.

### Phase 4 — Live GPS

Files: new `map_gps.{cpp,h}` (optional thin wrapper around
`SensorManager`), `map_screen.cpp`.

- The GPS is already polled by `SensorManager` per the variant
  `target.cpp`. We don't run our own task — just snapshot
  `node_lat / node_lon / node_sats / node_fix_valid` on each LVGL
  timer tick (1 Hz suffices for a moving badge in the user's hand).
- **Lock-to-position toggle** — long click on the encoder (≤ 1 s,
  shorter than the back gesture) toggles `s_lock`. When on, the centre
  snaps to the live GPS fix.
- **Transport-profile picker** mirrors Tanmatsu: `Walking / Cycling /
  Driving / Manual`. Profile sets the GPS-snapshot interval (no actual
  PA1010D polling on the Pager — the existing SensorManager runs at its
  own rate). The profile is mostly cosmetic on the Pager, but keeps the
  UX symmetric with the badge.

**Acceptance:** with the Pager outside, the status strip shows `SAT:n`
and the centre follows the live fix when lock is on.

### Phase 5 — Source switcher + status strip

Files: `map_screen.cpp` + extend `NodePrefs.h`.

- Status strip (24 px top, dark bg, light text):
  - left: `MAP · <source>` (e.g. `MAP · PDOK`)
  - middle: `SAT:n  z=NN`
  - right: `bat%` (mirrors Tanmatsu styling)
- Bottom 18 px strip: source-name only when the picker is open, else
  the attribution string (e.g. `(c) Kadaster` for PDOK,
  `(c) OpenStreetMap` for OSM). Each source's attribution is in
  `MAPS.md` already.
- Source picker = LVGL list popup on a dedicated key (e.g. `M` for
  Map-style) or click while no edit is active. Selecting a source clears
  the tile cache so the new style renders cleanly.

**Acceptance:** the picker cycles through all five sources, each
re-renders correctly, attribution updates.

### Phase 6 — Persistence + polish

Files: `NodePrefs.h`, `MyMesh.cpp` (load/save plumbing).

- All six `map.*` NVS keys load on boot, save on the same debounced
  path as pan/zoom.
- First-fix toast (one-shot per power-cycle).
- Stale-fix indicator: SAT line goes red and a `STALE` chip appears
  next to it when last-fix age > 60 s.
- Scale bar in bottom-left when the bottom strip is showing
  attribution.

**Acceptance:** reboot restores last centre, zoom, lock, source. Stale
indicator behaves correctly during a forced GPS dropout.

### Phase 7 — Test + push

- Golden path: PDOK source outside, encoder zoom in to z=14, see
  Den Haag street names.
- Edge cases: SD ejected → toast `"No tiles on /sd/tiles/<source>"` and
  exit to carousel. SD has only OSM but user picks PDOK → grey rects
  with a hint `"Run download_tiles.py for pdok"`.
- Build + flash, smoke on the actual Pager hardware.
- Squash into 5-7 logical commits, push to `feat/pager-map-view` on
  Gitea, open PR.

## 5. SD-sharing with the Tanmatsu badge

The Tanmatsu uses `/maps/<profile>/tiles/...` (see
`~/stack/Projects/Tanmatsu/meshcore-settings/main/map.c`) and the Pager
uses `/tiles/<source>/...` (this folder's `MAPS.md`). Three options
when re-using the same SD:

| Option | Effect on Pager | Effect on Tanmatsu | Disk cost |
|---|---|---|---|
| **Symlink** `/maps/carto/tiles` → `/tiles/osm` etc. | works | works | 0 (FAT doesn't really do symlinks though) |
| **Dual layout** — keep both folders | works | works | 2× |
| **One scheme, port the other** — recommend porting Tanmatsu to `/tiles/<source>/` to align with the Pager standard | needs Tanmatsu firmware update + SD reorg | needs Tanmatsu firmware update | 1× |

Recommendation: keep them on **separate SDs** for now (16 GB Tanmatsu
exists; Pager's 32 GB is fresh). Revisit if/when the user wants single-
card shared maps — that's a follow-up commit on the Tanmatsu side.

## 6. Files that change

NEW:

```
variants/lilygo_tlora_pager_lr1121/map_screen.cpp     (LVGL screen + handlers)
variants/lilygo_tlora_pager_lr1121/map_screen.h
variants/lilygo_tlora_pager_lr1121/map_tiles.cpp      (slippy math + LRU cache)
variants/lilygo_tlora_pager_lr1121/map_tiles.h
variants/lilygo_tlora_pager_lr1121/map_gps.cpp        (snapshot helper + profile presets)
variants/lilygo_tlora_pager_lr1121/map_gps.h
```

MODIFIED:

```
variants/lilygo_tlora_pager_lr1121/UITask.cpp          (Map tile click handler + screen dispose)
variants/lilygo_tlora_pager_lr1121/UITask.h            (one extern + enum entry if needed)
examples/companion_radio/NodePrefs.h                   (map.* fields)
examples/companion_radio/MyMesh.cpp                    (load/save plumbing)
variants/lilygo_tlora_pager_lr1121/DEVLOG.md           (S3.6 phase entries)
```

## 7. References to current code (verify before editing — moves over time)

- LVGL config + bundled decoders:
  `variants/lilygo_tlora_pager_lr1121/lv_conf.h:774` (LV_USE_LODEPNG).
- Display geometry:
  `variants/lilygo_tlora_pager_lr1121/TPagerST7796Display.h:87` (480 × 222).
- Existing tile-carousel layout + back-gesture wiring:
  `variants/lilygo_tlora_pager_lr1121/UITask.cpp`.
- GPS snapshot fields:
  `helpers/sensors/SensorManager.h` — `node_lat`, `node_lon`,
  `node_fix_valid`, `node_sats`.
- Tile-source table + attribution + per-source NL size estimates:
  `variants/lilygo_tlora_pager_lr1121/MAPS.md`.
- Reference port for slippy math, LRU eviction, GPS task pattern:
  `~/stack/Projects/Tanmatsu/meshcore-settings/main/{map.c,map.h,gps_task.c}`.

## 8. Open decisions to surface during the goal session

1. **Phase 0 §2.1 — 256 vs 128 px tiles.** Default 256; decide after
   the first visual.
2. **Phase 0 §2.3 — encoder = zoom (A) vs encoder = pan (B).**
   Default A.
3. **Phase 4 — GPS task vs SensorManager-snapshot.** Default snapshot.
   Bump to a real task only if 1 Hz refresh causes visible jitter while
   moving.
4. **§5 — SD sharing with Tanmatsu.** Default: separate cards. Revisit
   later.
5. **NVS namespace.** Keep `map.*` flat under the existing prefs
   namespace, or carve a `map.` sub-namespace. Default: flat, matches
   the Tanmatsu style.

## 9. How to invoke this plan

In a fresh Claude Code session inside the Pager repo:

```
/goal Implementeer Phase 0 t/m Phase 7 uit
variants/lilygo_tlora_pager_lr1121/PLAN_MAP_VIEW.md. Werk in branch
feat/pager-map-view, vertrek vanaf feature/lilygo-tlora-pager-lr1121.
Vraag pas iets aan mij als een "Open beslissing" uit sectie 8 nodig is.
```

Phase 0 outputs (PNG-decode latency + encoder choice + SD throughput)
unblock the rest. Finish Phase 0 before starting Phase 1 even if the
goal-runner is autonomous.
