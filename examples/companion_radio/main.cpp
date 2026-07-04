#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>
#include "MyMesh.h"
#include <esp_heap_caps.h>  // heap_caps_calloc / MALLOC_CAP_SPIRAM

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
// LR1121 and persist the change to SPIFFS. RX-boost included: the LR1121
// LNA-boost register is NOT touched by radio_set_params (that call only
// re-runs setFrequency/SF/BW/CR), so without the explicit
// setRxBoostedGainMode() call below a UI-side toggle would only take
// effect on the next reboot.
extern "C" void ui_apply_radio_changes() {
  auto* p = the_mesh.getNodePrefs();
  radio_set_params(p->freq, p->bw, p->sf, p->cr);
  radio_set_tx_power(p->tx_power_dbm);
  radio_driver.setRxBoostedGainMode(p->rx_boosted_gain);
  the_mesh.savePrefs();
}

// Variant UIs read the duty-cycle USAGE as tenths-of-percent (0 = idle,
// 1000 = quota fully consumed). Reports CUMULATIVE TX airtime since boot
// against the per-hour DC quota: monotonically rising so each advert /
// message TX visibly moves the counter. Previously this returned the
// leaky-bucket "remaining" derivative, which refilled back to 0 within
// seconds of a TX — confusing on a UI that refreshes at 1 Hz.
extern "C" int ui_get_duty_cycle_used_tenths() {
  auto* p = the_mesh.getNodePrefs();
  float duty_cycle = 1.0f / (1.0f + p->airtime_factor);
  unsigned long max_budget = (unsigned long)(3600000UL * duty_cycle);
  if (max_budget == 0) return 0;
  unsigned long used = the_mesh.getTotalAirTime();
  if (used > max_budget) used = max_budget;        // cap at 100%
  return (int)((used * 1000UL) / max_budget);
}

// Same data exposed as airtime numbers. Returns USED in milliseconds
// (1 ms resolution so even a single 80-200 ms advert shifts the
// display) and MAX in seconds (matches the per-hour quota in human
// units). UI formats `X.Xs / Ys`.
extern "C" void ui_get_duty_cycle_seconds(int* used_ms, int* max_s) {
  auto* p = the_mesh.getNodePrefs();
  float duty_cycle = 1.0f / (1.0f + p->airtime_factor);
  unsigned long max_budget = (unsigned long)(3600000UL * duty_cycle);
  unsigned long used = the_mesh.getTotalAirTime();
  if (used_ms) *used_ms = (int)used;
  if (max_s)   *max_s   = (int)(max_budget / 1000UL);
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

extern "C" bool ui_delete_channel(int populated_idx) {
  if (!the_mesh.uiDeleteChannel(populated_idx)) return false;
  the_mesh.uiSaveChannels();
  return true;
}

// Send a self-advert from the UI (Discovered tile "+ Send advert" row).
// `flood=true` reaches the mesh beyond direct neighbours; false stays
// local. Matches the companion-app CMD_SEND_SELF_ADVERT flow.
extern "C" bool ui_send_self_advert(bool flood) {
  return the_mesh.uiSendSelfAdvert(flood);
}

// Set the pager's node_name (Settings tile rename field) and persist.
extern "C" void ui_set_node_name(const char* name) {
  the_mesh.uiSetNodeName(name);
}

extern "C" const char* ui_get_node_name() {
  return the_mesh.getNodePrefs()->node_name;
}

// ----------- Contacts (S3.5) -------------------------------------------------
//
// Per-contact SNR / RSSI cache. Keyed by an 8-byte prefix of the
// contact's pub_key (PUB_KEY_SIZE=32 in MeshCore; first 8 bytes are
// already used as the contact hash so collisions are vanishingly rare
// among the few dozen nodes a single device hears at once). Fixed-size
// + LRU eviction so we don't blow the DRAM budget — a per-contact
// parallel array sized to MAX_CONTACTS (=350) overflowed the segment.

#include <helpers/ContactInfo.h>

struct UiSigEntry {
  uint8_t  prefix[8];     // first 8 bytes of pub_key
  int8_t   snr_q;
  int16_t  rssi;
  uint32_t last_ms;       // millis() at last update — used for LRU eviction
};
static const int UI_SIG_CACHE = 64;
static UiSigEntry s_sig_cache[UI_SIG_CACHE];

static UiSigEntry* sig_find(const uint8_t* pub_key) {
  for (int i = 0; i < UI_SIG_CACHE; i++) {
    if (s_sig_cache[i].last_ms == 0) continue;
    if (memcmp(s_sig_cache[i].prefix, pub_key, 8) == 0) return &s_sig_cache[i];
  }
  return nullptr;
}

static UiSigEntry* sig_allocate(const uint8_t* pub_key) {
  // Reuse a matching entry first (paranoia — sig_find should have hit).
  UiSigEntry* hit = sig_find(pub_key);
  if (hit) return hit;
  // Else find empty / oldest entry (LRU by last_ms).
  UiSigEntry* victim = &s_sig_cache[0];
  uint32_t oldest = victim->last_ms;
  for (int i = 1; i < UI_SIG_CACHE; i++) {
    if (s_sig_cache[i].last_ms == 0) {
      victim = &s_sig_cache[i];
      break;
    }
    if (s_sig_cache[i].last_ms < oldest) {
      oldest = s_sig_cache[i].last_ms;
      victim = &s_sig_cache[i];
    }
  }
  memcpy(victim->prefix, pub_key, 8);
  return victim;
}

extern "C" void ui_on_advert_received(const uint8_t* pub_key, float snr, float rssi) {
  UiSigEntry* e = sig_allocate(pub_key);
  e->snr_q   = (int8_t)(snr * 4.0f);
  e->rssi    = (int16_t)rssi;
  e->last_ms = millis() ? millis() : 1;     // 0 reserved as "empty"
}

struct UiContact {
  char     name[32];
  uint8_t  type;            // ADV_TYPE_*
  int32_t  gps_lat;         // x10^6
  int32_t  gps_lon;
  uint32_t last_advert;     // their clock (epoch seconds)
  int8_t   snr_q;           // SNR * 4 (or 0x80 / -128 = unknown)
  int16_t  rssi;            // dBm (or 0 = unknown)
  uint8_t  pub_key[32];     // S3.6c: needed for toggle-favorite + DM thread lookup
  uint8_t  flags;           // S3.6c: bit 0 = favorite
  uint8_t  path_len;        // S3.6b: hop count from Discovered entries (0 = direct)
};

// Contacts tile (S3.6c) shows favorites ONLY. The full contact list is
// still in contacts[], but the tile filters on `flags & 0x01`.
extern "C" int ui_get_contact_count() {
  int count = 0;
  for (uint32_t i = 0; i < MAX_CONTACTS; i++) {
    ContactInfo ci;
    if (!the_mesh.getContactByIdx(i, ci)) break;
    if (!ci.name[0]) continue;
    if (ci.flags & 0x01) count++;
  }
  return count;
}

// Indexed view across the favorites-only subset.
extern "C" bool ui_get_contact_info(int idx, UiContact* out) {
  if (!out) return false;
  int seen = 0;
  for (uint32_t i = 0; i < MAX_CONTACTS; i++) {
    ContactInfo ci;
    if (!the_mesh.getContactByIdx(i, ci)) break;
    if (!ci.name[0]) continue;
    if ((ci.flags & 0x01) == 0) continue;
    if (seen == idx) {
      memset(out, 0, sizeof(*out));
      StrHelper::strncpy(out->name, ci.name, sizeof(out->name));
      out->type        = ci.type;
      out->gps_lat     = ci.gps_lat;
      out->gps_lon     = ci.gps_lon;
      out->last_advert = ci.last_advert_timestamp;
      UiSigEntry* sig  = sig_find(ci.id.pub_key);
      out->snr_q       = sig ? sig->snr_q : (int8_t)-128;
      out->rssi        = sig ? sig->rssi  : (int16_t)0;
      memcpy(out->pub_key, ci.id.pub_key, 32);
      out->flags       = ci.flags;
      return true;
    }
    seen++;
  }
  return false;
}

// Toggle the favorite-bit on a contact identified by pub_key. Used by
// both the Contacts tile (unmark) and the Discovered tile (mark as
// favorite — which also adds to contacts[] first via ui_add_discovered).
extern "C" bool ui_toggle_favorite(const uint8_t* pub_key) {
  if (!the_mesh.uiToggleFavorite(pub_key)) return false;
  the_mesh.uiSaveContacts();
  return true;
}

// ---- Discovered nodes ring buffer (S3.6b) ----------------------------------
// Captures every advert we hear, including chats that don't pass the
// auto-add filter (default companion-app pref leaves AUTO_ADD_CHAT off,
// so the standard Contacts list misses other users entirely). Backed by
// the new BaseChatMesh hook `ui_on_advert_seen` which fires before the
// filter runs.
//
// 50 entries, LRU eviction on last_ms — enough for an active mesh hour.

struct UiDiscovered {
  uint8_t  pub_key[32];     // full key so we can call uiAddContact later
  char     name[32];
  uint8_t  type;            // ADV_TYPE_*
  int8_t   snr_q;           // SNR * 4
  int16_t  rssi;            // dBm
  uint8_t  path_len;        // 0 = direct, N = N hops
  uint32_t first_ms;        // millis() at first sighting (this boot)
  uint32_t last_ms;         // millis() at last update (0 = empty slot)
};
static const int UI_DISC_CACHE = 50;
// Heap-allocated in PSRAM (50 entries × ~80 bytes = 4 KB; doesn't fit in DRAM
// alongside everything else, and the ESP32-S3 on this board has 8 MB PSRAM).
// Allocated lazily on first ui_on_advert_seen call; UI bridges check for nullptr.
static UiDiscovered* s_disc_cache = nullptr;
static bool ensure_disc_cache() {
  if (s_disc_cache) return true;
  // PSRAM-backed zero-initialized buffer.
  s_disc_cache = (UiDiscovered*)heap_caps_calloc(
      UI_DISC_CACHE, sizeof(UiDiscovered),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return s_disc_cache != nullptr;
}

static UiDiscovered* disc_find(const uint8_t* pub_key) {
  if (!s_disc_cache) return nullptr;
  for (int i = 0; i < UI_DISC_CACHE; i++) {
    if (s_disc_cache[i].last_ms == 0) continue;
    if (memcmp(s_disc_cache[i].pub_key, pub_key, 32) == 0) return &s_disc_cache[i];
  }
  return nullptr;
}

// SD persistence for the discovered cache (defined below ui_on_advert_seen).
void disc_persist_save();
void disc_persist_load();

static UiDiscovered* disc_allocate(const uint8_t* pub_key) {
  if (!ensure_disc_cache()) return nullptr;
  UiDiscovered* hit = disc_find(pub_key);
  if (hit) return hit;
  // empty slot first, else LRU by last_ms
  UiDiscovered* victim = &s_disc_cache[0];
  uint32_t oldest = victim->last_ms;
  for (int i = 1; i < UI_DISC_CACHE; i++) {
    if (s_disc_cache[i].last_ms == 0) { victim = &s_disc_cache[i]; break; }
    if (s_disc_cache[i].last_ms < oldest) {
      oldest = s_disc_cache[i].last_ms;
      victim = &s_disc_cache[i];
    }
  }
  memcpy(victim->pub_key, pub_key, 32);
  victim->first_ms = millis() ? millis() : 1;
  return victim;
}

extern "C" void ui_on_advert_seen(const uint8_t* pub_key, const char* name,
                                  uint8_t type, float snr, float rssi,
                                  uint8_t path_len) {
  UiDiscovered* e = disc_allocate(pub_key);
  if (!e) return;     // PSRAM alloc failed; silently drop
  StrHelper::strncpy(e->name, name ? name : "", sizeof(e->name));
  e->type     = type;
  e->snr_q    = (int8_t)(snr * 4.0f);
  e->rssi     = (int16_t)rssi;
  e->path_len = path_len;
  e->last_ms  = millis() ? millis() : 1;

  // Throttled persist of the whole cache to SD (item: save discovered nodes).
  static unsigned long s_disc_save_next = 0;
  unsigned long now = millis();
  if ((long)(now - s_disc_save_next) >= 0) {
    s_disc_save_next = now + 15000;
    disc_persist_save();
  }
}

// ---- Discovered-nodes SD persistence (whole-cache binary dump) -------------
void disc_persist_save() {
  if (!s_disc_cache || !sd_init()) return;
  FILE* f = fopen("/sd/disc.dat", "wb");
  if (!f) return;
  uint32_t magic = 0x43534944;   // 'DISC'
  fwrite(&magic, 4, 1, f);
  for (int i = 0; i < UI_DISC_CACHE; i++) {
    if (s_disc_cache[i].last_ms == 0) continue;
    fwrite(&s_disc_cache[i], sizeof(UiDiscovered), 1, f);
  }
  fclose(f);
}

void disc_persist_load() {
  if (!ensure_disc_cache() || !sd_init()) return;
  FILE* f = fopen("/sd/disc.dat", "rb");
  if (!f) return;
  uint32_t magic = 0;
  if (fread(&magic, 4, 1, f) != 1 || magic != 0x43534944) { fclose(f); return; }
  int idx = 0;
  UiDiscovered e;
  while (idx < UI_DISC_CACHE && fread(&e, sizeof(e), 1, f) == 1) {
    memcpy(&s_disc_cache[idx], &e, sizeof(e));
    // millis() fields are meaningless across a reboot; mark the slot
    // occupied with a tiny age so it sorts as "old/loaded".
    s_disc_cache[idx].first_ms = 1;
    s_disc_cache[idx].last_ms  = 1;
    idx++;
  }
  fclose(f);
}

extern "C" int ui_get_discovered_count() {
  if (!s_disc_cache) return 0;
  int n = 0;
  for (int i = 0; i < UI_DISC_CACHE; i++) {
    if (s_disc_cache[i].last_ms != 0) n++;
  }
  return n;
}

// Returns discovered entries newest-first (by last_ms).
extern "C" bool ui_get_discovered_info(int idx, UiContact* out) {
  if (!out || !s_disc_cache) return false;
  // Collect non-empty indices, sort by last_ms desc — cheap for 50 entries.
  int order[UI_DISC_CACHE];
  int n = 0;
  for (int i = 0; i < UI_DISC_CACHE; i++) {
    if (s_disc_cache[i].last_ms != 0) order[n++] = i;
  }
  for (int i = 1; i < n; i++) {
    int v = order[i];
    int j = i;
    while (j > 0 && s_disc_cache[order[j-1]].last_ms < s_disc_cache[v].last_ms) {
      order[j] = order[j-1]; j--;
    }
    order[j] = v;
  }
  if (idx < 0 || idx >= n) return false;
  const UiDiscovered& d = s_disc_cache[order[idx]];
  memset(out, 0, sizeof(*out));
  StrHelper::strncpy(out->name, d.name, sizeof(out->name));
  out->type        = d.type;
  out->snr_q       = d.snr_q;
  out->rssi        = d.rssi;
  out->last_advert = d.last_ms / 1000;  // ms since boot → "secs since boot"
                                        // Discovered tile shows "Ns ago" relative.
  memcpy(out->pub_key, d.pub_key, 32);
  out->flags       = 0;
  out->path_len    = d.path_len;
  return true;
}

// Add a discovered entry to contacts[]. `favorite=true` also marks
// the contact as favorite (bit 0 of flags), making it appear in the
// favorites-only Contacts tile.
extern "C" bool ui_add_discovered_to_contacts(int idx, bool favorite) {
  UiContact uc;
  if (!ui_get_discovered_info(idx, &uc)) return false;
  if (!the_mesh.uiAddContact(uc.pub_key, uc.name, uc.type, favorite)) return false;
  the_mesh.uiSaveContacts();
  return true;
}

// ---- DM tile bridges (S3.6d) -----------------------------------------------
//
// DM tile lists chat-type contacts from contacts[] (regardless of
// favorite — DM is for any chat contact). 32-entry shared ring buffer
// in PSRAM, tagged by 8-byte pub_key prefix. Not persisted across
// boots — S3.6d.2 will swap this for an AES-256-CTR-encrypted SD store.

struct DmMsg {
  uint8_t  pubkey8[8];      // first 8 bytes of contact's pub_key (tagging)
  uint8_t  from_me;         // 1 = sent by us, 0 = received
  char     text[120];       // Tanmatsu-compatible 120-char limit
  uint32_t ts_ms;           // millis() at append (in-memory only)
};
static const int DM_RING_SIZE = 32;
static DmMsg* s_dm_ring = nullptr;
static int    s_dm_ring_head = 0;     // next write slot (wraps)
static int    s_dm_ring_count = 0;    // # valid entries, ≤ DM_RING_SIZE

static bool ensure_dm_ring() {
  if (s_dm_ring) return true;
  s_dm_ring = (DmMsg*)heap_caps_calloc(
      DM_RING_SIZE, sizeof(DmMsg),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return s_dm_ring != nullptr;
}

static void dm_ring_push(const uint8_t* pub_key, bool from_me, const char* text) {
  if (!ensure_dm_ring() || !text) return;
  DmMsg& m = s_dm_ring[s_dm_ring_head];
  memcpy(m.pubkey8, pub_key, 8);
  m.from_me = from_me ? 1 : 0;
  StrHelper::strncpy(m.text, text, sizeof(m.text));
  m.ts_ms = millis();
  s_dm_ring_head = (s_dm_ring_head + 1) % DM_RING_SIZE;
  if (s_dm_ring_count < DM_RING_SIZE) s_dm_ring_count++;
}

extern "C" void ui_refresh_open_dm(const uint8_t* pub_key) __attribute__((weak));

// ---- DM history SD persistence --------------------------------------------
// One append-log per peer at /sd/dm_<hex8>.dat. Each record is
// [uint8 from_me][uint8 len][len bytes]. Loaded into the ring on chat open.
bool sd_init();   // from target.cpp
static bool dm_has_active_history(const uint8_t* pub_key);   // fwd

static void dm_hist_path(const uint8_t* pub_key, char* out, size_t cap) {
  static const char* H = "0123456789abcdef";
  char hx[17];
  for (int i = 0; i < 8; i++) { hx[i*2] = H[pub_key[i] >> 4]; hx[i*2+1] = H[pub_key[i] & 0x0f]; }
  hx[16] = 0;
  snprintf(out, cap, "/sd/dm_%s.dat", hx);
}

static void dm_persist_append(const uint8_t* pub_key, bool from_me, const char* text) {
  if (!text || !sd_init()) return;
  char path[40]; dm_hist_path(pub_key, path, sizeof(path));
  FILE* f = fopen(path, "ab");
  if (!f) return;
  uint8_t fm = from_me ? 1 : 0;
  size_t L = strlen(text); if (L > 119) L = 119;
  uint8_t len = (uint8_t)L;
  fwrite(&fm, 1, 1, f); fwrite(&len, 1, 1, f); fwrite(text, 1, L, f);
  fclose(f);
}

// Restore a peer's history into the ring (oldest-first, last DM_RING_SIZE).
// No-op if the ring already holds messages for the peer.
extern "C" void ui_load_dm_history(const uint8_t* pub_key) {
  if (dm_has_active_history(pub_key) || !sd_init()) return;
  char path[40]; dm_hist_path(pub_key, path, sizeof(path));
  FILE* f = fopen(path, "rb");
  if (!f) return;
  static char texts[DM_RING_SIZE][120];
  static uint8_t fms[DM_RING_SIZE];
  int n = 0;
  uint8_t fm, len;
  while (fread(&fm, 1, 1, f) == 1 && fread(&len, 1, 1, f) == 1) {
    if (len > 119) break;
    char buf[120];
    if (fread(buf, 1, len, f) != len) break;
    buf[len] = 0;
    int s = n % DM_RING_SIZE;
    fms[s] = fm; memcpy(texts[s], buf, len + 1);
    n++;
  }
  fclose(f);
  int start = n > DM_RING_SIZE ? n - DM_RING_SIZE : 0;
  for (int i = start; i < n; i++) {
    int s = i % DM_RING_SIZE;
    dm_ring_push(pub_key, fms[s] != 0, texts[s]);   // ring only — no re-persist
  }
}

extern "C" void ui_on_dm_message(const uint8_t* pub_key, uint32_t timestamp,
                                 const char* text) {
  (void)timestamp;
  dm_ring_push(pub_key, /*from_me=*/false, text);
  dm_persist_append(pub_key, /*from_me=*/false, text);
  if (ui_refresh_open_dm) ui_refresh_open_dm(pub_key);
}

// DM tile lists only ACTIVE DM threads: chat-type contacts where we
// either have ring-buffer history OR which are marked as favorite
// (so a "start DM" via Contacts → Open DM has somewhere to land).
// This avoids cluttering the tile with every chat-type advert ever
// heard (which is what the Discovered tile is for).
static bool dm_has_active_history(const uint8_t* pub_key) {
  if (!s_dm_ring) return false;
  for (int i = 0; i < s_dm_ring_count; i++) {
    if (memcmp(s_dm_ring[i].pubkey8, pub_key, 8) == 0) return true;
  }
  return false;
}

extern "C" int ui_get_dm_contact_count() {
  int count = 0;
  for (uint32_t i = 0; i < MAX_CONTACTS; i++) {
    ContactInfo ci;
    if (!the_mesh.getContactByIdx(i, ci)) break;
    if (!ci.name[0]) continue;
    if (ci.type != 1 /* ADV_TYPE_CHAT */) continue;
    bool fav    = (ci.flags & 0x01) != 0;
    bool active = dm_has_active_history(ci.id.pub_key);
    if (fav || active) count++;
  }
  return count;
}

extern "C" bool ui_get_dm_contact_info(int idx, UiContact* out) {
  if (!out) return false;
  int seen = 0;
  for (uint32_t i = 0; i < MAX_CONTACTS; i++) {
    ContactInfo ci;
    if (!the_mesh.getContactByIdx(i, ci)) break;
    if (!ci.name[0]) continue;
    if (ci.type != 1) continue;
    bool fav    = (ci.flags & 0x01) != 0;
    bool active = dm_has_active_history(ci.id.pub_key);
    if (!fav && !active) continue;
    if (seen == idx) {
      memset(out, 0, sizeof(*out));
      StrHelper::strncpy(out->name, ci.name, sizeof(out->name));
      out->type        = ci.type;
      out->last_advert = ci.last_advert_timestamp;
      memcpy(out->pub_key, ci.id.pub_key, 32);
      out->flags       = ci.flags;
      UiSigEntry* sig  = sig_find(ci.id.pub_key);
      out->snr_q       = sig ? sig->snr_q : (int8_t)-128;
      out->rssi        = sig ? sig->rssi  : (int16_t)0;
      return true;
    }
    seen++;
  }
  return false;
}

extern "C" bool ui_send_dm(const uint8_t* pub_key, const char* text) {
  if (!pub_key || !text || !text[0]) return false;
  if (!the_mesh.uiSendDm(pub_key, text)) return false;
  // Also append to the local ring so the user sees their own send.
  dm_ring_push(pub_key, /*from_me=*/true, text);
  dm_persist_append(pub_key, /*from_me=*/true, text);
  return true;
}

// Variant UIs iterate the ring tagged by contact. `idx` is 0-indexed
// over messages matching `pub_key8` (first 8 bytes of full pub_key),
// returned oldest-first. Returns false past the end of the per-contact
// list.
struct UiDmMsg {
  char     text[120];
  uint8_t  from_me;
  uint32_t age_s;       // seconds since this message was appended
};

extern "C" int ui_get_dm_msg_count(const uint8_t* pub_key) {
  if (!s_dm_ring || !pub_key) return 0;
  int n = 0;
  for (int i = 0; i < s_dm_ring_count; i++) {
    if (memcmp(s_dm_ring[i].pubkey8, pub_key, 8) == 0) n++;
  }
  return n;
}

extern "C" bool ui_get_dm_msg(const uint8_t* pub_key, int idx, UiDmMsg* out) {
  if (!s_dm_ring || !pub_key || !out) return false;
  // Iterate oldest-first: start at head and walk forward (head is the
  // NEXT-write slot, so head itself is oldest once the ring is full).
  int start = (s_dm_ring_count < DM_RING_SIZE) ? 0 : s_dm_ring_head;
  int seen = 0;
  uint32_t now = millis();
  for (int k = 0; k < s_dm_ring_count; k++) {
    int i = (start + k) % DM_RING_SIZE;
    if (memcmp(s_dm_ring[i].pubkey8, pub_key, 8) != 0) continue;
    if (seen == idx) {
      StrHelper::strncpy(out->text, s_dm_ring[i].text, sizeof(out->text));
      out->from_me = s_dm_ring[i].from_me;
      out->age_s   = (now - s_dm_ring[i].ts_ms) / 1000;
      return true;
    }
    seen++;
  }
  return false;
}

extern "C" void ui_get_self_loc(double* lat, double* lon) {
  if (lat) *lat = sensors.node_lat;
  if (lon) *lon = sensors.node_lon;
}

// Saved "home" location (T-Pager Settings tile). ui_get_home_loc returns
// true only when a home has been stored (non-zero). ui_set_home_loc stores
// + persists; (0,0) clears it.
extern "C" bool ui_get_home_loc(double* lat, double* lon) {
  NodePrefs* p = the_mesh.getNodePrefs();
  if (lat) *lat = p->home_lat;
  if (lon) *lon = p->home_lon;
  return (p->home_lat != 0.0 || p->home_lon != 0.0);
}

extern "C" void ui_set_home_loc(double lat, double lon) {
  NodePrefs* p = the_mesh.getNodePrefs();
  p->home_lat = lat;
  p->home_lon = lon;
  the_mesh.savePrefs();
}

extern "C" uint32_t ui_get_now_epoch() {
  return the_mesh.getRTCClock()->getCurrentTime();
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

  // S3.6 follow-up — confirm PSRAM is alive. Without this, the chat /
  // discovered / DM ring buffers (heap_caps_calloc(MALLOC_CAP_SPIRAM))
  // silently return NULL and the UI shows empty lists. Banner is also
  // useful in the next round of testing.
  Serial.printf("[BOOT] psramFound=%d psram_size=%u free_psram=%u\n",
                psramFound() ? 1 : 0,
                (unsigned)ESP.getPsramSize(),
                (unsigned)ESP.getFreePsram());

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

  // Restore the discovered-nodes cache from SD (DM/channel history load
  // lazily on chat open). Safe no-op if there's no card / no file.
  disc_persist_load();

#if ENV_INCLUDE_GPS == 1
  the_mesh.applyGpsPrefs();
  #if defined(T_LORA_PAGER_LR1121)
  // The companion default is gps_enabled=0 (the phone toggles it on). The
  // pager is a standalone GPS device with no such toggle, and applyGpsPrefs()
  // above just turned the receiver off — force it on so it actually acquires
  // and MicroNMEA parses the stream. (ENV_SKIP_GPS_DETECT makes gps_detected
  // true so this setting is accepted.)
  sensors.setSettingValue("gps", "1");
  sensors.setSettingValue("gps_interval", "1");
  #endif
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
