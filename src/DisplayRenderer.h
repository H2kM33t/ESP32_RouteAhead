#ifndef DISPLAY_RENDERER_H
#define DISPLAY_RENDERER_H

#include "Display.h"
#include "RouteData.h"

namespace DisplayRenderer {

  // Call once in setup() - brings up the ST7735 over SPI and allocates the framebuffer.
  void begin();

  // Call every loop() iteration - redraws the whole screen from current navState.
  void render();

}

#endif
