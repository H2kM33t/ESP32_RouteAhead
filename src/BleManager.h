#ifndef BLE_MANAGER_H
#define BLE_MANAGER_H

#include "RouteData.h"

namespace BleManager {

  // Call once in setup() - initializes NimBLE, creates service/characteristic, starts advertising.
  void begin();

  // True while a phone is connected. Useful if you want to show a "connected" indicator later.
  bool isConnected();

}

#endif
