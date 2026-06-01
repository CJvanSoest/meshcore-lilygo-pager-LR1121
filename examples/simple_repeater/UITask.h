#pragma once

#include <helpers/ui/DisplayDriver.h>
#include <helpers/CommonCLI.h>

// Variants with multiple screens (e.g. encoder-driven UIs) override this.
#ifndef NUM_SCREENS
  #define NUM_SCREENS 1
#endif

class UITask {
  DisplayDriver* _display;
  unsigned long _next_read, _next_refresh, _auto_off;
  int _prevBtnState;
  uint8_t _current_screen = 0;   // 0 = home, 1..N-1 = additional screens
  NodePrefs* _node_prefs;
  char _version_info[32];

  void renderCurrScreen();
public:
  UITask(DisplayDriver& display) : _display(&display) { _next_read = _next_refresh = 0; }
  void begin(NodePrefs* node_prefs, const char* build_date, const char* firmware_version);

  void loop();
};