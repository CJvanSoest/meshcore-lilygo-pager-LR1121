#pragma once

#define RADIOLIB_STATIC_ONLY 1
#include <RadioLib.h>
#include <helpers/ESP32Board.h>
#include <helpers/AutoDiscoverRTCClock.h>
#include <helpers/sensors/EnvironmentSensorManager.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#include <helpers/radiolib/CustomLR1110.h>
#include <helpers/radiolib/CustomLR1110Wrapper.h>

#ifdef DISPLAY_CLASS
  #include "TPagerST7796Display.h"
  #include <helpers/ui/MomentaryButton.h>
#endif

class TLoraPagerBoard : public ESP32Board {
public:
  void begin();
  uint16_t getBattMilliVolts() override;
  const char* getManufacturerName() const override { return "LilyGo"; }
};

extern TLoraPagerBoard board;
extern WRAPPER_CLASS radio_driver;
extern AutoDiscoverRTCClock rtc_clock;
extern EnvironmentSensorManager sensors;
#ifdef DISPLAY_CLASS
  extern DISPLAY_CLASS display;
  extern MomentaryButton user_btn;
#endif

bool radio_init();
uint32_t radio_get_rng_seed();
void radio_set_params(float freq, float bw, uint8_t sf, uint8_t cr);
void radio_set_tx_power(int8_t dbm);
mesh::LocalIdentity radio_new_identity();
