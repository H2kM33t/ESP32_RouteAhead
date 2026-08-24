#include <Arduino.h>
#include "RouteData.h"
#include "DisplayRenderer.h"
#include "BleManager.h"

// Single shared nav state - declared extern in RouteData.h, written by BleManager
// from the NimBLE host task, read by DisplayRenderer from loop().
NavState navState;
SemaphoreHandle_t navStateMutex = NULL;

#if PANEL_PROBE

#include "PanelProbe.h"

void setup() {
  Serial.begin(115200);
  delay(2500);
  Serial.println("RouteAhead panel probe");
}

void loop() { PanelProbe::sweep(); }

#elif PIN_SCAN

#include "PinScan.h"

void setup() {
  Serial.begin(115200);
  delay(2500);
  Serial.println("RouteAhead pin scanner");
}

void loop() {
  // Backlight pin is itself a guess, so the scanner drives the current one high and
  // also leaves it high between candidates.
  PinScan::sweep(TFT_BL);
}

#elif DISPLAY_TEST

// ---------------------------------------------------------------------------
// Bring-up diagnostic. Build with: pio run -e display-test -t upload
//
// A blank or wrong-looking panel has several plausible causes needing different
// fixes, so this separates them rather than making you guess. Each colour band is
// LABELLED with the colour it is meant to be, so the screen diagnoses itself.
// ---------------------------------------------------------------------------

#include "Display.h"
static LGFX testPanel;

void setup() {
  Serial.begin(115200);
  // The C3 talks over native USB, which re-enumerates on reset. Without a moment to
  // settle, everything printed here is lost before the host reopens the port.
  delay(2500);

  Serial.println();
  Serial.println("=== RouteAhead display bring-up test ===");
  Serial.printf("Pins: SCLK=%d MOSI=%d CS=%d DC=%d RST=%d BL=%d\n",
                TFT_SCLK, TFT_MOSI, TFT_CS, TFT_DC, TFT_RST, TFT_BL);
  Serial.printf("Free heap: %u bytes\n", (unsigned)ESP.getFreeHeap());

  testPanel.init();
  testPanel.setRotation(1);
  testPanel.setBrightness(255);
  Serial.printf("Panel reports: %d x %d\n", testPanel.width(), testPanel.height());

  Serial.println();
  Serial.println("The test loops. What to look for:");
  Serial.println("  band says RED but looks BLUE    -> rgb_order needs flipping");
  Serial.println("  everything looks photo-negative -> invert needs flipping");
  Serial.println("  white frame missing on an edge  -> offset_x / offset_y wrong");
}

/** Three labelled bands. Reading them settles the colour order without guesswork. */
static void colourBands() {
  const int w = testPanel.width();
  const int h = testPanel.height();
  const int band = h / 3;

  struct { const char* name; uint16_t colour; } bands[] = {
    { "RED",   0xF800 },
    { "GREEN", 0x07E0 },
    { "BLUE",  0x001F },
  };

  testPanel.setFont(&fonts::Font4);
  testPanel.setTextDatum(MC_DATUM);

  for (int i = 0; i < 3; i++) {
    int y = i * band;
    int tall = (i == 2) ? (h - y) : band;
    testPanel.fillRect(0, y, w, tall, bands[i].colour);
    // Label in white on the band it names. If the band reading "RED" looks blue, the
    // panel's colour order is swapped - no interpretation needed.
    testPanel.setTextColor(0xFFFF);
    testPanel.drawString(bands[i].name, w / 2, y + tall / 2);
  }

  // A one-pixel white frame: if an edge is missing, or sits inside a coloured strip,
  // the panel's RAM offsets are wrong.
  testPanel.drawRect(0, 0, w, h, 0xFFFF);
}

void loop() {
  Serial.println("PHASE 1: blinking backlight 4x - watch for brightness changing");
  pinMode(TFT_BL, OUTPUT);
  for (int i = 0; i < 4; i++) {
    digitalWrite(TFT_BL, LOW);  delay(350);
    digitalWrite(TFT_BL, HIGH); delay(350);
  }
  testPanel.setBrightness(255);

  Serial.println("PHASE 2: labelled colour bands (8s) - do labels match the colours?");
  colourBands();
  delay(8000);

  Serial.println("PHASE 3: white frame + diagonals (5s) - are all four edges visible?");
  testPanel.fillScreen(0x0000);
  testPanel.drawRect(0, 0, testPanel.width(), testPanel.height(), 0xFFFF);
  testPanel.drawLine(0, 0, testPanel.width() - 1, testPanel.height() - 1, 0x07E0);
  testPanel.drawLine(testPanel.width() - 1, 0, 0, testPanel.height() - 1, 0x07E0);
  testPanel.setFont(&fonts::Font4);
  testPanel.setTextDatum(MC_DATUM);
  testPanel.setTextColor(0xFFFF, 0x0000);
  testPanel.drawString("RouteAhead", testPanel.width() / 2, testPanel.height() / 2);
  delay(5000);
}

#else

void setup() {
  Serial.begin(115200);

  // Created before anything can touch navState. navStateLock() no-ops if this is
  // still NULL, so ordering matters: BLE must not start before the mutex exists.
  navStateMutex = xSemaphoreCreateMutex();

  DisplayRenderer::begin();
  BleManager::begin();

  Serial.println("RouteAhead ready, advertising as BikeNav-RouteAhead");
}

void loop() {
  DisplayRenderer::render();
  delay(100); // ~10 fps; BLE writes update navState asynchronously in the background
}

#endif
