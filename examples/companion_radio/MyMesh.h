#pragma once

#include <Arduino.h>
#include <Mesh.h>
#include "AbstractUITask.h"

/*------------ Frame Protocol --------------*/
#define FIRMWARE_VER_CODE 13

#ifndef FIRMWARE_BUILD_DATE
#define FIRMWARE_BUILD_DATE "6 Jun 2026"
#endif

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "v1.16.0"
#endif

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
#include <InternalFileSystem.h>
#elif defined(RP2040_PLATFORM)
#include <LittleFS.h>
#elif defined(ESP32)
#include <SPIFFS.h>
#endif

#include "DataStore.h"
#include "NodePrefs.h"

#include <RTClib.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/BaseSerialInterface.h>
#include <helpers/IdentityStore.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>
#include <target.h>

/* ---------------------------------- CONFIGURATION ------------------------------------- */

#ifndef LORA_FREQ
#define LORA_FREQ 915.0
#endif
#ifndef LORA_BW
#define LORA_BW 250
#endif
#ifndef LORA_SF
#define LORA_SF 10
#endif
#ifndef LORA_CR
#define LORA_CR 5
#endif
#ifndef LORA_TX_POWER
#define LORA_TX_POWER 20
#endif
#ifndef MAX_LORA_TX_POWER
#define MAX_LORA_TX_POWER LORA_TX_POWER
#endif

#ifndef MAX_CONTACTS
#define MAX_CONTACTS 100
#endif

#ifndef OFFLINE_QUEUE_SIZE
#define OFFLINE_QUEUE_SIZE 16
#endif

#ifndef BLE_NAME_PREFIX
#define BLE_NAME_PREFIX "MeshCore-"
#endif

#include <helpers/BaseChatMesh.h>
#include <helpers/TransportKeyStore.h>

/* -------------------------------------------------------------------------------------- */

#define REQ_TYPE_GET_STATUS             0x01 // same as _GET_STATS
#define REQ_TYPE_KEEP_ALIVE             0x02
#define REQ_TYPE_GET_TELEMETRY_DATA     0x03

struct AdvertPath {
  uint8_t pubkey_prefix[7];
  uint8_t path_len;
  char    name[32];
  uint32_t recv_timestamp;
  uint8_t path[MAX_PATH_SIZE];
};

class MyMesh : public BaseChatMesh, public DataStoreHost {
public:
  MyMesh(mesh::Radio &radio, mesh::RNG &rng, mesh::RTCClock &rtc, SimpleMeshTables &tables, DataStore& store, AbstractUITask* ui=NULL);

  void begin(bool has_display);
  void startInterface(BaseSerialInterface &serial);

  const char *getNodeName();
  NodePrefs *getNodePrefs();
  uint32_t getBLEPin();

  void loop();
  void handleCmdFrame(size_t len);
  bool advert();
  void enterCLIRescue();

  int  getRecentlyHeard(AdvertPath dest[], int max_num);

protected:
  float getAirtimeBudgetFactor() const override;
  int getInterferenceThreshold() const override;
  int calcRxDelay(float score, uint32_t air_time) const override;
  uint32_t getRetransmitDelay(const mesh::Packet *packet) override;
  uint32_t getDirectRetransmitDelay(const mesh::Packet *packet) override;
  uint8_t getExtraAckTransmitCount() const override;
  bool filterRecvFloodPacket(mesh::Packet* packet) override;
  bool allowPacketForward(const mesh::Packet* packet) override;

  void sendFloodScoped(const TransportKey& scope, mesh::Packet* pkt, uint32_t delay_millis);
  void sendFloodScoped(const ContactInfo& recipient, mesh::Packet* pkt, uint32_t delay_millis=0) override;
  void sendFloodScoped(const mesh::GroupChannel& channel, mesh::Packet* pkt, uint32_t delay_millis=0) override;

  void logRxRaw(float snr, float rssi, const uint8_t raw[], int len) override;
  bool isAutoAddEnabled() const override;
  bool shouldAutoAddContactType(uint8_t type) const override;
  bool shouldOverwriteWhenFull() const override;
  uint8_t getAutoAddMaxHops() const override;
  void onContactsFull() override;
  void onContactOverwrite(const uint8_t* pub_key) override;
  bool onContactPathRecv(ContactInfo& from, uint8_t* in_path, uint8_t in_path_len, uint8_t* out_path, uint8_t out_path_len, uint8_t extra_type, uint8_t* extra, uint8_t extra_len) override;
  void onDiscoveredContact(ContactInfo &contact, bool is_new, uint8_t path_len, const uint8_t* path) override;
  void onContactPathUpdated(const ContactInfo &contact) override;
  ContactInfo* processAck(const uint8_t *data) override;
  void queueMessage(const ContactInfo &from, uint8_t txt_type, mesh::Packet *pkt, uint32_t sender_timestamp,
                    const uint8_t *extra, int extra_len, const char *text);

  void onMessageRecv(const ContactInfo &from, mesh::Packet *pkt, uint32_t sender_timestamp,
                     const char *text) override;
  void onCommandDataRecv(const ContactInfo &from, mesh::Packet *pkt, uint32_t sender_timestamp,
                         const char *text) override;
  void onSignedMessageRecv(const ContactInfo &from, mesh::Packet *pkt, uint32_t sender_timestamp,
                           const uint8_t *sender_prefix, const char *text) override;
  void onChannelMessageRecv(const mesh::GroupChannel &channel, mesh::Packet *pkt, uint32_t timestamp,
                            const char *text) override;
  void onChannelDataRecv(const mesh::GroupChannel &channel, mesh::Packet *pkt, uint16_t data_type,
                         const uint8_t *data, size_t data_len) override;

  uint8_t onContactRequest(const ContactInfo &contact, uint32_t sender_timestamp, const uint8_t *data,
                           uint8_t len, uint8_t *reply) override;
  void onContactResponse(const ContactInfo &contact, const uint8_t *data, uint8_t len) override;
  void onControlDataRecv(mesh::Packet *packet) override;
  void onRawDataRecv(mesh::Packet *packet) override;
  void onTraceRecv(mesh::Packet *packet, uint32_t tag, uint32_t auth_code, uint8_t flags,
                   const uint8_t *path_snrs, const uint8_t *path_hashes, uint8_t path_len) override;

  uint32_t calcFloodTimeoutMillisFor(uint32_t pkt_airtime_millis) const override;
  uint32_t calcDirectTimeoutMillisFor(uint32_t pkt_airtime_millis, uint8_t path_len) const override;
  void onSendTimeout() override;

  // DataStoreHost methods
  bool onContactLoaded(const ContactInfo& contact) override { return addContact(contact); }
  bool getContactForSave(uint32_t idx, ContactInfo& contact) override { return getContactByIdx(idx, contact); }
  bool onChannelLoaded(uint8_t channel_idx, const ChannelDetails& ch) override { return setChannel(channel_idx, ch); }
  bool getChannelForSave(uint8_t channel_idx, ChannelDetails& ch) override { return getChannel(channel_idx, ch); }

  void clearPendingReqs() {
    pending_login = pending_status = pending_telemetry = pending_discovery = pending_req = 0;
  }

public:
  void savePrefs() { _store->savePrefs(_prefs, sensors.node_lat, sensors.node_lon); }
  // Public wrapper around the private saveChannels() shortcut so variant
  // UIs can persist a newly-added channel after addChannel().
  void uiSaveChannels() { _store->saveChannels(this); }

  // Variant-UI channel add. Walks channels[] for an empty slot and uses
  // setChannel() so we never overwrite a slot that loadChannels() filled
  // (the upstream `num_channels` counter is only bumped by addChannel,
  // not by setChannel-via-load, so after a reboot it stays at 1 and
  // BaseChatMesh::addChannel would clobber slot 1). The 16-byte secret
  // is consumed verbatim (no base64 round-trip).
  bool uiAddHashtagChannel(const char* name, const uint8_t secret16[16]) {
    int idx = -1;
    for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
      if (channels[i].name[0] == 0) { idx = i; break; }
    }
    if (idx < 0) return false;
    ChannelDetails ch;
    memset(&ch, 0, sizeof(ch));
    memcpy(ch.channel.secret, secret16, 16);   // upper 16 bytes stay zero
    StrHelper::strncpy(ch.name, name, sizeof(ch.name));
    if (!setChannel(idx, ch)) return false;
    if (idx + 1 > num_channels) num_channels = idx + 1;
    return true;
  }

  // Variant-UI bridges for the Discovered + Contacts tiles (S3.6b/c).
  void uiSaveContacts() { _store->saveContacts(this); }

  // Variant-UI channel delete (S3.6 round 2). Walks channels[] to find
  // the Nth POPULATED slot (matching how the UI lists channels via
  // ui_get_channel_name), zeroes it, then compacts the table so
  // populated-index == slot-index stays true (otherwise subsequent
  // sends through getChannel(slot) would hit empty slots).
  bool uiDeleteChannel(int populated_idx) {
    int seen = 0, target_slot = -1;
    for (int slot = 0; slot < MAX_GROUP_CHANNELS; slot++) {
      if (channels[slot].name[0] == 0) continue;
      if (seen == populated_idx) { target_slot = slot; break; }
      seen++;
    }
    if (target_slot < 0) return false;
    // Compact: shift later populated entries down by one.
    for (int slot = target_slot; slot + 1 < MAX_GROUP_CHANNELS; slot++) {
      channels[slot] = channels[slot + 1];
    }
    memset(&channels[MAX_GROUP_CHANNELS - 1], 0,
           sizeof(channels[MAX_GROUP_CHANNELS - 1]));
    while (num_channels > 0 && channels[num_channels - 1].name[0] == 0) {
      num_channels--;
    }
    return true;
  }

  // Add a discovered node into contacts[]. Caller passes the cached
  // pub_key + name + type from the Discovered tile ring buffer. We can't
  // recover the full advert blob (path, lat/lon, signed payload) without
  // another advert, but the bare contact entry is enough — the next
  // onAdvertRecv from that pub_key will fill in the rest naturally.
  // Returns false on table-full.
  bool uiAddContact(const uint8_t* pub_key, const char* name, uint8_t type,
                    bool favorite, int32_t lat = 0, int32_t lon = 0) {
    // De-dup: if already present, just update flags + coords + (re-)save.
    for (int i = 0; i < num_contacts; i++) {
      if (memcmp(contacts[i].id.pub_key, pub_key, PUB_KEY_SIZE) == 0) {
        if (favorite) contacts[i].flags |= 0x01;
        if (lat != 0 || lon != 0) { contacts[i].gps_lat = lat; contacts[i].gps_lon = lon; }
        return true;
      }
    }
    if (num_contacts >= MAX_CONTACTS) return false;
    ContactInfo& ci = contacts[num_contacts];
    memset(&ci, 0, sizeof(ci));
    memcpy(ci.id.pub_key, pub_key, PUB_KEY_SIZE);
    ci.out_path_len = OUT_PATH_UNKNOWN;
    StrHelper::strncpy(ci.name, name, sizeof(ci.name));
    ci.type = type;
    ci.gps_lat = lat;                         // 0 if the advert carried no location
    ci.gps_lon = lon;
    if (favorite) ci.flags |= 0x01;
    ci.last_advert_timestamp = 0;             // fills on next advert
    ci.lastmod = getRTCClock()->getCurrentTime();
    num_contacts++;
    return true;
  }

  // Toggle the favorite-bit on a contact, looked up by pub_key prefix
  // (full 32 bytes). Returns true if found+toggled, false if not present.
  bool uiToggleFavorite(const uint8_t* pub_key) {
    for (int i = 0; i < num_contacts; i++) {
      if (memcmp(contacts[i].id.pub_key, pub_key, PUB_KEY_SIZE) == 0) {
        contacts[i].flags ^= 0x01;
        return true;
      }
    }
    return false;
  }

  // Variant-UI self-advert send (S3.6 round 3). Builds the same packet
  // the companion-app CMD_SEND_SELF_ADVERT path produces. `flood=true`
  // sends a flood-routed advert (visible to the mesh beyond direct
  // neighbours); flood=false sends zero-hop (only direct hearers).
  bool uiSendSelfAdvert(bool flood) {
    mesh::Packet* pkt;
    if (_prefs.advert_loc_policy == ADVERT_LOC_NONE) {
      pkt = createSelfAdvert(_prefs.node_name);
    } else {
      pkt = createSelfAdvert(_prefs.node_name, sensors.node_lat, sensors.node_lon);
    }
    if (!pkt) return false;
    if (flood) {
      TransportKey default_scope;
      memcpy(&default_scope.key, _prefs.default_scope_key, sizeof(default_scope.key));
      sendFloodScoped(default_scope, pkt, /*delay=*/0);
    } else {
      sendZeroHop(pkt);
    }
    return true;
  }

  // Variant-UI rename — set node_name + persist.
  void uiSetNodeName(const char* name) {
    if (!name) return;
    StrHelper::strncpy(_prefs.node_name, name, sizeof(_prefs.node_name));
    savePrefs();
  }

  // Variant-UI DM send (S3.6d). Looks up the contact by pub_key, calls
  // sendMessage() with the current RTC time. Returns false if the
  // contact isn't in contacts[] (DM requires a known contact —
  // populateContactFromAdvert sets the basics, but a path is needed for
  // sendDirect; sendMessage falls back to flood routing if path is
  // OUT_PATH_UNKNOWN).
  bool uiSendDm(const uint8_t* pub_key, const char* text) {
    for (int i = 0; i < num_contacts; i++) {
      if (memcmp(contacts[i].id.pub_key, pub_key, PUB_KEY_SIZE) == 0) {
        uint32_t ack = 0, est = 0;
        uint32_t now = getRTCClock()->getCurrentTime();
        int r = sendMessage(contacts[i], now, 0, text, ack, est);
        return r >= 0;
      }
    }
    return false;
  }

#if ENV_INCLUDE_GPS == 1
  void applyGpsPrefs() {
    sensors.setSettingValue("gps", _prefs.gps_enabled ? "1" : "0");
    if (_prefs.gps_interval > 0) {
      char interval_str[12];  // Max: 24 hours = 86400 seconds (5 digits + null)
      sprintf(interval_str, "%u", _prefs.gps_interval);
      sensors.setSettingValue("gps_interval", interval_str);
    }
  }
#endif

  // To check if there is pending work
  bool hasPendingWork() const;

private:
  void writeOKFrame();
  void writeErrFrame(uint8_t err_code);
  void writeDisabledFrame();
  void writeContactRespFrame(uint8_t code, const ContactInfo &contact);
  void updateContactFromFrame(ContactInfo &contact, uint32_t& last_mod, const uint8_t *frame, int len);
  void addToOfflineQueue(const uint8_t frame[], int len);
  int getFromOfflineQueue(uint8_t frame[]);
  int getBlobByKey(const uint8_t key[], int key_len, uint8_t dest_buf[]) override { 
    return _store->getBlobByKey(key, key_len, dest_buf);
  }
  bool putBlobByKey(const uint8_t key[], int key_len, const uint8_t src_buf[], int len) override {
    return _store->putBlobByKey(key, key_len, src_buf, len);
  }

  void checkCLIRescueCmd();
  void checkSerialInterface();
  bool isValidClientRepeatFreq(uint32_t f) const;

  // helpers, short-cuts
  void saveChannels() { _store->saveChannels(this); }
  void saveContacts();

  DataStore* _store;
  NodePrefs _prefs;
  uint32_t pending_login;
  uint32_t pending_status;
  uint32_t pending_telemetry, pending_discovery;   // pending _TELEMETRY_REQ
  uint32_t pending_req;   // pending _BINARY_REQ
  BaseSerialInterface *_serial;
  AbstractUITask* _ui;

  ContactsIterator _iter;
  uint32_t _iter_filter_since;
  uint32_t _most_recent_lastmod;
  uint32_t _active_ble_pin;
  bool _iter_started;
  bool _cli_rescue;
  bool send_unscoped;   // force un-scoped flood (instead of using send_scope)
  char cli_command[80];
  uint8_t app_target_ver;
  uint8_t *sign_data;
  uint32_t sign_data_len;
  unsigned long dirty_contacts_expiry;

  TransportKey send_scope;

  uint8_t cmd_frame[MAX_FRAME_SIZE + 1];
  uint8_t out_frame[MAX_FRAME_SIZE + 1];
  CayenneLPP telemetry;

  struct Frame {
    uint8_t len;
    uint8_t buf[MAX_FRAME_SIZE];

    bool isChannelMsg() const;
  };
  int offline_queue_len;
  Frame offline_queue[OFFLINE_QUEUE_SIZE];

  struct AckTableEntry {
    unsigned long msg_sent;
    uint32_t ack;
    ContactInfo* contact;
  };
  #define EXPECTED_ACK_TABLE_SIZE 8
  AckTableEntry expected_ack_table[EXPECTED_ACK_TABLE_SIZE]; // circular table
  int next_ack_idx;

  #define ADVERT_PATH_TABLE_SIZE   16
  AdvertPath advert_paths[ADVERT_PATH_TABLE_SIZE]; // circular table
};

extern MyMesh the_mesh;
