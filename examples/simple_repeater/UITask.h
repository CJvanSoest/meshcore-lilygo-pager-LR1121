#pragma once

#include <helpers/ui/DisplayDriver.h>
#include <helpers/CommonCLI.h>

// Variants with multiple screens (e.g. encoder-driven UIs) override this.
#ifndef NUM_SCREENS
  #define NUM_SCREENS 1
#endif

// Optional compose screen (variants with a keyboard set COMPOSE_BUFFER_SIZE)
#ifndef COMPOSE_BUFFER_SIZE
  #define COMPOSE_BUFFER_SIZE 0
#endif

class UITask {
  DisplayDriver* _display;
  unsigned long _next_read, _next_refresh, _auto_off;
  int _prevBtnState;
  uint8_t _current_screen = 0;   // 0 = home, 1..N-1 = additional screens
  NodePrefs* _node_prefs;
  char _version_info[32];
#if COMPOSE_BUFFER_SIZE > 0
  char _compose_buf[COMPOSE_BUFFER_SIZE];
  uint16_t _compose_len = 0;
#endif

  void renderCurrScreen();
public:
  UITask(DisplayDriver& display) : _display(&display) { _next_read = _next_refresh = 0; }
  void begin(NodePrefs* node_prefs, const char* build_date, const char* firmware_version);

  void loop();

#if COMPOSE_BUFFER_SIZE > 0
  // Input from a variant keyboard. '\b' / 0x08 removes the last char,
  // '\n' / '\r' triggers send via the weak ui_send_message bridge,
  // other printable chars are appended (truncated if buffer full).
  void addInputChar(char c);
  bool isComposeActive() const { return _current_screen == 2; }
#endif
};