#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>

#include "MyMesh.h"

#ifdef DISPLAY_CLASS
  #include "UITask.h"
  static UITask ui_task(display);
#endif

StdRNG fast_rng;
SimpleMeshTables tables;

MyMesh the_mesh(board, radio_driver, *new ArduinoMillis(), fast_rng, rtc_clock, tables);

// Bridge so variant UI code can show the configured region without
// depending on MyMesh internals. The symbol is picked up by variants
// via a __attribute__((weak)) declaration in their target.cpp.
extern "C" const char* mesh_home_region_name() {
  return the_mesh.getHomeRegionName();
}

// Bridge invoked by the UITask compose screen when the user presses Enter.
// Sends a GROUP_TXT packet to the hard-coded #test channel via plain
// sendFlood (no region scope yet — full region-scoped channel send is
// follow-up work; for now this is enough to make the message visible to
// any node tuned to the same channel).
extern "C" void ui_send_message(const char* msg) {
  if (!msg || !msg[0]) return;

#ifndef COMPOSE_CHANNEL
  #define COMPOSE_CHANNEL "#test"
#endif
  const char* CHANNEL_NAME = COMPOSE_CHANNEL;
  mesh::GroupChannel channel;
  uint8_t full_secret[32];
  mesh::Utils::sha256(full_secret, 16, (const uint8_t*)CHANNEL_NAME, strlen(CHANNEL_NAME));
  memset(channel.secret, 0, sizeof(channel.secret));
  memcpy(channel.secret, full_secret, 16);
  mesh::Utils::sha256(channel.hash, sizeof(channel.hash), channel.secret, 16);

  // Payload: 4-byte timestamp + TXT_TYPE_PLAIN (0) + "<sender>: <text>"
  uint32_t ts = the_mesh.getRTCClock()->getCurrentTime();
  const char* name = the_mesh.getNodePrefs()->node_name;

  // Payload buffer: 4 bytes timestamp + 1 byte TXT_TYPE_PLAIN + sender+msg
  static const int PAYLOAD_BUF_LEN = 196;
  uint8_t buf[PAYLOAD_BUF_LEN];
  memcpy(buf, &ts, 4);
  buf[4] = 0;  // TXT_TYPE_PLAIN
  int prefix = snprintf((char*)&buf[5], PAYLOAD_BUF_LEN - 5, "%s: ", name);
  if (prefix < 0) prefix = 0;
  int max_text = PAYLOAD_BUF_LEN - 5 - prefix - 1;
  int text_len = (int)strlen(msg);
  if (text_len > max_text) text_len = max_text;
  memcpy(&buf[5 + prefix], msg, text_len);
  int total = 5 + prefix + text_len;

  auto pkt = the_mesh.createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, channel, buf, total);
  if (pkt) {
    uint8_t hash_size = the_mesh.getNodePrefs()->path_hash_mode + 1;
    TransportKey scope;
    if (the_mesh.getHomeScope(scope)) {
      the_mesh.sendFloodScoped(scope, pkt, 0, hash_size);
      Serial.print("[SEND #test scoped] ");
    } else {
      the_mesh.sendFlood(pkt, 0, hash_size);
      Serial.print("[SEND #test unscoped] ");
    }
    Serial.println(msg);
  } else {
    Serial.println("[SEND #test] packet alloc failed");
  }
}

// Bridge for variants with their own input device (e.g. T-Pager keyboard).
// They call this on every key-down; we forward to UITask when compose is
// the active screen.
extern "C" void ui_input_char(char c) {
#if defined(DISPLAY_CLASS) && (COMPOSE_BUFFER_SIZE > 0)
  if (ui_task.isComposeActive()) ui_task.addInputChar(c);
#else
  (void)c;
#endif
}

void halt() {
  while (1) ;
}

static char command[160];

// For power saving
unsigned long POWERSAVING_FIRSTSLEEP_SECS = 120; // The first sleep (if enabled) from boot

#if defined(PIN_USER_BTN) && defined(_SEEED_SENSECAP_SOLAR_H_)
static unsigned long userBtnDownAt = 0;
#define USER_BTN_HOLD_OFF_MILLIS 1500
#endif

void setup() {
  Serial.begin(115200);
  delay(1000);

  board.begin();

#if defined(MESH_DEBUG) && defined(NRF52_PLATFORM)
  // give some extra time for serial to settle so
  // boot debug messages can be seen on terminal
  delay(5000);
#endif

#ifdef DISPLAY_CLASS
  if (display.begin()) {
    display.startFrame();
    display.setCursor(0, 0);
    display.print("Please wait...");
    display.endFrame();
  }
#endif

  if (!radio_init()) {
    MESH_DEBUG_PRINTLN("Radio init failed!");
    halt();
  }

  fast_rng.begin(radio_driver.getRngSeed());

  FILESYSTEM* fs;
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS.begin();
  fs = &InternalFS;
  IdentityStore store(InternalFS, "");
#elif defined(ESP32)
  SPIFFS.begin(true);
  fs = &SPIFFS;
  IdentityStore store(SPIFFS, "/identity");
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  fs = &LittleFS;
  IdentityStore store(LittleFS, "/identity");
  store.begin();
#else
  #error "need to define filesystem"
#endif
  if (!store.load("_main", the_mesh.self_id)) {
    MESH_DEBUG_PRINTLN("Generating new keypair");
    the_mesh.self_id = radio_new_identity();   // create new random identity
    int count = 0;
    while (count < 10 && (the_mesh.self_id.pub_key[0] == 0x00 || the_mesh.self_id.pub_key[0] == 0xFF)) {  // reserved id hashes
      the_mesh.self_id = radio_new_identity(); count++;
    }
    store.save("_main", the_mesh.self_id);
  }

  Serial.print("Repeater ID: ");
  mesh::Utils::printHex(Serial, the_mesh.self_id.pub_key, PUB_KEY_SIZE); Serial.println();

  command[0] = 0;

  sensors.begin();

  the_mesh.begin(fs);

#ifdef DISPLAY_CLASS
  ui_task.begin(the_mesh.getNodePrefs(), FIRMWARE_BUILD_DATE, FIRMWARE_VERSION);
#endif

  // send out initial zero hop Advertisement to the mesh
#if ENABLE_ADVERT_ON_BOOT == 1
  the_mesh.sendSelfAdvertisement(16000, false);
#endif

  board.onBootComplete();
}

void loop() {
  int len = strlen(command);
  while (Serial.available() && len < sizeof(command)-1) {
    char c = Serial.read();
    if (c != '\n') {
      command[len++] = c;
      command[len] = 0;
      Serial.print(c);
    }
    if (c == '\r') break;
  }
  if (len == sizeof(command)-1) {  // command buffer full
    command[sizeof(command)-1] = '\r';
  }

  if (len > 0 && command[len - 1] == '\r') {  // received complete line
    Serial.print('\n');
    command[len - 1] = 0;  // replace newline with C string null terminator
    char reply[160];
    the_mesh.handleCommand(0, command, reply);  // NOTE: there is no sender_timestamp via serial!
    if (reply[0]) {
      Serial.print("  -> "); Serial.println(reply);
    }

    command[0] = 0;  // reset command buffer
  }

#if defined(PIN_USER_BTN) && defined(_SEEED_SENSECAP_SOLAR_H_)
  // Hold the user button to power off the SenseCAP Solar repeater.
  int btnState = digitalRead(PIN_USER_BTN);
  if (btnState == LOW) {
    if (userBtnDownAt == 0) {
      userBtnDownAt = millis();
    } else if ((unsigned long)(millis() - userBtnDownAt) >= USER_BTN_HOLD_OFF_MILLIS) {
      Serial.println("Powering off...");
      board.powerOff();  // does not return
    }
  } else {
    userBtnDownAt = 0;
  }
#endif

  the_mesh.loop();
  sensors.loop();
#ifdef DISPLAY_CLASS
  ui_task.loop();
#endif
  rtc_clock.tick();

  // Variant-supplied periodic work (e.g. polling a keyboard FIFO). Default
  // is a no-op via the weak symbol below.
  extern void variant_loop() __attribute__((weak));
  if (variant_loop) variant_loop();

  if (the_mesh.getNodePrefs()->powersaving_enabled && !the_mesh.hasPendingWork()) {
#if defined(NRF52_PLATFORM)
    board.sleep(0); // nrf ignores seconds param, sleeps whenever possible
#else
    if (the_mesh.millisHasNowPassed(POWERSAVING_FIRSTSLEEP_SECS * 1000)) { // To check if it is time to sleep
      board.sleep(30); // Sleep. Wake up after a while or when receiving a LoRa packet
    }
#endif
  }
}
