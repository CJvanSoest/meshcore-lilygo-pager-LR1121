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
