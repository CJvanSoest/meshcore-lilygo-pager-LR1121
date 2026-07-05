#include <Arduino.h>
#include <Wire.h>
#include <SD.h>
#include <Adafruit_TCA8418.h>
#include "target.h"

// SD card wiring (LilyGoLib upstream pins_arduino.h for the T-Pager).
// CS sits on a dedicated GPIO; SCLK/MISO/MOSI share the LR1121's SPI
// bus. Power-enable + card-detect run through the XL9555 expander.
#ifndef SD_CS_PIN
  #define SD_CS_PIN 21
#endif
#define EXPANDS_SD_DET    10
#define EXPANDS_SD_PULLEN 11
#define EXPANDS_SD_EN     12

TLoraPagerBoard board;

#ifdef DISPLAY_CLASS
  LGFX_TPager tpager_lgfx_panel;
  DISPLAY_CLASS display;
  #ifndef USER_BTN_LONG_PRESS_MS
    #define USER_BTN_LONG_PRESS_MS 1000
  #endif
  MomentaryButton user_btn(PIN_USER_BTN, USER_BTN_LONG_PRESS_MS, true);
#endif

static SPIClass spi;

RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, spi);

WRAPPER_CLASS radio_driver(radio, board);

// Battery-backed PCF85063A real-time clock (I2C 0x51 on the shared bus). The
// pager's RTC keeps time across reboots / power-off on its backup supply, so
// the clock survives a reboot instead of resetting to a stale volatile value.
// MeshCore's AutoDiscoverRTCClock only knows PCF8563 (different register map,
// same address) so we drive the PCF85063 directly and keep an ESP32 volatile
// clock as a within-session fallback when the chip has no valid time yet.
#include <RTClib.h>            // DateTime epoch<->calendar (already a build dep)
#ifndef PCF85063_I2C_ADDR
  #define PCF85063_I2C_ADDR 0x51
#endif
class PCF85063Clock : public mesh::RTCClock {
  ESP32RTCClock _sys;
  static uint8_t bcd2dec(uint8_t b) { return (uint8_t)((b >> 4) * 10 + (b & 0x0F)); }
  static uint8_t dec2bcd(uint8_t d) { return (uint8_t)(((d / 10) << 4) | (d % 10)); }
  bool readRegs(uint8_t start, uint8_t* buf, uint8_t n) {
    Wire.beginTransmission(PCF85063_I2C_ADDR);
    Wire.write(start);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)PCF85063_I2C_ADDR, (int)n) != n) return false;
    for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
    return true;
  }
public:
  void begin() {
    _sys.begin();
    // Control_1 (0x00): STOP=0 (run), 12_24=0 (24-hour), CAP_SEL default.
    Wire.beginTransmission(PCF85063_I2C_ADDR);
    Wire.write(0x00); Wire.write(0x00);
    Wire.endTransmission();
  }
  uint32_t getCurrentTime() override {
    uint8_t b[7];
    if (readRegs(0x04, b, 7)) {
      const bool os = (b[0] & 0x80) != 0;   // oscillator stopped => time lost
      const int  sec = bcd2dec(b[0] & 0x7F);
      const int  mi  = bcd2dec(b[1] & 0x7F);
      const int  hr  = bcd2dec(b[2] & 0x3F);
      const int  day = bcd2dec(b[3] & 0x3F);
      const int  mon = bcd2dec(b[5] & 0x1F);
      const int  yr  = 2000 + bcd2dec(b[6]);
      if (!os && yr >= 2024 && mon >= 1 && mon <= 12 && day >= 1 && day <= 31)
        return DateTime(yr, mon, day, hr, mi, sec).unixtime();
    }
    return _sys.getCurrentTime();           // chip absent / not yet set
  }
  void setCurrentTime(uint32_t t) override {
    _sys.setCurrentTime(t);
    DateTime dt(t);
    Wire.beginTransmission(PCF85063_I2C_ADDR);
    Wire.write(0x04);
    Wire.write(dec2bcd(dt.second()));       // bit7 (OS) cleared by this write
    Wire.write(dec2bcd(dt.minute()));
    Wire.write(dec2bcd(dt.hour()));
    Wire.write(dec2bcd(dt.day()));
    Wire.write((uint8_t)(dt.dayOfTheWeek() & 0x07));
    Wire.write(dec2bcd(dt.month()));
    Wire.write(dec2bcd((uint8_t)(dt.year() - 2000)));
    Wire.endTransmission();
  }
  void tick() override { _sys.tick(); }
};

PCF85063Clock fallback_clock;
AutoDiscoverRTCClock rtc_clock(fallback_clock);

#if ENV_INCLUDE_GPS
  #include <helpers/sensors/MicroNMEALocationProvider.h>
  MicroNMEALocationProvider nmea = MicroNMEALocationProvider(Serial1, &rtc_clock);
  EnvironmentSensorManager sensors = EnvironmentSensorManager(nmea);
#else
  EnvironmentSensorManager sensors;
#endif

// GPS status bridges for the Map status strip (UITask / map_screen).
extern "C" int ui_get_sat_count() {
#if ENV_INCLUDE_GPS
  return (int)nmea.satellitesCount();
#else
  return -1;
#endif
}
extern "C" bool ui_get_gps_valid() {
#if ENV_INCLUDE_GPS
  return nmea.isValid();
#else
  return false;
#endif
}
// Satellites in VIEW (from GSV) — non-zero as soon as the antenna hears
// anything, unlike ui_get_sat_count() (sats used in the fix). -1 = no GPS.
extern "C" int ui_get_sat_in_view() {
#if ENV_INCLUDE_GPS
  return nmea.getSatsInView();
#else
  return -1;
#endif
}
// Best carrier-to-noise (C/N0, dBHz) seen across the constellations this
// second. A healthy sky gives 30-45; 0 means no usable signal. -1 = no GPS.
extern "C" int ui_get_gps_best_snr() {
#if ENV_INCLUDE_GPS
  return nmea.getBestSNR();
#else
  return -1;
#endif
}

#ifndef LORA_CR
  #define LORA_CR 5
#endif

#ifndef XL9555_I2C_ADDR
  #define XL9555_I2C_ADDR 0x20
#endif

static bool xl9555_set_output(uint8_t pin, bool high) {
  // XL9555 has 16 GPIOs (P0-P7 on port 0, P8-P15 on port 1)
  // Configure pin as output and set level. We use a simple per-call read-modify-write.
  uint8_t port_reg_cfg  = (pin < 8) ? 0x06 : 0x07;  // configuration registers
  uint8_t port_reg_out  = (pin < 8) ? 0x02 : 0x03;  // output port registers
  uint8_t bit = pin & 0x07;

  // Read current config
  Wire.beginTransmission(XL9555_I2C_ADDR);
  Wire.write(port_reg_cfg);
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom(XL9555_I2C_ADDR, (uint8_t)1);
  if (!Wire.available()) return false;
  uint8_t cfg = Wire.read();

  // Configure as output (0 = output, 1 = input)
  cfg &= ~(1 << bit);
  Wire.beginTransmission(XL9555_I2C_ADDR);
  Wire.write(port_reg_cfg);
  Wire.write(cfg);
  if (Wire.endTransmission() != 0) return false;

  // Read current output value
  Wire.beginTransmission(XL9555_I2C_ADDR);
  Wire.write(port_reg_out);
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom(XL9555_I2C_ADDR, (uint8_t)1);
  if (!Wire.available()) return false;
  uint8_t out = Wire.read();

  // Set/clear bit
  if (high) out |= (1 << bit);
  else      out &= ~(1 << bit);

  Wire.beginTransmission(XL9555_I2C_ADDR);
  Wire.write(port_reg_out);
  Wire.write(out);
  return Wire.endTransmission() == 0;
}

// --- SD card bringup ----------------------------------------------------------

static bool s_sd_mounted = false;

bool sd_init() {
  if (s_sd_mounted) return true;

  // Power the SD slot via XL9555 EXPANDS_SD_EN (pin 12). Without this,
  // the SD socket has no Vcc and SD.begin() fails with "no card".
  if (!xl9555_set_output(EXPANDS_SD_EN, true)) {
    Serial.println("WARN: XL9555 SD_EN write failed (expander reachable?)");
    return false;
  }
  delay(10);  // settle for the regulator

  // CS idle HIGH so the SD doesn't latch a stray transaction while the
  // LR1121 is talking on the same SPI bus.
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);

  // Make sure the shared SPI bus is up. spi.begin() is idempotent on the
  // Arduino-ESP32 core, so calling it here makes sd_init() standalone
  // even when radio_init() has not run yet.
  spi.begin(P_LORA_SCLK, P_LORA_MISO, P_LORA_MOSI);

  // 4 MHz matches LilyGoLib upstream. The Adafruit SD library negotiates
  // the actual clock down for cards that can't keep up.
  if (!SD.begin(SD_CS_PIN, spi, 4000000U, "/sd")) {
    Serial.println("ERROR: SD.begin failed (no card / unformatted / wiring?)");
    return false;
  }
  uint64_t mb = SD.cardSize() / (1024ULL * 1024ULL);
  Serial.printf("SD: mounted at /sd, %llu MB\n", mb);
  s_sd_mounted = true;
  return true;
}

void TLoraPagerBoard::begin() {
  ESP32Board::begin();

  // Encoder push button (ROTARY_C, GPIO 7) — active-low with internal pull-up.
#ifdef PIN_USER_BTN
  pinMode(PIN_USER_BTN, INPUT_PULLUP);
#endif
#ifdef PIN_ENCODER_A
  pinMode(PIN_ENCODER_A, INPUT_PULLUP);
#endif
#ifdef PIN_ENCODER_B
  pinMode(PIN_ENCODER_B, INPUT_PULLUP);
#endif

  // Bring up I2C on the LilyGo T-Pager bus (SDA=3, SCL=2 per LilyGoLib pinmap)
  Wire.begin(PIN_BOARD_SDA, PIN_BOARD_SCL);

  // One-time I2C bus scan to verify which devices respond. Useful for
  // diagnosing BQ27220 / XL9555 wiring issues.
  Serial.println("I2C scan:");
  for (uint8_t addr = 0x08; addr < 0x78; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  found 0x%02X\n", addr);
    }
  }

  // Enable LoRa power rail via XL9555 expander pin 3 (EXPANDS_LORA_EN)
  // Without this, the LR1121 is unpowered and radio_init() will fail.
  if (!xl9555_set_output(3, true)) {
    Serial.println("WARN: XL9555 LORA_EN write failed (expander reachable?)");
  }
  delay(10);  // small settle for the regulator

  // Keyboard: power on via XL9555 EXPANDS_KB_EN (pin 8), pulse-reset via
  // EXPANDS_KB_RST (pin 2), then init the TCA8418 matrix scanner.
  xl9555_set_output(8, true);    // KB_EN HIGH — power keyboard
  delay(2);
  xl9555_set_output(2, false);   // KB_RST LOW
  delay(5);
  xl9555_set_output(2, true);    // KB_RST HIGH — out of reset
  delay(20);

  // GPS (L76K): power the rail via XL9555 EXPANDS_GPS_EN (pin 4) and lift
  // the module out of reset via EXPANDS_GPS_RST (pin 7). Without GPS_EN the
  // module has no Vcc, sends no NMEA, and EnvironmentSensorManager's boot
  // probe (Serial1.available() within 1 s) sees nothing -> it marks GPS
  // "not detected" and disables it permanently, so no fix is ever acquired.
  // Must run before sensors.begin()/initBasicGPS() in main.cpp setup().
#if ENV_INCLUDE_GPS
  #define EXPANDS_GPS_EN  4
  #define EXPANDS_GPS_RST 7
  xl9555_set_output(EXPANDS_GPS_RST, false);  // hold in reset
  xl9555_set_output(EXPANDS_GPS_EN,  true);   // power the GPS rail
  delay(50);                                  // let the regulator settle
  xl9555_set_output(EXPANDS_GPS_RST, true);   // release reset -> module boots
  delay(20);
#endif
}

// --- TCA8418 keyboard ---------------------------------------------------------
static Adafruit_TCA8418 _kb;
static bool _kb_ok = false;

// QWERTY keymap copied from LilyGo_LoRa_Pager.cpp (4 rows x 10 cols).
// '\0' marks a position with no key (or a modifier handled elsewhere —
// e.g. the orange FN key sits at row 2 col 0 next to Z, see kb_keymap_symbol).
static const char kb_keymap[4][10] = {
  {'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p'},
  {'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', '\n'},
  {'\0','z', 'x', 'c', 'v', 'b', 'n', 'm', '\0','\0'},
  {' ', '\0','\0','\0','\0','\0','\0','\0','\0','\0'}
};

// Symbol layer — engaged while the dedicated orange FN key (row 2 col 0,
// next to Z) is held down. This table mirrors the labels silk-screened
// on the physical T-Pager keys, copied from LilyGoLib's
// LilyGo_LoRa_Pager.cpp `symbol_map`, so D → '+', F → '-' etc. match
// the printed hardware glyphs.
//
// One extension over the factory map: FN+Space → '#'. MeshCore channel
// names start with '#' (e.g. `#mc-radar`) and the LilyGo layout has no
// other place to bind the character. Space-on-its-own keeps its meaning
// (the Space key is unmodified). FN-tap-without-other-keys never emits
// anything, so this binding doesn't shadow regular Space input.
static const char kb_keymap_symbol[4][10] = {
  {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'},
  {'*', '/', '+', '-', '=', ':', '\'','"', '@', '\n'},
  {'\0','_', '$', ';', '?', '!', ',', '.', '\0','\0'},
  {'#', '\0','\0','\0','\0','\0','\0','\0','\0','\0'}
};
#define TPAGER_KB_FN_RAW 21    // row 2, col 0 — the orange "FN" key
// Caps-lock key: the unmarked key right of M (row 2, col 8, raw = 29).
// LilyGo's factory layout leaves this slot blank in their symbol_map.
// We bind it to a sticky caps-lock toggle so letters can be capitalised
// without holding a shift modifier (the board has no Shift key).
#define TPAGER_KB_CAPS_RAW 29

static void kb_begin() {
  _kb_ok = _kb.begin(TCA8418_DEFAULT_ADDR, &Wire);
  if (!_kb_ok) {
    Serial.println("WARN: TCA8418 begin failed");
    return;
  }
  _kb.matrix(4, 10);
  _kb.flush();
  Serial.println("TCA8418: 4x10 matrix ready");
}

// Backspace key raw code observed on the LR1121 hardware: 0x1E (row 2
// col 9 by our (k-1)/10 decode). LilyGoLib lists 0x1D in their generic
// config, but on this board the right-most key in row 2 emits 0x1E.
// Verified via the freq-popup diag overlay.
#ifndef TPAGER_KB_BACKSPACE_RAW
  #define TPAGER_KB_BACKSPACE_RAW 0x1E
#endif

// Called from main loop() via the variant_loop weak hook. Drains pending
// key events from the TCA8418 FIFO; key-down events are forwarded to
// main.cpp's ui_input_char() (weak — null in non-UI builds) which routes
// them to UITask when the compose screen is active.
extern "C" void ui_input_char(char c) __attribute__((weak));

// Last raw key event the TCA8418 emitted (low 7 bits = key code, bit 7 =
// press flag). Read via tpager_kb_last_raw() — used by the freq-editor
// debug overlay to identify mystery keys that don't map to anything in
// kb_keymap. Cleared on read so a slow UI sees each event once.
static volatile uint8_t s_kb_last_raw_event = 0;

extern "C" uint8_t tpager_kb_last_raw() {
  uint8_t v = s_kb_last_raw_event;
  s_kb_last_raw_event = 0;
  return v;
}

#ifdef TPAGER_CHARGE_DEBUG
extern "C" void tpager_charge_debug();   // defined below, near the BQ25896 code
#endif

extern "C" void variant_loop() {
  if (!_kb_ok) { kb_begin(); return; }   // try late init if I2C wasn't ready

  uint32_t now_ms = millis();

#ifdef TPAGER_CHARGE_DEBUG
  // Periodic charge-state diagnostic — tells us whether "stops at N%" is the
  // charger terminating early or the BQ27220 gauge under-reading. Once/3 s so
  // it never adds meaningful bus load. Sniff over USB with pyserial dtr=True.
  // Off by default: the Serial spam interleaves with companion-protocol frames.
  static uint32_t s_last_chg_dbg = 0;
  if (now_ms - s_last_chg_dbg >= 3000) { s_last_chg_dbg = now_ms; tpager_charge_debug(); }
#endif

  // Static so the modifier state survives across loop ticks — FN can be
  // pressed for hundreds of millis before the user reaches the next key.
  static bool s_fn_held = false;
  static bool s_caps_lock = false;     // sticky; toggled by the key right of M

  // Throttle the TCA8418 FIFO poll. _kb.available() is an I2C read on the
  // shared Wire bus (RTC / XL9555 / BQ25896 also live there); running it every
  // loop iteration floods the bus and starves the other devices, which surfaces
  // as general UI lag. The key FIFO buffers events, so draining it every ~15 ms
  // batches the I2C traffic with no perceptible typing latency.
  static uint32_t s_last_kb_poll = 0;
  if (now_ms - s_last_kb_poll < 15) return;
  s_last_kb_poll = now_ms;

  while (_kb.available()) {
    uint8_t ev = _kb.getEvent();
    bool pressed = (ev & 0x80) != 0;
    uint8_t k = ev & 0x7F;

    // Stash for the diag overlay (Freq popup), whether or not we end up
    // emitting a char. Most-recent event wins; UI clears on read.
    if (pressed) s_kb_last_raw_event = ev;

    // FN modifier (orange key next to Z) — track press/release, no char.
    if (k == TPAGER_KB_FN_RAW) {
      s_fn_held = pressed;
      Serial.printf("KB FN %s\n", pressed ? "down" : "up");
      continue;
    }

    // Caps-lock — toggle on key-down only (sticky).
    if (k == TPAGER_KB_CAPS_RAW) {
      if (pressed) {
        s_caps_lock = !s_caps_lock;
        Serial.printf("KB CAPS %s\n", s_caps_lock ? "on" : "off");
      }
      continue;
    }

    if (!pressed) continue;            // only act on key-down for the rest

    char c = '\0';
    if (k == TPAGER_KB_BACKSPACE_RAW) {
      c = '\b';   // backspace works in both layers
    } else {
      int row = (k - 1) / 10;
      int col = (k - 1) % 10;
      if (row >= 0 && row < 4 && col >= 0 && col < 10) {
        c = s_fn_held ? kb_keymap_symbol[row][col] : kb_keymap[row][col];
        // Caps-lock only affects letters in the base layer. FN-layer
        // symbols and Enter/Space/Backspace pass through unchanged.
        if (!s_fn_held && s_caps_lock && c >= 'a' && c <= 'z') {
          c = (char)(c - 'a' + 'A');
        }
      }
    }
    Serial.printf("KB raw=%u ch=%c%s%s\n", k, c ? c : '?',
                  s_fn_held ? " (FN)" : "",
                  s_caps_lock ? " (CAPS)" : "");

    if (c != '\0' && ui_input_char) ui_input_char(c);
  }
}

bool radio_init() {
  fallback_clock.begin();

  #ifdef LR11X0_DIO3_TCXO_VOLTAGE
    float tcxo = LR11X0_DIO3_TCXO_VOLTAGE;
  #else
    float tcxo = 1.8f;
  #endif

  spi.begin(P_LORA_SCLK, P_LORA_MISO, P_LORA_MOSI);

  int status = radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR,
                           RADIOLIB_LR11X0_LORA_SYNC_WORD_PRIVATE,
                           LORA_TX_POWER, 16, tcxo);
  if (status != RADIOLIB_ERR_NONE) {
    Serial.print("ERROR: LR1121 init failed: ");
    Serial.println(status);
    return false;
  }

  radio.setCRC(2);
  radio.explicitHeader();

  #ifdef RX_BOOSTED_GAIN
    radio.setRxBoostedGainMode(RX_BOOSTED_GAIN);
  #endif

  return true;
}

uint32_t radio_get_rng_seed() {
  return radio.random(0x7FFFFFFF);
}

void radio_set_params(float freq, float bw, uint8_t sf, uint8_t cr) {
  radio.setFrequency(freq);
  radio.setSpreadingFactor(sf);
  radio.setBandwidth(bw);
  radio.setCodingRate(cr);
}

void radio_set_tx_power(int8_t dbm) {
  radio.setOutputPower(dbm);
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);
}

// --- BQ27220 fuel gauge (minimal I2C read of State Of Charge register) ---
#ifndef BQ27220_I2C_ADDR
  #define BQ27220_I2C_ADDR 0x55
#endif
#define BQ27220_REG_SOC     0x2C  // State Of Charge, 2 bytes, 0..100 %
                                  // (0x1C is StandbyTimeToEmpty — common confusion)
#define BQ27220_REG_VOLTAGE 0x08  // Voltage, 2 bytes, mV

#define BQ25896_I2C_ADDR 0x6B
#define BQ25896_REG_09H  0x09     // BATFET_DIS bit 5 — set to shut down.

// Pull power rail down via the BQ25896 battery charger. Mirrors what
// XPowersLib::PowersBQ25896::shutdown() does: read REG09, set BATFET_DIS,
// write back. The chip kills the system rail immediately.
extern "C" void tpager_power_off() {
  Wire.beginTransmission(BQ25896_I2C_ADDR);
  Wire.write(BQ25896_REG_09H);
  if (Wire.endTransmission(true) != 0) return;
  Wire.requestFrom(BQ25896_I2C_ADDR, (uint8_t)1);
  if (!Wire.available()) return;
  uint8_t val = Wire.read();
  val |= (1 << 5);
  Wire.beginTransmission(BQ25896_I2C_ADDR);
  Wire.write(BQ25896_REG_09H);
  Wire.write(val);
  Wire.endTransmission();
}

static int read_bq27220_word(uint8_t reg) {
  Wire.beginTransmission(BQ27220_I2C_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(true) != 0) return -1;
  if (Wire.requestFrom(BQ27220_I2C_ADDR, (uint8_t)2) != 2) return -1;
  uint8_t lo = Wire.read();
  uint8_t hi = Wire.read();
  return (int)((hi << 8) | lo);
}

uint16_t TLoraPagerBoard::getBattMilliVolts() {
  int v = read_bq27220_word(BQ27220_REG_VOLTAGE);
  return v >= 0 ? (uint16_t)v : 0;
}

// C-linkage getter so UITask (a separate translation unit) can read the
// fuel-gauge SoC without pulling in the BQ27220 driver. Returns 0..100
// on success, or a negative debug marker on I²C failure.
extern "C" int tpager_battery_percent();

static int read_battery_percent() {
  // BQ27220 uses STOP-between-write-and-read (not repeated start) — pass
  // endTransmission(true), then requestFrom in a fresh transaction.
  Wire.beginTransmission(BQ27220_I2C_ADDR);
  Wire.write(BQ27220_REG_SOC);
  uint8_t err = Wire.endTransmission(true);
  if (err != 0) return -1000 - err;   // negative debug marker
  if (Wire.requestFrom(BQ27220_I2C_ADDR, (uint8_t)2) != 2) return -2000;
  uint8_t lo = Wire.read();
  uint8_t hi = Wire.read();
  return (int)((hi << 8) | lo);
}

// Battery % from resting LiPo voltage. The BQ27220 gauge's coulomb-counted SoC
// tops out around ~74% because its full-charge-capacity is programmed for a
// larger cell than is installed — but the *voltage* it reports is accurate, so
// we derive % from a standard single-cell LiPo discharge curve instead. Top of
// the curve is 4.15 V = 100 % (a fully-charged cell rests there once off the
// charger; ~4.2 V only occurs while actively charging).
static int lipo_percent_from_mv(int mv) {
  static const int kV[] = {3270,3610,3690,3710,3730,3750,3770,3790,3800,3820,
                           3840,3850,3870,3910,3950,3980,4020,4080,4110,4150};
  static const int kP[] = {   0,   5,  10,  15,  20,  25,  30,  35,  40,  45,
                             50,  55,  60,  65,  70,  75,  80,  85,  90, 100};
  const int n = (int)(sizeof(kV) / sizeof(kV[0]));
  if (mv <= kV[0])   return 0;
  if (mv >= kV[n-1]) return 100;
  for (int i = 1; i < n; i++) {
    if (mv < kV[i]) {
      int span = kV[i] - kV[i - 1];
      return kP[i - 1] + (kP[i] - kP[i - 1]) * (mv - kV[i - 1]) / span;
    }
  }
  return 100;
}

static int read_battery_percent_voltage() {
  int mv = read_bq27220_word(BQ27220_REG_VOLTAGE);
  if (mv < 0) return -1;
  int pct = lipo_percent_from_mv(mv);
  // Light EMA so transmit-time voltage sag doesn't make the reading jitter.
  static float ema = -1.0f;
  if (ema < 0.0f) ema = (float)pct;
  else            ema = ema * 0.8f + (float)pct * 0.2f;
  return (int)(ema + 0.5f);
}

extern "C" int tpager_battery_percent() {
  int p = read_battery_percent_voltage();
  if (p < 0) return -1;
  if (p > 100) return 100;
  return p;
}

#ifdef TPAGER_CHARGE_DEBUG
static int read_bq25896_reg(uint8_t reg) {
  Wire.beginTransmission(BQ25896_I2C_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(true) != 0) return -1;
  if (Wire.requestFrom(BQ25896_I2C_ADDR, (uint8_t)1) != 1) return -1;
  return (int)Wire.read();
}

// Dump charge state so we can tell whether the battery "stops at ~74%" because
// the BQ25896 terminates charge early (low VREG target / not-charging) or the
// BQ27220 fuel gauge is under-reading a cell that is actually full:
//   - state=done + vbat ~4.15-4.2 V  → cell full, gauge under-reads (calib)
//   - state=fast-charging, soc stuck → gauge issue while still charging
//   - state=not-charging + low vbat + low vreg → charger terminating early
// REG0B = system status (VBUS_STAT[7:5] / CHRG_STAT[4:3] / PG_STAT[2]).
// REG06 = charge-voltage limit: VREG = 3.840 V + VREG_field*16 mV (POR = 4.208 V).
extern "C" void tpager_charge_debug() {
  int reg0b = read_bq25896_reg(0x0B);
  int reg06 = read_bq25896_reg(0x06);
  int mv    = read_bq27220_word(BQ27220_REG_VOLTAGE);
  int soc   = read_battery_percent();
  if (reg0b < 0 || reg06 < 0) { Serial.println("CHG: BQ25896 read err"); return; }

  static const char* kChg[4] = { "not-charging", "pre-charge", "fast-charging", "done" };
  int chrg_stat = (reg0b >> 3) & 0x3;
  int pg        = (reg0b >> 2) & 0x1;
  int vreg_mv   = 3840 + (((reg06 >> 2) & 0x3F) * 16);

  // BQ27220 capacity standard-commands: RemainingCapacity 0x10,
  // FullChargeCapacity 0x12, DesignCapacity 0x3C (all mAh). If the cell is
  // full but soc<100, FCC/design set for a bigger cell is the smoking gun.
  int rem  = read_bq27220_word(0x10);
  int fcc  = read_bq27220_word(0x12);
  int dcap = read_bq27220_word(0x3C);

  Serial.printf("CHG: soc=%d%% vbat=%dmV state=%s pg=%d vreg=%dmV | rem=%dmAh fcc=%dmAh design=%dmAh (REG0B=0x%02X REG06=0x%02X)\n",
                soc, mv, kChg[chrg_stat], pg, vreg_mv, rem, fcc, dcap, reg0b, reg06);
}
#endif  // TPAGER_CHARGE_DEBUG

// --- Variant status hook (called from UITask::renderCurrScreen home-screen) ---
#include <helpers/ui/DisplayDriver.h>
extern WRAPPER_CLASS radio_driver;

// Weak bridge — defined by the active mesh-example's main.cpp (e.g.
// simple_repeater) which can see the_mesh and its RegionMap. Null when
// the example doesn't provide one (e.g. companion_radio), in which case
// the RGN line is skipped.
extern "C" const char* mesh_home_region_name() __attribute__((weak));

void render_extra_status_lines(DisplayDriver* d, int start_y) {
  char tmp[40];
  d->setColor(DisplayDriver::LIGHT);
  int y = start_y;

  // Region — read live from the active mesh's RegionMap home_id.
  if (mesh_home_region_name) {
    const char* rgn = mesh_home_region_name();
    d->setCursor(0, y);
    sprintf(tmp, "RGN: %s", rgn ? rgn : "(none)");
    d->print(tmp);
    y += 10;
  }

  // RX RSSI / SNR of the last received packet
  d->setCursor(0, y);
  sprintf(tmp, "RX: %d dBm / %d dB",
          (int)radio_driver.getLastRSSI(),
          (int)radio_driver.getLastSNR());
  d->print(tmp);
  y += 10;

  // Battery percentage (derived from BQ27220 voltage — see tpager_battery_percent)
  int bat = tpager_battery_percent();
  d->setCursor(0, y);
  if (bat >= 0 && bat <= 100) {
    sprintf(tmp, "BAT: %d %%", bat);
  } else if (bat > 100) {
    sprintf(tmp, "BAT: raw %d", bat);
  } else {
    sprintf(tmp, "BAT: err %d", bat);
  }
  d->print(tmp);
  y += 10;

  // GPS fix + coordinates (only when valid)
#if ENV_INCLUDE_GPS
  extern MicroNMEALocationProvider nmea;
  d->setCursor(0, y);
  if (nmea.isValid()) {
    float lat = nmea.getLatitude() / 1000000.0f;
    float lon = nmea.getLongitude() / 1000000.0f;
    sprintf(tmp, "GPS: %.4f, %.4f", lat, lon);
  } else {
    sprintf(tmp, "GPS: -- (%ld sat)", (long)nmea.satellitesCount());
  }
  d->print(tmp);
#endif
}
