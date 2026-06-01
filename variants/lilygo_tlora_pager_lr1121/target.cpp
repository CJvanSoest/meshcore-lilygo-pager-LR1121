#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_TCA8418.h>
#include "target.h"

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

ESP32RTCClock fallback_clock;
AutoDiscoverRTCClock rtc_clock(fallback_clock);

#if ENV_INCLUDE_GPS
  #include <helpers/sensors/MicroNMEALocationProvider.h>
  MicroNMEALocationProvider nmea = MicroNMEALocationProvider(Serial1, &rtc_clock);
  EnvironmentSensorManager sensors = EnvironmentSensorManager(nmea);
#else
  EnvironmentSensorManager sensors;
#endif

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
}

// --- TCA8418 keyboard ---------------------------------------------------------
static Adafruit_TCA8418 _kb;
static bool _kb_ok = false;

// QWERTY keymap copied from LilyGo_LoRa_Pager.cpp (4 rows x 10 cols).
// '\0' marks a position with no key (or a modifier handled elsewhere).
static const char kb_keymap[4][10] = {
  {'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p'},
  {'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', '\n'},
  {'\0','z', 'x', 'c', 'v', 'b', 'n', 'm', '\0','\0'},
  {' ', '\0','\0','\0','\0','\0','\0','\0','\0','\0'}
};

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

// Backspace key raw code per LilyGo T-Pager config (no ASCII letter mapped).
#ifndef TPAGER_KB_BACKSPACE_RAW
  #define TPAGER_KB_BACKSPACE_RAW 0x1D
#endif

// Called from main loop() via the variant_loop weak hook. Drains pending
// key events from the TCA8418 FIFO; key-down events are forwarded to
// main.cpp's ui_input_char() (weak — null in non-UI builds) which routes
// them to UITask when the compose screen is active.
extern "C" void ui_input_char(char c) __attribute__((weak));

void variant_loop() {
  if (!_kb_ok) { kb_begin(); return; }   // try late init if I2C wasn't ready

  while (_kb.available()) {
    uint8_t ev = _kb.getEvent();
    bool pressed = (ev & 0x80) != 0;
    uint8_t k = ev & 0x7F;
    if (!pressed) continue;            // only act on key-down

    char c = '\0';
    if (k == TPAGER_KB_BACKSPACE_RAW) {
      c = '\b';
    } else {
      int row = (k - 1) / 10;
      int col = (k - 1) % 10;
      if (row >= 0 && row < 4 && col >= 0 && col < 10) c = kb_keymap[row][col];
    }
    Serial.printf("KB raw=%u ch=%c\n", k, c ? c : '?');

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

  // Battery percentage (BQ27220 fuel gauge)
  int bat = read_battery_percent();
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
