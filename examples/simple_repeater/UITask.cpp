#include "UITask.h"
#include <Arduino.h>
#include <helpers/CommonCLI.h>

#ifndef USER_BTN_PRESSED
#define USER_BTN_PRESSED LOW
#endif

#define AUTO_OFF_MILLIS      20000  // 20 seconds
#define BOOT_SCREEN_MILLIS   4000   // 4 seconds

// 'meshcore', 128x13px
static const uint8_t meshcore_logo [] PROGMEM = {
    0x3c, 0x01, 0xe3, 0xff, 0xc7, 0xff, 0x8f, 0x03, 0x87, 0xfe, 0x1f, 0xfe, 0x1f, 0xfe, 0x1f, 0xfe, 
    0x3c, 0x03, 0xe3, 0xff, 0xc7, 0xff, 0x8e, 0x03, 0x8f, 0xfe, 0x3f, 0xfe, 0x1f, 0xff, 0x1f, 0xfe, 
    0x3e, 0x03, 0xc3, 0xff, 0x8f, 0xff, 0x0e, 0x07, 0x8f, 0xfe, 0x7f, 0xfe, 0x1f, 0xff, 0x1f, 0xfc, 
    0x3e, 0x07, 0xc7, 0x80, 0x0e, 0x00, 0x0e, 0x07, 0x9e, 0x00, 0x78, 0x0e, 0x3c, 0x0f, 0x1c, 0x00, 
    0x3e, 0x0f, 0xc7, 0x80, 0x1e, 0x00, 0x0e, 0x07, 0x1e, 0x00, 0x70, 0x0e, 0x38, 0x0f, 0x3c, 0x00, 
    0x7f, 0x0f, 0xc7, 0xfe, 0x1f, 0xfc, 0x1f, 0xff, 0x1c, 0x00, 0x70, 0x0e, 0x38, 0x0e, 0x3f, 0xf8, 
    0x7f, 0x1f, 0xc7, 0xfe, 0x0f, 0xff, 0x1f, 0xff, 0x1c, 0x00, 0xf0, 0x0e, 0x38, 0x0e, 0x3f, 0xf8, 
    0x7f, 0x3f, 0xc7, 0xfe, 0x0f, 0xff, 0x1f, 0xff, 0x1c, 0x00, 0xf0, 0x1e, 0x3f, 0xfe, 0x3f, 0xf0, 
    0x77, 0x3b, 0x87, 0x00, 0x00, 0x07, 0x1c, 0x0f, 0x3c, 0x00, 0xe0, 0x1c, 0x7f, 0xfc, 0x38, 0x00, 
    0x77, 0xfb, 0x8f, 0x00, 0x00, 0x07, 0x1c, 0x0f, 0x3c, 0x00, 0xe0, 0x1c, 0x7f, 0xf8, 0x38, 0x00, 
    0x73, 0xf3, 0x8f, 0xff, 0x0f, 0xff, 0x1c, 0x0e, 0x3f, 0xf8, 0xff, 0xfc, 0x70, 0x78, 0x7f, 0xf8, 
    0xe3, 0xe3, 0x8f, 0xff, 0x1f, 0xfe, 0x3c, 0x0e, 0x3f, 0xf8, 0xff, 0xfc, 0x70, 0x3c, 0x7f, 0xf8, 
    0xe3, 0xe3, 0x8f, 0xff, 0x1f, 0xfc, 0x3c, 0x0e, 0x1f, 0xf8, 0xff, 0xf8, 0x70, 0x3c, 0x7f, 0xf8, 
};

void UITask::begin(NodePrefs* node_prefs, const char* build_date, const char* firmware_version) {
  _prevBtnState = HIGH;
  _auto_off = millis() + AUTO_OFF_MILLIS;
  _node_prefs = node_prefs;
  _display->turnOn();

  // strip off dash and commit hash by changing dash to null terminator
  // e.g: v1.2.3-abcdef -> v1.2.3
  char *version = strdup(firmware_version);
  char *dash = strchr(version, '-');
  if(dash){
    *dash = 0;
  }

  // v1.2.3 (1 Jan 2025)
  sprintf(_version_info, "%s (%s)", version, build_date);
}

// Forward declaration of the optional 'About' screen renderer used when
// _current_screen == 1. Lives further down in this file so renderCurrScreen
// stays readable.
static void renderAboutScreen(DisplayDriver* d, const char* version_info);

void UITask::renderCurrScreen() {
  char tmp[80];

  // Non-home screens skip the boot/home logic entirely.
  if (_current_screen == 1) {
    renderAboutScreen(_display, _version_info);
    return;
  }

  if (millis() < BOOT_SCREEN_MILLIS) { // boot screen
    // meshcore logo
    _display->setColor(DisplayDriver::BLUE);
    int logoWidth = 128;
    _display->drawXbm((_display->width() - logoWidth) / 2, 3, meshcore_logo, logoWidth, 13);

    // version info
    _display->setColor(DisplayDriver::LIGHT);
    _display->setTextSize(1);
    uint16_t versionWidth = _display->getTextWidth(_version_info);
    _display->setCursor((_display->width() - versionWidth) / 2, 22);
    _display->print(_version_info);

    // node type
    const char* node_type = "< Repeater >";
    uint16_t typeWidth = _display->getTextWidth(node_type);
    _display->setCursor((_display->width() - typeWidth) / 2, 35);
    _display->print(node_type);
  } else {  // home screen
    // node name
    _display->setCursor(0, 0);
    _display->setTextSize(1);
    _display->setColor(DisplayDriver::GREEN);
    _display->print(_node_prefs->node_name);

    // freq / sf
    _display->setCursor(0, 20);
    _display->setColor(DisplayDriver::YELLOW);
    sprintf(tmp, "FREQ: %06.3f SF%d", _node_prefs->freq, _node_prefs->sf);
    _display->print(tmp);

    // bw / cr
    _display->setCursor(0, 30);
    sprintf(tmp, "BW: %03.2f CR: %d", _node_prefs->bw, _node_prefs->cr);
    _display->print(tmp);

    // uptime (only render on taller displays — fits below the bw/cr line)
    if (_display->height() >= 50) {
      unsigned long up_s = millis() / 1000;
      _display->setCursor(0, 40);
      _display->setColor(DisplayDriver::LIGHT);
      sprintf(tmp, "UP: %02lu:%02lu:%02lu",
              up_s / 3600, (up_s / 60) % 60, up_s % 60);
      _display->print(tmp);
    }

    // path hash size (mode 0/1/2 maps to 1/2/3 bytes)
    if (_display->height() >= 60) {
      _display->setCursor(0, 50);
      _display->setColor(DisplayDriver::LIGHT);
      int bytes = _node_prefs->path_hash_mode + 1;
      sprintf(tmp, "PATH: %d byte%s", bytes, bytes == 1 ? "" : "s");
      _display->print(tmp);
    }

    // Variant-defined extra status lines (weak symbol — default no-op).
    // The variant's target.cpp may override this to draw RGN, RSSI/SNR,
    // battery, GPS, etc. without touching shared UITask code.
    extern void render_extra_status_lines(DisplayDriver* d, int start_y)
      __attribute__((weak));
    if (render_extra_status_lines && _display->height() >= 70) {
      render_extra_status_lines(_display, 60);
    }
  }
}

void UITask::loop() {
#if defined(PIN_ENCODER_A) && defined(PIN_ENCODER_B)
  // Rotary encoder — falling edge on A signals one detent. Direction is
  // taken from B's state at that moment: B HIGH = CW, B LOW = CCW.
  // Polled here at loop rate; for fast spinning we may miss steps, but
  // for menu navigation that is fine.
  static int last_enc_a = HIGH;
  int enc_a = digitalRead(PIN_ENCODER_A);
  if (last_enc_a == HIGH && enc_a == LOW) {
    int enc_b = digitalRead(PIN_ENCODER_B);
    if (enc_b == HIGH) {
      _current_screen = (_current_screen + 1) % NUM_SCREENS;
    } else {
      _current_screen = (_current_screen + NUM_SCREENS - 1) % NUM_SCREENS;
    }
    _next_refresh = 0;
    _auto_off = millis() + AUTO_OFF_MILLIS;
    if (!_display->isOn()) _display->turnOn();
  }
  last_enc_a = enc_a;
#endif

#ifdef PIN_USER_BTN
  if (millis() >= _next_read) {
    int btnState = digitalRead(PIN_USER_BTN);
    if (btnState != _prevBtnState) {
      if (btnState == USER_BTN_PRESSED) {  // pressed?
        if (_display->isOn()) {
          // Cycle to the next screen (boards with NUM_SCREENS > 1).
          _current_screen = (_current_screen + 1) % NUM_SCREENS;
          _next_refresh = 0;  // force immediate redraw
        } else {
          _display->turnOn();
        }
        _auto_off = millis() + AUTO_OFF_MILLIS;   // extend auto-off timer
      }
      _prevBtnState = btnState;
    }
    _next_read = millis() + 200;  // 5 reads per second
  }
#endif

  if (_display->isOn()) {
    if (millis() >= _next_refresh) {
      _display->startFrame();
      renderCurrScreen();
      _display->endFrame();

      _next_refresh = millis() + 1000;   // refresh every second
    }
    if (millis() > _auto_off) {
      _display->turnOff();
    }
  }
}

// --- Optional 'About' screen --------------------------------------------------
//
// Renders firmware version, MAC address and free heap. Only used when a
// variant defines NUM_SCREENS > 1 and the user has cycled past the home
// screen via the PIN_USER_BTN.

#ifdef ESP_PLATFORM
#include <esp_mac.h>
#endif

static void renderAboutScreen(DisplayDriver* d, const char* version_info) {
  char tmp[64];
  d->setColor(DisplayDriver::LIGHT);
  d->setTextSize(1);

  d->setCursor(0, 0);
  d->print("About");

  d->setCursor(0, 12);
  d->print(version_info);

#ifdef ESP_PLATFORM
  uint8_t mac[6] = {0};
  esp_efuse_mac_get_default(mac);
  d->setCursor(0, 24);
  sprintf(tmp, "MAC: %02X:%02X:%02X:%02X:%02X:%02X",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  d->print(tmp);

  d->setCursor(0, 36);
  sprintf(tmp, "HEAP: %u KB", (unsigned)(ESP.getFreeHeap() / 1024));
  d->print(tmp);

  d->setCursor(0, 48);
  sprintf(tmp, "FLASH: %u MB", (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));
  d->print(tmp);
#endif
}
