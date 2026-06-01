// T-Pager tile-carousel UI — replaces examples/companion_radio/ui-new/UITask.h
// via the variant's include-path precedence. Keeps the same class name and
// constructor signature so companion/main.cpp picks us up without changes.
//
// Layout (landscape 480x222, rendered to a 240x111 buffer at UI_ZOOM=2):
//
//   <  [tile-1]  [tile-2]  [tile-3]  >
//                  Title
//
// Encoder rotation scrolls the row; click enters the focused tile.

#pragma once

#include <MeshCore.h>
#include <helpers/ui/DisplayDriver.h>
#include <helpers/SensorManager.h>
#include <helpers/BaseSerialInterface.h>
#include <Arduino.h>

#include "../../examples/companion_radio/AbstractUITask.h"
#include "../../examples/companion_radio/NodePrefs.h"

#ifndef AUTO_OFF_MILLIS
  #define AUTO_OFF_MILLIS  60000
#endif

class UITask : public AbstractUITask {
  DisplayDriver* _display;
  SensorManager* _sensors;
  NodePrefs* _prefs;

  unsigned long _next_refresh, _auto_off;
  int _msgcount;
  uint8_t _tile;        // currently focused tile index
  uint8_t _screen;      // 0 = carousel, 1 = inside tile (stub sub-screen)
  int _last_enc_a;
  int _prev_btn;
  unsigned long _btn_press_at;

  void renderCarousel();
  void renderSubscreen();
  void pollInput();

public:
  UITask(mesh::MainBoard* board, BaseSerialInterface* serial)
    : AbstractUITask(board, serial),
      _display(NULL), _sensors(NULL), _prefs(NULL),
      _next_refresh(0), _auto_off(0), _msgcount(0),
      _tile(0), _screen(0),
      _last_enc_a(HIGH), _prev_btn(HIGH), _btn_press_at(0) {}

  void begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* prefs);
  void loop() override;

  // Companion hooks — minimal stubs for now; full behaviour comes later.
  void msgRead(int msgcount) override { _msgcount = msgcount; }
  void newMsg(uint8_t /*path_len*/, const char* /*from*/, const char* /*text*/, int msgcount) override {
    _msgcount = msgcount;
    _auto_off = millis() + AUTO_OFF_MILLIS;
    if (_display) _display->turnOn();
  }
  void notify(UIEventType /*t*/ = UIEventType::none) override {
    _auto_off = millis() + AUTO_OFF_MILLIS;
    if (_display) _display->turnOn();
  }

  // Extra hooks that companion's main.cpp / MyMesh.cpp call on the standard
  // UITask — provide harmless defaults so the link succeeds.
  void showAlert(const char* /*text*/, int /*duration_millis*/) {}
  int  getMsgCount() const { return _msgcount; }
  bool hasDisplay() const { return _display != NULL; }
  bool isButtonPressed() const { return false; }
  void gotoHomeScreen() { _screen = 0; _next_refresh = 0; }
  void shutdown(bool /*restart*/ = false) {}
  void userLedHandler() {}
};
