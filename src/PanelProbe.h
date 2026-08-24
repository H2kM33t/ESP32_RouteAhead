#ifndef PANEL_PROBE_H
#define PANEL_PROBE_H

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

/**
 * Tries the CURRENT pin assignment against several panel/bus configurations.
 *
 * Separate from PinScan on purpose: a dead panel is either the wrong pins or the wrong
 * bus settings, and sweeping both at once makes a hit impossible to attribute. This
 * assumes the pins in platformio.ini are right and varies everything else.
 *
 * Each variant STROBES the screen black/white several times before holding a colour.
 * A strobe is far easier to catch out of the corner of your eye than a number you have
 * to be looking directly at when it appears.
 */
namespace PanelProbe {

  struct Variant {
    bool     threeWire;
    bool     useDma;
    uint32_t freq;
    bool     invert;
    bool     st7735s;      // false = plain ST7735 command set
    const char* note;
  };

  static const Variant VARIANTS[] = {
    { true,  true,   27000000, false, true,  "current settings" },
    { false, true,   27000000, false, true,  "3-wire off" },
    { true,  false,  27000000, false, true,  "DMA off" },
    { false, false,   8000000, false, true,  "3-wire off, DMA off, 8 MHz" },
    { false, false,   4000000, false, true,  "very slow 4 MHz" },
    { false, false,   8000000, true,  true,  "8 MHz, inverted" },
    { false, false,   8000000, false, false, "plain ST7735 command set" },
    { true,  false,   4000000, false, false, "plain ST7735, 3-wire, 4 MHz" },
  };
  static const int VARIANT_COUNT = sizeof(VARIANTS) / sizeof(VARIANTS[0]);

  class ProbePanel : public lgfx::LGFX_Device {
    lgfx::Panel_ST7735S _s;
    lgfx::Panel_ST7735  _plain;
    lgfx::Bus_SPI       _bus;

  public:
    bool apply(const Variant& v) {
      lgfx::Panel_Device* panel = v.st7735s
        ? static_cast<lgfx::Panel_Device*>(&_s)
        : static_cast<lgfx::Panel_Device*>(&_plain);

      {
        auto cfg = _bus.config();
        cfg.spi_host    = SPI2_HOST;
        cfg.spi_mode    = 0;
        cfg.freq_write  = v.freq;
        cfg.freq_read   = 4000000;
        cfg.spi_3wire   = v.threeWire;
        cfg.use_lock    = true;
        cfg.dma_channel = v.useDma ? SPI_DMA_CH_AUTO : 0;
        cfg.pin_sclk    = TFT_SCLK;
        cfg.pin_mosi    = TFT_MOSI;
        cfg.pin_miso    = -1;
        cfg.pin_dc      = TFT_DC;
        _bus.config(cfg);
        panel->setBus(&_bus);
      }
      {
        auto cfg = panel->config();
        cfg.pin_cs        = TFT_CS;
        cfg.pin_rst       = TFT_RST;
        cfg.pin_busy      = -1;
        cfg.panel_width   = 128;
        cfg.panel_height  = 160;
        cfg.memory_width  = 128;
        cfg.memory_height = 160;
        cfg.offset_x      = 0;
        cfg.offset_y      = 0;
        cfg.readable      = false;
        cfg.invert        = v.invert;
        cfg.rgb_order     = false;
        cfg.bus_shared    = false;
        panel->config(cfg);
      }
      setPanel(panel);

      // Backlight as a plain GPIO so a PWM misconfiguration cannot mask a working bus.
      pinMode(TFT_BL, OUTPUT);
      digitalWrite(TFT_BL, HIGH);

      // init() returns false when the panel cannot be brought up. Never checked before,
      // which meant a failed init looked exactly like a wiring fault.
      return init();
    }
  };

  static ProbePanel panel;

  inline void sweep() {
    Serial.println();
    Serial.println("=== PANEL PROBE ===");
    Serial.printf("Pins held fixed: SCLK=%d MOSI=%d CS=%d DC=%d RST=%d BL=%d\n",
                  TFT_SCLK, TFT_MOSI, TFT_CS, TFT_DC, TFT_RST, TFT_BL);
    Serial.println("Watch for the screen FLASHING. Note the variant number if it does.");
    Serial.println();

    for (int i = 0; i < VARIANT_COUNT; i++) {
      const Variant& v = VARIANTS[i];
      bool ok = panel.apply(v);

      Serial.printf("[%d] %-32s init()=%s  size=%dx%d\n",
                    i + 1, v.note, ok ? "OK" : "FAILED",
                    panel.width(), panel.height());

      panel.setRotation(1);

      // Hard strobe - impossible to miss even in peripheral vision.
      for (int k = 0; k < 6; k++) {
        panel.fillScreen(0xFFFF); delay(120);
        panel.fillScreen(0x0000); delay(120);
      }

      panel.fillScreen(0x07E0); // green hold
      panel.setFont(&fonts::Font7);
      panel.setTextDatum(MC_DATUM);
      panel.setTextColor(0x0000);
      panel.drawNumber(i + 1, panel.width() / 2, panel.height() / 2);
      delay(2500);
    }

    Serial.println();
    Serial.println("Sweep complete - repeating.");
  }
}

#endif
