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
