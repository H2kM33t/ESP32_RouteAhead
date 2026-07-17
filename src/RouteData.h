#ifndef ROUTE_DATA_H
#define ROUTE_DATA_H

#include <Arduino.h>

// A single route point in metres, relative to the bike, heading-up rotated.
// +Y = straight ahead, +X = right of travel. Matches RouteTransform.kt/LocalPoint on Android.
struct RoutePoint {
  float x;
  float y;
};

const int MAX_ROUTE_POINTS = 12;

// Must match RoutePacketBuilder.ManeuverType enum order on the Android side exactly -
// it's sent over BLE as a raw index, so any mismatch here silently shows the wrong icon.
enum ManeuverType {
  MANEUVER_STRAIGHT,
  MANEUVER_LEFT,
  MANEUVER_RIGHT,
  MANEUVER_SLIGHT_LEFT,
  MANEUVER_SLIGHT_RIGHT,
  MANEUVER_SHARP_LEFT,
  MANEUVER_SHARP_RIGHT,
  MANEUVER_UTURN_RIGHT,
  MANEUVER_ROUNDABOUT
};

// Single shared struct holding "what should currently be shown on screen".
// BleManager writes to this when a new packet arrives; DisplayRenderer reads from it.
// Kept as plain global state (like a shared UART receive buffer) rather than passing
// copies around - simplest option for a single-core render loop like this.
struct NavState {
  RoutePoint route[MAX_ROUTE_POINTS];
  int numPoints = 0;

  ManeuverType maneuver = MANEUVER_STRAIGHT;
  uint16_t distanceToTurnM = 0;
  char distanceLabel[16] = "-- m";

  bool hasData = false; // false until the first valid BLE packet arrives
};

extern NavState navState; // defined once in main.cpp

#endif
