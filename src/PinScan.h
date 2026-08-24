#ifndef PIN_SCAN_H
#define PIN_SCAN_H

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

/**
 * Finds the panel wiring by trying known-plausible pin sets one at a time.
 *
 * For an unknown board this beats guessing: each candidate gets configured, initialised
 * and told to paint the whole screen a bright colour with a big number on it. Watch the
 * panel, note the number that appears, and that candidate's pins are the right ones.
 *
 * Only the ESP32-C3's usable GPIOs appear here. 11-17 are the SPI flash, 18/19 are the
 * native USB pair, and 20/21 are the UART - wiring a display to any of those would stop
 * the board booting or talking, so they are deliberately excluded.
 */
namespace PinScan {

  struct Candidate {
    int8_t sclk, mosi, cs, dc, rst;
    const char* note;
  };

  // Ordered most-likely-first. The first entry is what the firmware currently assumes.
  static const Candidate CANDIDATES[] = {
    { 4,  6,  7,  5, 10, "current default" },
    { 2,  3,  7,  6, 10, "common C3 SuperMini wiring" },
    { 4,  6, 10,  7,  3, "CS/DC/RST rotated" },
    { 6,  7,  5,  4, 10, "SPI on 6/7" },
    { 1,  0,  4,  3,  2, "low GPIO block" },
    { 5,  4,  7,  6, 10, "SCLK/MOSI swapped from default" },
    { 2,  7,  6,  3, 10, "mixed" },
    { 8,  9,  7,  6, 10, "strapping pins 8/9 (works once booted)" },
    { 4,  6,  7,  5, -1, "default, RST tied to 3V3" },
    { 4,  6, -1,  5, 10, "default, CS tied to GND" },
    { 0,  1,  3,  2, 10, "0/1 SPI" },
    { 7,  6,  5,  4, 10, "reverse of default" },
  };
  static const int CANDIDATE_COUNT = sizeof(CANDIDATES) / sizeof(CANDIDATES[0]);

  /** A panel whose wiring can be reconfigured between attempts. */
  class ScanPanel : public lgfx::LGFX_Device {
    lgfx::Panel_ST7735S _panel;
    lgfx::Bus_SPI       _bus;

  public:
    void configure(const Candidate& c, int backlightPin) {
      {
        auto cfg = _bus.config();
        cfg.spi_host    = SPI2_HOST;   // the C3's only general-purpose SPI
        cfg.spi_mode    = 0;
        // Deliberately slow while scanning: a marginal or long flying lead can work at
        // 8 MHz and fail at 27, and a false negative here would send us hunting the
        // wrong pins entirely.
        cfg.freq_write  = 8000000;
        cfg.freq_read   = 8000000;
        cfg.spi_3wire   = true;
        cfg.use_lock    = true;
        cfg.dma_channel = SPI_DMA_CH_AUTO;
        cfg.pin_sclk    = c.sclk;
        cfg.pin_mosi    = c.mosi;
        cfg.pin_miso    = -1;
        cfg.pin_dc      = c.dc;
        _bus.config(cfg);
        _panel.setBus(&_bus);
      }
      {
        auto cfg = _panel.config();
        cfg.pin_cs        = c.cs;
        cfg.pin_rst       = c.rst;
        cfg.pin_busy      = -1;
        cfg.panel_width   = 128;
        cfg.panel_height  = 160;
        cfg.memory_width  = 128;
        cfg.memory_height = 160;
        cfg.offset_x      = 0;
        cfg.offset_y      = 0;
        cfg.readable      = false;
        cfg.invert        = false;
        cfg.rgb_order     = false;
        cfg.bus_shared    = false;
        _panel.config(cfg);
      }
      setPanel(&_panel);

      // Backlight is driven as a plain GPIO here rather than through Light_PWM, so a
      // wrong backlight pin cannot mask an otherwise-correct SPI candidate.
      if (backlightPin >= 0) {
        pinMode(backlightPin, OUTPUT);
        digitalWrite(backlightPin, HIGH);
      }
    }
  };

  static ScanPanel panel;

  /** Runs one full sweep, holding each candidate on screen for holdMs. */
  inline void sweep(int backlightPin, uint32_t holdMs = 3000) {
    Serial.println();
    Serial.println("=== PIN SCAN ===");
    Serial.println("Watch the panel. When a NUMBER appears on a coloured screen,");
    Serial.println("note it - that candidate's pins are your wiring.");
    Serial.println();

    // Alternating hues so two consecutive hits are still distinguishable.
    const uint16_t hues[] = { 0xF800, 0x07E0, 0xFFE0, 0x07FF, 0xF81F, 0xFFFF };

    for (int i = 0; i < CANDIDATE_COUNT; i++) {
      const Candidate& c = CANDIDATES[i];
      Serial.printf("[%2d] SCLK=%-3d MOSI=%-3d CS=%-3d DC=%-3d RST=%-3d  %s\n",
                    i + 1, c.sclk, c.mosi, c.cs, c.dc, c.rst, c.note);

      panel.configure(c, backlightPin);
      panel.init();
      panel.setRotation(1);

      panel.fillScreen(hues[i % 6]);
      panel.setFont(&fonts::Font7);   // seven-segment style, legible across a room
      panel.setTextDatum(MC_DATUM);
      panel.setTextColor(0x0000);
      panel.drawNumber(i + 1, panel.width() / 2, panel.height() / 2);

      delay(holdMs);
    }

    Serial.println();
    Serial.println("Sweep complete - repeating. Ctrl-C when you have the number.");
  }
}

#endif
