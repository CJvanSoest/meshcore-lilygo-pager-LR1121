# T-Pager VIEW_MAP — devlog

Plan: [PLAN_MAP_VIEW.md](PLAN_MAP_VIEW.md). Branch: `feat/pager-map-view`
off `feature/lilygo-tlora-pager-lr1121`.

## 2026-06-18 — Phase 0 kick-off

### §2.1 Layout — 256 vs 128 px tiles

Pinned **Option A (256 px native, 2×1 horizontal raster)**.

- Viewport `480 × 222`. After the 24 px top status strip + 18 px bottom
  attribution we have `480 × 180`. Two 256 px-wide tiles fit the X axis
  (centre seam between two adjacent X tiles); a 256 px-tall tile
  overflows the Y axis by ~38 px above + below — clipped at the strip
  edges, no LVGL scaling needed.
- Option B (128 px down-scaled) would halve cache footprint but
  re-render is cheap on PSRAM (~64 KB per RGB565 tile, 8 slots = 512 KB
  out of 8 MB). Cache cost is not the binding constraint, decode
  latency is — keeping native 256 px avoids per-frame
  `lv_image_set_scale_*` overhead and is a direct copy-port from
  Tanmatsu `map.c`. Re-render at 128 only if A's edge clipping looks
  visually bad on hardware.

### §2.3 Encoder + click semantics — Mapping A

Pinned **Mapping A**: encoder rotate = zoom, click = source picker
toggle, TCA8418 arrow keys = pan, encoder long-press = back (matches
existing carousel `back_long_press_event` in `UITask.cpp:1780`).

- Matches the rest of the carousel: rotate = magnitude, click = open,
  long-press = back.
- Existing encoder API: tile_clicked_event + LV_EVENT_LONG_PRESSED on
  `s_root` already cover the click and back behaviours. The map view
  just rebinds the LVGL encoder group to a map-local group when the
  Map tile opens.

### §2.2 PNG decode latency — **PENDING (hardware spike)**

Requires a real Pager + a sample tile PNG on the SD. Will measure
`lv_image_decoder_open` + `lv_image_decoder_read_line` × 256 with
`micros()` once the Pager-side SD has at least one `/tiles/osm/14/<x>/<y>.png`
present. Target < 200 ms; fallback (raw RGB565 pre-conversion) only
kicks in if measured > 500 ms.

### §2.4 SD read throughput — **PENDING (hardware spike)**

Same gate as §2.2. Time `fread` of a 30 KB tile via the existing SD
wrapper. Target ≥ 5 tiles/sec (≤ 200 ms each); fallback identical.

### Phase 0 → Phase 1 gate

Per plan §4, finishing Phase 0 is a prerequisite for Phase 1. The two
**hardware spikes are not blocking the Phase 1 plumbing** — that is
pure LVGL screen wiring with no tile decoding yet. Phase 1 ships with
the placeholder body; Phase 2 is what depends on the §2.2/§2.4 numbers,
because the cache + decoder path is what those spikes measure.

Recording: code-only Phase 0 decisions (§2.1, §2.3) are landed in this
devlog; Phase 1 plumbing follows in the next commit. The §2.2/§2.4
spikes get their own devlog entry once measured on the Pager with the
first test tile available.

## 2026-06-18 — Pager SD inspection

User mounted the Pager's 32 GB microSD. It is **not fresh** as the
plan §5 assumed — it already carries the Ripple Radio Europe tileset
from a prior firmware:

```
/tiles/<z>/<x>/<y>.png   (Ripple-style, no <source> directory)
z=1..10, ~25.8k tiles total, 256×256 PNG 8-bit colormap, ~2-16 KB each
```

No z=11..14 tiles — street-name scale will need a fresh download via
`tools/download_tiles.py` later.

Decision: **firmware accepts both schemas**. MAPS.md updated with the
fallback section; `map_tiles::format_tile_path` / `_legacy` provide
the two snprintf shapes Phase 2 will try in order. Source-specific
tiles win over the legacy fallback when both exist for the same
(z, x, y). No SD-side mutation — the Ripple tiles stay in place.

This also unblocks Phase 0 §2.2 (PNG decode latency) and §2.4 (SD
throughput) — the existing z=10 tiles are real-world 256×256 PNGs
suitable for the spike.

## 2026-06-18 — Blocker: SD card not mounted in firmware

Visual test of Phase 1 on hardware passed (placeholder body renders,
CLICK + back-gesture work). Tried to scope Phase 2 raster path and
hit a missing prerequisite:

- `lv_conf.h` has `LV_USE_FS_POSIX = 1`, drive letter `'A'`, base path
  `/sd`. So LVGL paths would be `"A:/tiles/<z>/<x>/<y>.png"`, **not**
  `"S:/..."` as written in the plan. PLAN_MAP_VIEW.md §4 Phase 2 will
  need updating to use the `A:` prefix.
- BUT `/sd` is not actually mounted. `grep -r SD.begin|SD_MMC|sdmmc`
  across the variant + companion examples returns zero hits. The
  Pager README (`§S3.6d.2`, `§S3.7`) already lists SD bringup as a
  prereq for both DM persistence and the map renderer.
- SD pinmap (LilyGoLib upstream): `SD_CS=21`, SPI bus shared with the
  LR1121 (`SCLK=35 MOSI=34 MISO=33`); power-enable + detect through
  the XL9555 I2C expander (`EXPANDS_SD_EN=12`, `EXPANDS_SD_DET=10`,
  `EXPANDS_SD_PULLEN=11`).

Bringing SD up safely against a shared LoRa SPI bus is its own
sprint — it needs CS-arbitration with the radio, power-sequencing
via the expander, and a card-detect path. That work belongs in
S3.6d.2 (already on the README backlog), not inside this Phase 2.

Phase 2 raster path will resume once SD mount lands. The slippy
math + path helpers from earlier commits are ready to plug in.

## 2026-06-18 — Phase 2 single-tile renders on hardware

SD bringup landed (S3.6d.2) and the Map view now displays the Den
Haag z=10 OSM tile from the existing Ripple Radio Europe set on the
Pager SD. Pipeline confirmed end-to-end:

```
slippy(52.080, 4.310, 10) -> tile (524, 337, px 66, 223)
-> /sd/tiles/10/524/337.png (legacy schema)
-> A:/tiles/10/524/337.png (LVGL drive)
-> lv_image_set_src -> lv_lodepng -> ARGB8888 256x256
-> blit to 480x222 ST7796
```

Two LVGL config defaults were silent blockers — both fixed in
`lv_conf.h`:

1. **`LV_CACHE_DEF_SIZE` was `0`.** With cache disabled, lodepng
   releases the decoded buffer after the first draw. Image widget
   renders nothing on subsequent frames. Bumped to **1 MB** so we
   can keep ~4 decoded 256x256 ARGB tiles around — enough for the
   Phase 2 single-tile path and the eventual 2x1 raster from §2.1.
2. **`LV_USE_STDLIB_MALLOC` was `LV_STDLIB_BUILTIN`.** The builtin
   pool is a 64 KB internal-RAM static array; a single 256 KB ARGB
   tile alloc immediately fails with no surface error other than
   `Error decoding PNG` (lodepng error code is swallowed by the
   draw_buf alloc wrapper). Switched to **`LV_STDLIB_CLIB`** so
   malloc falls back to PSRAM for big chunks — required for any
   `lv_image` work that touches a PSRAM device.

Debug timeline (kept for future LVGL allocator headaches):
- `lv_fs_open` returns OK + 35.7 KB → FS driver fine.
- `lv_image_decoder_open` returns `LV_RESULT_INVALID` → decoder fails
  before producing a header.
- `LV_USE_LOG=1` + `LV_LOG_PRINTF=1` reveal
  `lv_lodepng.c:173 Error decoding PNG` (generic; the inner code
  from `lodepng_decode32` is lost).
- After cache + malloc fix: same diagnostic prints disappear and the
  tile renders.

Phase 2 minimum acceptance: ✅ on-screen Den Haag tile. The 2x1 raster
from plan §2.1 Option A + a status strip / GPS lock are still to come
in Phase 3-5. The Phase 0 §2.2 / §2.4 timing spikes can now be folded
into the next session since real-world tile decode + SD read are
working.

## 2026-06-19 — Phase 2 finishing: 2x1 raster + Map-aware top row

Hardware-confirmed the full Option A layout: two adjacent z=10 OSM
tiles render with the seam at viewport centre, antimeridian-safe via
`map_tiles::wrap_tile_x`. Status strip + DC + battery readouts share
the top 22 px without overlap on the 480 px display.

Layout reorganisation in `UITask.cpp`:
- Map case 5 hides `s_header_label` (Pager-name) and the bottom
  `s_back_hint` (edit-mode hint is meaningless inside the Map view).
  Both restore in `leave_subscreen`.
- `s_dc_label` is re-anchored from `TOP_MID` to the left of
  `s_battery_label` on Map open so the orange "MAP - osm  z=N
  (X-X+1, Y)" string has the left half to itself.
- `s_back_hint` was promoted from a `build_ui` local to a file-static
  so the toggle stays cheap.

`map_screen.cpp`:
- Split `s_tile_img` into `s_tile_img_l` / `s_tile_img_r`, each
  resolved independently (primary `<source>/` first, legacy fallback
  second). One missing tile blanks only its half.
- Status strip drawn after the tiles, opaque black background, so the
  orange text stays legible against any tile colours below.

## 2026-06-19 — BQ25896 power-off requires USB unplug first

Spent ~30 min thinking the encoder ≥3 s power-off gesture was broken
after the Phase 2 work. It wasn't. The polling code fires correctly
and `tpager_power_off()` (target.cpp:371) writes `BATFET_DIS` (bit 5
of REG09H) on the BQ25896 — which **only** disconnects the battery
from the PMID system rail. With USB plugged in, VBUS keeps the rail
alive, so the call appears to do nothing.

Procedure: unplug USB, then hold encoder ≥3 s and release. Display
goes black. Captured in
[bq25896_poweroff_usb_quirk](../../../../../.claude/projects/-Users-cjvs/memory/bq25896_poweroff_usb_quirk.md)
auto-memory for the next BQ25896-board debug session.
