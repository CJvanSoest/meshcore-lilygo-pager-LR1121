#pragma once

#include <helpers/ui/LGFXDisplay.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// LovyanGFX panel configuration for the LilyGo T-Lora Pager's
// ST7796-controlled 222x480 portrait IPS LCD.
//
// The display chip is a Sitronix ST7796 (controller memory 320x480). The
// physical glass is 222 px wide, which means we render with an X offset
// of (320-222)/2 = 49 pixels so the image sits centred on the panel.
//
// The SPI bus is shared with the LR1121 radio and the SD card (see
// pins_arduino.h in LilyGoLib-PlatformIO/variants/lilygo_tlora_pager).
// CS lines are separate so concurrent access works as long as we manage
// chip-select discipline — LovyanGFX raises/lowers DISP_CS automatically.
class LGFX_TPager : public lgfx::LGFX_Device
{
  lgfx::Panel_ST7796 _panel;
  lgfx::Bus_SPI      _bus;
  lgfx::Light_PWM    _light;

public:
  LGFX_TPager()
  {
    {
      auto cfg = _bus.config();
      cfg.spi_host    = SPI3_HOST;
      cfg.spi_mode    = 0;
      cfg.freq_write  = 40000000;
      cfg.freq_read   = 16000000;
      cfg.spi_3wire   = false;
      cfg.use_lock    = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk    = 35;   // DISP_SCK shared with LORA_SCK
      cfg.pin_mosi    = 34;   // DISP_MOSI shared with LORA_MOSI
      cfg.pin_miso    = 33;   // DISP_MISO shared with LORA_MISO
      cfg.pin_dc      = 37;   // DISP_DC
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs           = 38;   // DISP_CS
      cfg.pin_rst          = -1;   // hardware tied
      cfg.pin_busy         = -1;
      cfg.memory_width     = 320;  // controller native
      cfg.memory_height    = 480;
      cfg.panel_width      = 222;  // actual glass
      cfg.panel_height     = 480;
      cfg.offset_x         = 49;   // centre on controller memory
      cfg.offset_y         = 0;
      cfg.offset_rotation  = 2;    // 180° — panel mounted upside-down vs controller orientation
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits  = 1;
      cfg.readable         = false;
      cfg.invert           = true;  // IPS panels typically need invert
      cfg.rgb_order        = false;
      cfg.dlen_16bit       = false;
      cfg.bus_shared       = true; // shared with LR1121 + SD
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl      = 42;    // DISP_BL
      cfg.invert      = false;
      cfg.freq        = 44100;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.light(&_light);
    }
    setPanel(&_panel);
  }
};

// Singleton panel so LVGL and MeshCore's DisplayDriver can both reach it.
extern LGFX_TPager tpager_lgfx_panel;

// LGFXDisplay::begin() applies setRotation(1) which swaps the panel axes —
// combined with our offset_rotation=2 the effective view is landscape
// 480 wide x 222 tall. The sprite dimensions must match that, otherwise
// UI_ZOOM scaling clips off-screen.
class TPagerST7796Display : public LGFXDisplay {
public:
  TPagerST7796Display() : LGFXDisplay(480, 222, tpager_lgfx_panel) {}
};
