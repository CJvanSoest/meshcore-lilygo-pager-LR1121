# LilyGo T-Lora Pager (LR1121) — MeshCore variant

Multi-band T-Pager port for MeshCore. The LR1121 variant of the Pager
ships with the multi-band Semtech LR1121 radio (sub-GHz + 2.4 GHz);
this folder also covers the rest of the on-board hardware (ST7796 IPS
display, TCA8418 keyboard, rotary encoder, AXP-style PMU, BQ27220 fuel
gauge, MicroNMEA GPS, XL9555 IO expander).

## Powering the device on and off

* **Power on:** the small dedicated power button on the side of the
  Pager (the one between the antennas) — same as the LilyGo factory
  firmware.
* **Power off:** hold the **encoder / scroll button for ≥ 3 seconds**.
  We pull the system rail down via the BQ25896 charger's BATFET_DIS
  bit (`tpager_power_off()` in `target.cpp`, mirrors
  `XPowersLib::PowersBQ25896::shutdown()`).
* The 600 ms encoder hold is reserved for the "back" gesture inside
  the LVGL UI. Only ≥ 3 s actually powers the device down.

## Build environments

| env name | purpose | based on |
|---|---|---|
| `T_LoRa_Pager_LR1121_repeater` | full-featured relay node with a thin status UI + chat compose | `examples/simple_repeater` |
| `T_LoRa_Pager_LR1121_companion_radio_usb` | standalone client with LVGL tile-carousel UI (LilyGo-demo style) | `examples/companion_radio`, with this folder's `UITask.cpp` replacing the stock `ui-new/UITask.cpp` |

Both pull in `helpers/ui/LGFXDisplay.cpp` and the variant's own
`TPagerST7796Display.h`, `target.cpp`, `TPagerLVGL.cpp` and `UITask.cpp`.

## Navigation (companion build, LVGL UI)

- **Encoder rotation** scrolls / changes value. Direction is inverted
  while inside sub-screens (vertical lists) so "rotate up" moves the
  focus up.
- **Click** opens a tile, applies a setting, or toggles a boolean
  inline. If the focus didn't land on the row the first click "wakes"
  it — in that case a **double-click** is the natural follow-through.
- **Long-press (≥600 ms)** goes back. From a settings edit popup it
  cancels without applying; from a sub-screen it returns to the
  carousel.

The companion `UITask` is replaced by our LVGL variant via the
`build_src_filter` exclusion in `platformio.ini` (the include path order
also ensures our `UITask.h` shadows the upstream one).

## Pin map / hardware

Mirrors `Xinyuan-LilyGO/LilyGoLib-PlatformIO/variants/lilygo_tlora_pager/pins_arduino.h`.

- LoRa (LR1121): `CS=36 IRQ=14 RST=47 BUSY=48` on the shared SPI bus
  (`SCLK=35 MOSI=34 MISO=33`).
- Display (ST7796): `CS=38 DC=37 BL=42`. Panel mounted 180° (offset
  rotation 2); LGFXDisplay applies +90° on top, so the effective layout
  is landscape 480×222.
- Encoder: `ROTARY_A=40 ROTARY_B=41 ROTARY_C=7` (push).
- Keyboard (TCA8418 on I²C 0x34): 4×10 matrix.
- I²C bus: `SDA=3 SCL=2`.
- XL9555 IO expander on 0x20 with `EXPANDS_LORA_EN=3`,
  `EXPANDS_KB_EN=8`, `EXPANDS_KB_RST=2` — the LR1121 power rail and
  the keyboard reset are routed through the expander, not direct GPIO.

## Keyboard layout

| layer    | how to engage                       | what it produces                                    |
|----------|-------------------------------------|-----------------------------------------------------|
| QWERTY   | tap a letter key                    | the silk-screened letter (a-z, Enter, Space, Backspace) |
| Symbol   | hold the **orange FN key** next to Z | the silk-screened symbol on the same key            |

Symbol-layer map (matches the physical hardware labels — taken from
LilyGoLib's `LilyGo_LoRa_Pager.cpp` `symbol_map`):

```
FN+Q..P → 1 2 3 4 5 6 7 8 9 0
FN+A..L → * / + - = : ' " @
FN+Z..M → _ $ ; ? ! , .
FN+Space → #         (our addition — needed for MeshCore channel names)
```

The TCA8418 FIFO is drained from `UITask::loop` via the
`variant_loop()` hook in `target.cpp`. Upstream `examples/companion_radio`
doesn't call any variant hook on its own, so we drive it explicitly.

The Messages tile is an input-test screen that echoes the live keystroke
buffer — handy for verifying the keyboard end-to-end without a host
serial monitor.

## Known LVGL caveats

These cost some debug time and are documented here so the next pair of
hands doesn't have to rediscover them:

- **LVGL needs a tick source.** Call
  `lv_tick_set_cb((uint32_t(*)())millis)` once before `lv_timer_handler`
  — without it timers (including the indev read-cb) never fire.
- **Encoder indev does not raise `LV_EVENT_LONG_PRESSED`** on focused
  widgets. The long-press is reserved for the group's edit-mode
  transition. Detect long-press yourself in the input poll.
- **Encoder `enc_diff` accumulator** needs a cap (`±4` here) and
  `continue_reading=false`, otherwise a fast spin drains the queue in
  one cycle and feels like the focus jumped to the end.
- **`LV_USE_STDLIB_MALLOC=LV_STDLIB_CUSTOM`** in the canonical LilyGo
  `lv_conf.h` expects you to provide `lv_malloc_core`/`lv_free_core`.
  We switched to `LV_STDLIB_BUILTIN` since we don't ship those symbols.
- **Encoder + spinbox: don't call `lv_spinbox_step_prev`** at creation
  time. It bumps the step to 10, which collapses to the range bounds
  for small ranges (SF 5..12 only shows 5 or 12).

## Status as of S3.3 phase 2c

- Tile carousel with 6 tiles, encoder navigation, click/long-press semantics.
- Radio sub-screen renders the live `NodePrefs` values in a two-column
  list (label + value, right-aligned) with thin row separators.
- Editor matrix per row:
  - `Freq` → text-input popup (type digits + `.` via FN+QWERTY symbol
    layer; Enter or encoder-click commits; long-press cancels; range
    validated to 400.0…960.0 MHz)
  - `SF`, `CR`, `TX power` → spinbox popup (encoder rotates value,
    click saves)
  - `BW` → dropdown over 10 standard LoRa bandwidths (7.81 … 500 kHz)
  - `Path hash` → dropdown over the three valid path-hash modes
    (1 byte / 2 bytes / 3 bytes)
  - `RX boost` → inline toggle (no popup)
- Apply path calls the weak bridge `ui_apply_radio_changes()` from
  `examples/companion_radio/main.cpp` (`radio_set_params` +
  `radio_set_tx_power` + `savePrefs`). Path-hash changes only need the
  `savePrefs` (consumed inline on every send).
- Long-press in the popup cancels without applying; dropdown list is
  explicitly closed and the group drops out of edit mode so the
  encoder doesn't hang.
- The list auto-scrolls to keep the focused row in view
  (`LV_OBJ_FLAG_SCROLL_ON_FOCUS`).

## TODO (next phases)

- S3.3 phase 2d: duty-cycle indicator (likely top header, next to
  battery).
- S3.3 phase 2e: region scope picker (reads / writes RegionMap).
- S3.3 phase 2d: region scope picker (reads / writes RegionMap).
- S3.3 phase 2e: duty-cycle indicator (likely top header, next to battery).
- S3.4 prereq: symbol layer on the QWERTY keyboard (`Space + key`) — needed
  before chat / channel-name input can land any digit or `#`.
- S3.4: channel list + message history viewer (with `+ Add channel`
  via text-input — no hardcoded NL-specific channel names in the
  upstream code).
- S3.5: contacts tile (heard adverts) with favourite flag and a
  not-favourite-first eviction policy when the 350-contact cap is hit.
- S3.6: map renderer — XYZ raster tiles from microSD. Five base
  sources shipped with the firmware (OSM, PDOK for NL, OpenTopoMap,
  CyclOSM, Stamen Toner). See `MAPS.md` for the source list, tile-URL
  templates, the planned in-map source-switcher UX and links to free
  providers for other countries / world-wide use.
- S3.7: at-rest encrypted message store on SD (AES-256-CTR with a key
  derived from `efuse_mac || optional_passphrase`). DM threads and
  channel chat histories land in `/messages/<hash>.jsonl` instead of
  SPIFFS.
