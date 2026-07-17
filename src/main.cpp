#include <Arduino.h>
#include "RouteData.h"
#include "DisplayRenderer.h"
#include "BleManager.h"

// Single shared nav state - defined here, declared extern in RouteData.h,
// written by BleManager, read by DisplayRenderer.
NavState navState;

void setup() {
  Serial.begin(115200);

  DisplayRenderer::begin();
  BleManager::begin();
}

void loop() {
  DisplayRenderer::render();
  delay(100); // redraw rate; fine since BLE writes update navState asynchronously in the background
}
