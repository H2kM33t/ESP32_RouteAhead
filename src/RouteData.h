#ifndef ROUTE_DATA_H
#define ROUTE_DATA_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Wire protocol version this firmware speaks. Must match NavPacket.PROTOCOL_VERSION
// on the Android side. Frames tagged with anything else are dropped rather than
// guessed at - a misread frame shows the rider a confidently wrong turn.
#define NAV_PROTOCOL_VERSION 4

// A single route point in metres, relative to the bike, heading-up rotated.
// +Y = straight ahead, +X = right of travel. Matches LocalPoint on Android.
struct RoutePoint {
  float x;
  float y;
};

// Must equal NavPacket.MAX_ROUTE_POINTS on the Android side. 24 keeps the drawn
// route smooth enough to read as a ribbon; the array costs 24 * 8 = 192 bytes.
const int MAX_ROUTE_POINTS = 20;
const int MAX_STREET_NAME = 22;

// Places sent alongside the route so the rider can recognise where they are, rather
// than reading a bare line. Six is what fits the frame budget alongside 24 route
// points while staying inside the negotiated MTU.
const int MAX_LANDMARKS = 5;

// Stubs of the roads that branch off the route, so the ribbon sits in a road network
// rather than floating in black. Six is enough to show every arm of a normal
// junction plus a couple of side streets either side of it.
const int MAX_BRANCHES = 6;

// Must match Landmark.Type ordinals in the Android app.
enum LandmarkType {
  LANDMARK_PLACE,
  LANDMARK_FUEL,
  LANDMARK_FOOD,
  LANDMARK_HOSPITAL,
  LANDMARK_ATM,
  LANDMARK_JUNCTION,
  LANDMARK_TYPE_COUNT
};

// Same bike-relative frame as RoutePoint: +Y ahead, +X right, metres.
struct Landmark {
  float x;
  float y;
  uint8_t type;
};

// A neighbouring road, drawn as a short stub leaving (x, y).
//
// Sent as an anchor plus a direction rather than a polyline: a side road only needs to
// show WHERE it leaves the route and WHICH WAY it goes, and one point plus an angle
// costs 6 bytes against the 8 a two-point line would need - which is the difference
// between fitting six of them in the frame and fitting four.
struct Branch {
  float x;
  float y;
  float headingDeg;  // 0 = straight ahead, clockwise, 0..358
  float lengthM;
};

// Must match the Maneuver enum order in Route.kt exactly - it's sent over BLE as a
// raw ordinal, so any mismatch here silently shows the wrong icon. Append only.
enum ManeuverType {
  MANEUVER_STRAIGHT,
  MANEUVER_LEFT,
  MANEUVER_RIGHT,
  MANEUVER_SLIGHT_LEFT,
  MANEUVER_SLIGHT_RIGHT,
  MANEUVER_SHARP_LEFT,
  MANEUVER_SHARP_RIGHT,
  MANEUVER_UTURN,
  MANEUVER_ROUNDABOUT,
  MANEUVER_MERGE,
  MANEUVER_FORK_LEFT,
  MANEUVER_FORK_RIGHT,
  MANEUVER_DEPART,
  MANEUVER_ARRIVE,
  MANEUVER_ON_RAMP,
  MANEUVER_OFF_RAMP,
  MANEUVER_COUNT
};

// Flag bits in byte 1 of the frame.
#define NAV_FLAG_HAS_ROUTE  (1 << 0)
#define NAV_FLAG_OFF_ROUTE  (1 << 1)
#define NAV_FLAG_ARRIVED    (1 << 2)
#define NAV_FLAG_REROUTING  (1 << 3)

// "What should currently be on screen."
//
// BleManager writes this from the NimBLE host task; DisplayRenderer reads it from
// loop(). Those are different FreeRTOS tasks on different priorities, so access is
// guarded by navStateMutex - without it the renderer can read numPoints from a new
// frame while the coordinates are still from the old one, which draws a polyline
// with a spike through it.
struct NavState {
  RoutePoint route[MAX_ROUTE_POINTS];
  int numPoints = 0;

  ManeuverType maneuver = MANEUVER_STRAIGHT;
  uint16_t distanceToTurnM = 0;
  uint16_t speedKmhX10 = 0;
  uint16_t etaSeconds = 0;
  uint32_t remainingDistanceM = 0;
  char streetName[MAX_STREET_NAME + 1] = "";

  Landmark landmarks[MAX_LANDMARKS];
  int numLandmarks = 0;

  Branch branches[MAX_BRANCHES];
  int numBranches = 0;

  bool hasRoute = false;
  bool offRoute = false;
  bool arrived = false;
  bool rerouting = false;

  bool connected = false;         // a phone is attached
  uint32_t lastPacketMs = 0;      // millis() of the last valid frame, for staleness
};

extern NavState navState;              // defined once in main.cpp
extern SemaphoreHandle_t navStateMutex;

// Take/give the lock around every navState access. Held for microseconds, so a
// generous timeout here only ever matters if something has gone badly wrong.
inline bool navStateLock(TickType_t timeoutTicks = pdMS_TO_TICKS(50)) {
  return navStateMutex != NULL && xSemaphoreTake(navStateMutex, timeoutTicks) == pdTRUE;
}

inline void navStateUnlock() {
  if (navStateMutex != NULL) xSemaphoreGive(navStateMutex);
}

#endif
