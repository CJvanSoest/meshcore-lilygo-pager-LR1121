#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>
#include "MyMesh.h"

// Believe it or not, this std C function is busted on some platforms!
static uint32_t _atoi(const char* sp) {
  uint32_t n = 0;
  while (*sp && *sp >= '0' && *sp <= '9') {
    n *= 10;
    n += (*sp++ - '0');
  }
  return n;
}

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  #include <InternalFileSystem.h>
  #if defined(QSPIFLASH)
    #include <CustomLFS_QSPIFlash.h>
    DataStore store(InternalFS, QSPIFlash, rtc_clock);
  #else
  #if defined(EXTRAFS)
    #include <CustomLFS.h>
    CustomLFS ExtraFS(0xD4000, 0x19000, 128);
    DataStore store(InternalFS, ExtraFS, rtc_clock);
  #else
    DataStore store(InternalFS, rtc_clock);
  #endif
  #endif
#elif defined(RP2040_PLATFORM)
  #include <LittleFS.h>
  DataStore store(LittleFS, rtc_clock);
#elif defined(ESP32)
  #include <SPIFFS.h>
  DataStore store(SPIFFS, rtc_clock);
#endif

#ifdef ESP32
  #ifdef WIFI_SSID
    #include <helpers/esp32/SerialWifiInterface.h>
    SerialWifiInterface serial_interface;
    #ifndef TCP_PORT
      #define TCP_PORT 5000
    #endif
  #elif defined(BLE_PIN_CODE)
    #include <helpers/esp32/SerialBLEInterface.h>
    SerialBLEInterface serial_interface;
  #elif defined(SERIAL_RX)
    #include <helpers/ArduinoSerialInterface.h>
    ArduinoSerialInterface serial_interface;
    HardwareSerial companion_serial(1);
  #else
    #include <helpers/ArduinoSerialInterface.h>
    ArduinoSerialInterface serial_interface;
  #endif
#elif defined(RP2040_PLATFORM)
  //#ifdef WIFI_SSID
  //  #include <helpers/rp2040/SerialWifiInterface.h>
  //  SerialWifiInterface serial_interface;
  //  #ifndef TCP_PORT
  //    #define TCP_PORT 5000
  //  #endif
  // #elif defined(BLE_PIN_CODE)
  //   #include <helpers/rp2040/SerialBLEInterface.h>
  //   SerialBLEInterface serial_interface;
  #if defined(SERIAL_RX)
    #include <helpers/ArduinoSerialInterface.h>
    ArduinoSerialInterface serial_interface;
    HardwareSerial companion_serial(1);
  #else
    #include <helpers/ArduinoSerialInterface.h>
    ArduinoSerialInterface serial_interface;
  #endif
#elif defined(NRF52_PLATFORM)
  #ifdef BLE_PIN_CODE
    #include <helpers/nrf52/SerialBLEInterface.h>
    SerialBLEInterface serial_interface;
  #else
    #include <helpers/ArduinoSerialInterface.h>
    ArduinoSerialInterface serial_interface;
  #endif
#elif defined(STM32_PLATFORM)
  #include <helpers/ArduinoSerialInterface.h>
  ArduinoSerialInterface serial_interface;
#else
  #error "need to define a serial interface"
#endif

/* GLOBAL OBJECTS */
#ifdef DISPLAY_CLASS
  #include "UITask.h"
  UITask ui_task(&board, &serial_interface);
#endif

StdRNG fast_rng;
SimpleMeshTables tables;
MyMesh the_mesh(radio_driver, fast_rng, rtc_clock, tables, store
   #ifdef DISPLAY_CLASS
      , &ui_task
   #endif
);

/* END GLOBAL OBJECTS */

// Bridge: variant UIs (e.g. T-Pager LVGL settings list) call this after
// editing the NodePrefs in memory to push new radio parameters to the
// LR1121 and persist the change to SPIFFS.
extern "C" void ui_apply_radio_changes() {
  auto* p = the_mesh.getNodePrefs();
  radio_set_params(p->freq, p->bw, p->sf, p->cr);
  radio_set_tx_power(p->tx_power_dbm);
  the_mesh.savePrefs();
}

// Variant UIs read the duty-cycle USAGE as tenths-of-percent (0 = idle,
// 1000 = quota fully consumed). The leaky-bucket starts full at boot
// (= 0% used) and refills continuously, so a quiet hour will sink the
// reading back toward 0 even after a burst of TX.
extern "C" int ui_get_duty_cycle_used_tenths() {
  auto* p = the_mesh.getNodePrefs();
  float duty_cycle = 1.0f / (1.0f + p->airtime_factor);
  unsigned long max_budget = (unsigned long)(3600000UL * duty_cycle);
  if (max_budget == 0) return 0;
  unsigned long budget = the_mesh.getRemainingTxBudget();
  if (budget > max_budget) budget = max_budget;
  unsigned long used = max_budget - budget;
  return (int)((used * 1000UL) / max_budget);
}

// Same data exposed as "X seconds used out of Y seconds allowed per
// hour-window", which is what the Tanmatsu UI also surfaces — easier
// to interpret than a bare percentage when you're tracking your DC
// quota live during a chat.
extern "C" void ui_get_duty_cycle_seconds(int* used_s, int* max_s) {
  auto* p = the_mesh.getNodePrefs();
  float duty_cycle = 1.0f / (1.0f + p->airtime_factor);
  unsigned long max_budget = (unsigned long)(3600000UL * duty_cycle);
  unsigned long budget = the_mesh.getRemainingTxBudget();
  if (budget > max_budget) budget = max_budget;
  unsigned long used = max_budget - budget;
  if (used_s) *used_s = (int)(used / 1000UL);
  if (max_s)  *max_s  = (int)(max_budget / 1000UL);
}

// Channel bridges for the variant UI.
#include <helpers/ChannelDetails.h>

// 16-byte → 24-char base64 encoder. We can't include <base64.hpp>
// (already pulled in by BaseChatMesh.cpp and its functions aren't
// `inline`, so a second TU including it triggers multiple-definition
// link errors). This is a tiny self-contained variant that suffices
// for our 128-bit channel secrets.
static void b64_encode_16(const uint8_t* in, char* out) {
  static const char alph[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int oi = 0;
  for (int i = 0; i < 15; i += 3) {     // 5 full triplets
    uint32_t t = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8) | in[i+2];
    out[oi++] = alph[(t >> 18) & 0x3F];
    out[oi++] = alph[(t >> 12) & 0x3F];
    out[oi++] = alph[(t >>  6) & 0x3F];
    out[oi++] = alph[t & 0x3F];
  }
  uint32_t s = (uint32_t)in[15] << 16;  // trailing byte
  out[oi++] = alph[(s >> 18) & 0x3F];
  out[oi++] = alph[(s >> 12) & 0x3F];
  out[oi++] = '=';
  out[oi++] = '=';
  out[oi] = 0;
}

extern "C" int ui_get_channel_count() {
  ChannelDetails ch;
  int count = 0;
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
    if (the_mesh.getChannel(i, ch) && ch.name[0]) count++;
  }
  return count;
}

// Returns true if a channel with that index exists; fills name buf.
// `idx` indexes only the populated channels (i.e., skips empty slots).
extern "C" bool ui_get_channel_name(int idx, char* buf, int buf_size) {
  ChannelDetails ch;
  int seen = 0;
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
    if (the_mesh.getChannel(i, ch) && ch.name[0]) {
      if (seen == idx) {
        size_t n = strlen(ch.name);
        if ((int)n >= buf_size) n = buf_size - 1;
        memcpy(buf, ch.name, n);
        buf[n] = 0;
        return true;
      }
      seen++;
    }
  }
  return false;
}

// Add a hashtag-style channel. Secret is derived as
// SHA256("#"+name)[0..15] (the MeshCore convention for hashtag chans;
// see DEVLOG entry on channel auto-derivation). Returns false if the
// channel table is full or the name is empty.
//
// We bypass BaseChatMesh::addChannel and use setChannel into a manually-
// located free slot. Reason: addChannel uses `num_channels` as the next
// free slot index, but `num_channels` is only incremented by addChannel
// itself — loadChannels (via setChannel) does NOT bump it. So after a
// reboot `num_channels` stays at 1 ("Public" from first-boot init) and
// any subsequent addChannel call overwrites slot 1 regardless of which
// channels are already persisted there. Walking the table for an empty
// name slot is the deterministic fix.
extern "C" bool ui_add_hashtag_channel(const char* name) {
  if (!name || !name[0]) return false;
  char hashtagged[40];
  snprintf(hashtagged, sizeof(hashtagged), "#%s", name);
  uint8_t secret[16];
  mesh::Utils::sha256(secret, 16,
                      (const uint8_t*)hashtagged, strlen(hashtagged));
  if (!the_mesh.uiAddHashtagChannel(name, secret)) return false;
  the_mesh.uiSaveChannels();
  return true;
}

// Send a chat message to a channel by index. Returns false if the
// channel is empty or sendGroupMessage rejects the payload. The
// caller is responsible for the channel index — variant UIs find it
// via ui_get_channel_name iteration.
extern "C" bool ui_send_group_text(int channel_idx, const char* text) {
  if (!text || !text[0]) return false;
  ChannelDetails ch;
  if (!the_mesh.getChannel(channel_idx, ch) || !ch.name[0]) return false;
  uint32_t now = the_mesh.getRTCClock()->getCurrentTime();
  return the_mesh.sendGroupMessage(now, ch.channel,
                                   the_mesh.getNodeName(),
                                   text, strlen(text));
}

// Apply a new default-scope name from the variant UI. The HMAC key
// is derived from "#"+name via TransportKeyStore::getAutoKeyFor — same
// pattern MyMesh uses at first-boot from DEFAULT_FLOOD_SCOPE_NAME.
// Empty name zeroes the key (= wildcard / no scope).
#include <helpers/TransportKeyStore.h>
extern "C" void ui_apply_default_scope(const char* name) {
  auto* p = the_mesh.getNodePrefs();
  if (!name) name = "";
  size_t n = strlen(name);
  if (n >= sizeof(p->default_scope_name)) n = sizeof(p->default_scope_name) - 1;
  memcpy(p->default_scope_name, name, n);
  p->default_scope_name[n] = 0;
  if (n == 0) {
    memset(p->default_scope_key, 0, sizeof(p->default_scope_key));
  } else {
    TransportKeyStore temp;
    TransportKey key;
    char hashtagged[40];
    snprintf(hashtagged, sizeof(hashtagged), "#%s", p->default_scope_name);
    temp.getAutoKeyFor(0, hashtagged, key);
    memcpy(p->default_scope_key, key.key, sizeof(p->default_scope_key));
  }
  the_mesh.savePrefs();
}

void halt() {
  while (1) ;
}

void setup() {
  Serial.begin(115200);

  board.begin();

#ifdef DISPLAY_CLASS
  DisplayDriver* disp = NULL;
  if (display.begin()) {
    disp = &display;
    disp->startFrame();
  #ifdef ST7789
    disp->setTextSize(2);
  #endif
    disp->drawTextCentered(disp->width() / 2, 28, "Loading...");
    disp->endFrame();
  }
#endif

  if (!radio_init()) { halt(); }

  fast_rng.begin(radio_get_rng_seed());

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS.begin();
  #if defined(QSPIFLASH)
    if (!QSPIFlash.begin()) {
      // debug output might not be available at this point, might be too early. maybe should fall back to InternalFS here?
      MESH_DEBUG_PRINTLN("CustomLFS_QSPIFlash: failed to initialize");
    } else {
      MESH_DEBUG_PRINTLN("CustomLFS_QSPIFlash: initialized successfully");
    }
  #else
  #if defined(EXTRAFS)
      ExtraFS.begin();
  #endif
  #endif
  store.begin();
  the_mesh.begin(
    #ifdef DISPLAY_CLASS
        disp != NULL
    #else
        false
    #endif
  );

#ifdef BLE_PIN_CODE
  serial_interface.begin(BLE_NAME_PREFIX, the_mesh.getNodePrefs()->node_name, the_mesh.getBLEPin());
#else
  serial_interface.begin(Serial);
#endif
  the_mesh.startInterface(serial_interface);
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  store.begin();
  the_mesh.begin(
    #ifdef DISPLAY_CLASS
        disp != NULL
    #else
        false
    #endif
  );

  //#ifdef WIFI_SSID
  //  WiFi.begin(WIFI_SSID, WIFI_PWD);
  //  serial_interface.begin(TCP_PORT);
  // #elif defined(BLE_PIN_CODE)
  //   char dev_name[32+16];
  //   sprintf(dev_name, "%s%s", BLE_NAME_PREFIX, the_mesh.getNodeName());
  //   serial_interface.begin(dev_name, the_mesh.getBLEPin());
  #if defined(SERIAL_RX)
    companion_serial.setPins(SERIAL_RX, SERIAL_TX);
    companion_serial.begin(115200);
    serial_interface.begin(companion_serial);
  #else
    serial_interface.begin(Serial);
  #endif
    the_mesh.startInterface(serial_interface);
#elif defined(ESP32)
  SPIFFS.begin(true);
  store.begin();
  the_mesh.begin(
    #ifdef DISPLAY_CLASS
        disp != NULL
    #else
        false
    #endif
  );

#ifdef WIFI_SSID
  board.setInhibitSleep(true);   // prevent sleep when WiFi is active
  WiFi.begin(WIFI_SSID, WIFI_PWD);
  serial_interface.begin(TCP_PORT);
#elif defined(BLE_PIN_CODE)
  serial_interface.begin(BLE_NAME_PREFIX, the_mesh.getNodePrefs()->node_name, the_mesh.getBLEPin());
#elif defined(SERIAL_RX)
  companion_serial.setPins(SERIAL_RX, SERIAL_TX);
  companion_serial.begin(115200);
  serial_interface.begin(companion_serial);
#else
  serial_interface.begin(Serial);
#endif
  the_mesh.startInterface(serial_interface);
#else
  #error "need to define filesystem"
#endif

  sensors.begin();

#if ENV_INCLUDE_GPS == 1
  the_mesh.applyGpsPrefs();
#endif

#ifdef DISPLAY_CLASS
  ui_task.begin(disp, &sensors, the_mesh.getNodePrefs());  // still want to pass this in as dependency, as prefs might be moved
#endif
}

void loop() {
  the_mesh.loop();
  sensors.loop();
#ifdef DISPLAY_CLASS
  ui_task.loop();
#endif
  rtc_clock.tick();
}
