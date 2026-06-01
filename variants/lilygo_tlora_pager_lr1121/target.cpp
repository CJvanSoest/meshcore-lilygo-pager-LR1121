#include <Arduino.h>
#include <Wire.h>
#include "target.h"

TLoraPagerBoard board;

#ifdef DISPLAY_CLASS
  DISPLAY_CLASS display;
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
#define BQ27220_REG_SOC 0x2C  // State Of Charge, 2 bytes, 0..100 %
                              // (0x1C is StandbyTimeToEmpty — common confusion)

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

void render_extra_status_lines(DisplayDriver* d, int start_y) {
  char tmp[40];
  d->setColor(DisplayDriver::LIGHT);

  // RX RSSI / SNR of the last received packet
  d->setCursor(0, start_y);
  sprintf(tmp, "RX: %d dBm / %d dB",
          (int)radio_driver.getLastRSSI(),
          (int)radio_driver.getLastSNR());
  d->print(tmp);

  // Battery percentage (BQ27220 fuel gauge)
  int bat = read_battery_percent();
  d->setCursor(0, start_y + 10);
  if (bat >= 0 && bat <= 100) {
    sprintf(tmp, "BAT: %d %%", bat);
  } else if (bat > 100) {
    sprintf(tmp, "BAT: raw %d", bat);  // device responded but value out of 0..100
  } else {
    sprintf(tmp, "BAT: err %d", bat);  // I2C-level error
  }
  d->print(tmp);

  // GPS fix + coordinates (only when valid)
#if ENV_INCLUDE_GPS
  extern MicroNMEALocationProvider nmea;
  d->setCursor(0, start_y + 20);
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
