#ifndef DISPLAY_RENDERER_H
#define DISPLAY_RENDERER_H

#include <U8g2lib.h>
#include "RouteData.h"

namespace DisplayRenderer {

  // Call once in setup() - initializes the OLED over I2C.
  void begin();

  // Call every loop() iteration - redraws the full screen from current navState.
  void render();

}

#endif
