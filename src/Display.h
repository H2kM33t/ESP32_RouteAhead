#ifndef DISPLAY_H
#define DISPLAY_H

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

/**
 * LovyanGFX device config for the 1.8" ST7735 panel on an ESP32-C3.
 *
 * Declared explicitly in the repo rather than relying on an auto-detect header, so the
 * wiring lives with the code. Pin numbers come from build flags in platformio.ini.
 *
 * (This replaced TFT_eSPI, which store-faults on boot on the C3 - see the note in
 * platformio.ini for the details.)
 */
class LGFX : public lgfx::LGFX_Device {

  lgfx::Panel_ST7735S _panel;
  lgfx::Bus_SPI       _bus;
  lgfx::Light_PWM     _light;

public:
  LGFX() {
    {
      auto cfg = _bus.config();
      // The C3 has exactly one general-purpose SPI peripheral usable here: SPI2.
      cfg.spi_host    = SPI2_HOST;
      cfg.spi_mode    = 0;
      cfg.freq_write  = SPI_FREQUENCY;
      cfg.freq_read   = 8000000;
      cfg.spi_3wire   = true;   // no MISO wired on these modules
      cfg.use_lock    = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk    = TFT_SCLK;
      cfg.pin_mosi    = TFT_MOSI;
      cfg.pin_miso    = -1;
      cfg.pin_dc      = TFT_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }

    {
      auto cfg = _panel.config();
      cfg.pin_cs   = TFT_CS;
      cfg.pin_rst  = TFT_RST;
      cfg.pin_busy = -1;

      // Native portrait resolution. setRotation(1) in DisplayRenderer turns this into
      // the 160x128 landscape the layout is designed for.
      cfg.panel_width      = 128;
      cfg.panel_height     = 160;
      cfg.memory_width     = 128;
      cfg.memory_height    = 160;

      // ---- If the picture is shifted or has a coloured edge strip, change these ----
      // ST7735 panels differ in where their visible area sits inside controller RAM,
      // the thing TFT_eSPI called the "tab colour". 0/0 suits most 128x160 modules;
      // some want offset_x = 2, offset_y = 1 (or 1/2). Adjust rather than guessing at
      // a tab name - here it is just two numbers.
      cfg.offset_x         = 0;
      cfg.offset_y         = 0;
      cfg.offset_rotation  = 0;

      cfg.readable         = false;
      // ---- If colours look photo-negative, flip invert. If red and blue are ----
      // ---- swapped, flip rgb_order. These two cover every ST7735 variant.   ----
      cfg.invert           = false;
      // This panel is BGR-ordered: with rgb_order=false, red and blue came out
      // swapped on hardware. true corrects it. If a future panel looks wrong the
      // other way, flip this back.
      cfg.rgb_order        = true;
      cfg.dlen_16bit       = false;
      cfg.bus_shared       = false;
      _panel.config(cfg);
    }

#if TFT_BL >= 0
    {
      // Only attach a backlight driver when one is actually wired to a GPIO. On this
      // board BL goes straight to 3V3, so TFT_BL is -1 and this block compiles out -
      // driving an unrelated pin "as the backlight" is what previously held the panel
      // in reset.
      auto cfg = _light.config();
      cfg.pin_bl      = TFT_BL;
      cfg.invert      = false;
      cfg.freq        = 12000;
      cfg.pwm_channel = 0;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
#endif

    setPanel(&_panel);
  }
};

#endif
