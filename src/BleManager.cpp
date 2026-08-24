#include "BleManager.h"
#include <NimBLEDevice.h>
#include <string.h>

namespace BleManager {

  // These UUIDs must match BleRouteClient.kt on the Android side exactly.
  static const char* SERVICE_UUID    = "4fafc201-1fb5-459e-8fcc-c5c9c331914b";
  static const char* ROUTE_CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8";
  static const char* DEVICE_NAME     = "BikeNav-RouteAhead"; // must match BleRouteClient.DEVICE_NAME

  // Set to 1 to print every decoded frame to the serial monitor at 115200 baud.
  // The quickest way to tell whether the phone is actually reaching the device, and
  // whether the numbers it sends match what the app is showing on screen.
  #define NAV_DEBUG 1

  static NimBLEServer* server = nullptr;
  static NimBLECharacteristic* routeCharacteristic = nullptr;

#if NAV_DEBUG
  static const char* maneuverName(uint8_t m) {
    static const char* names[] = {
      "STRAIGHT", "LEFT", "RIGHT", "SLIGHT_LEFT", "SLIGHT_RIGHT",
      "SHARP_LEFT", "SHARP_RIGHT", "UTURN", "ROUNDABOUT", "MERGE",
      "FORK_LEFT", "FORK_RIGHT", "DEPART", "ARRIVE", "ON_RAMP", "OFF_RAMP"
    };
    return m < MANEUVER_COUNT ? names[m] : "?";
  }
#endif

  // ---------- Packet parsing ----------
  // Frame layout (little-endian), mirrors NavPacket.kt:
  //   0      version (must be NAV_PROTOCOL_VERSION)
  //   1      flags
  //   2      maneuver ordinal
  //   3-4    uint16 distance to maneuver, metres
  //   5-6    uint16 speed, 0.1 km/h units
  //   7-8    uint16 ETA, seconds
  //   9-10   uint16 remaining distance, decametres
  //   11     numPoints
  //   12..   numPoints * { int16 x cm, int16 y cm }
  //   then   uint8 numLandmarks, then numLandmarks * { int16 x cm, int16 y cm, uint8 type }
  //   then   uint8 numBranches,  then numBranches  * { int16 x cm, int16 y cm,
  //                                                    uint8 heading/2 deg, uint8 length/2 m }
  //   then   uint8 street name length, then that many UTF-8 bytes
  static const size_t HEADER_LEN = 12;

  static bool parsePacket(const uint8_t* data, size_t length) {
    if (length < HEADER_LEN) return false;       // too short to be a frame at all
    if (data[0] != NAV_PROTOCOL_VERSION) return false; // app and firmware are out of step

    uint8_t flags     = data[1];
    uint8_t maneuver  = data[2];
    uint16_t distance = (uint16_t)data[3] | ((uint16_t)data[4] << 8);
    uint16_t speed    = (uint16_t)data[5] | ((uint16_t)data[6] << 8);
    uint16_t eta      = (uint16_t)data[7] | ((uint16_t)data[8] << 8);
    uint16_t remDam   = (uint16_t)data[9] | ((uint16_t)data[10] << 8);
    uint8_t numPoints = data[11];

    if (maneuver >= MANEUVER_COUNT) maneuver = MANEUVER_STRAIGHT;
    if (numPoints > MAX_ROUTE_POINTS) numPoints = MAX_ROUTE_POINTS;

    // Validate the whole frame BEFORE touching navState, so a truncated write can
    // never leave the display showing half of one frame and half of another.
    size_t pointsEnd = HEADER_LEN + (size_t)numPoints * 4;
    if (length < pointsEnd + 1) return false;

    // v3 inserts the landmark block between the route points and the street name.
    uint8_t numLandmarks = data[pointsEnd];
    if (numLandmarks > MAX_LANDMARKS) numLandmarks = MAX_LANDMARKS;

    size_t landmarksEnd = pointsEnd + 1 + (size_t)numLandmarks * 5;
    if (length < landmarksEnd + 1) return false;

    // v4 adds the neighbouring-road stubs, again ahead of the street name so every
    // variable-length block stays in a fixed order.
    uint8_t numBranches = data[landmarksEnd];
    if (numBranches > MAX_BRANCHES) numBranches = MAX_BRANCHES;

    size_t branchesEnd = landmarksEnd + 1 + (size_t)numBranches * 6;
    if (length < branchesEnd + 1) return false;

    uint8_t nameLen = data[branchesEnd];
    if (nameLen > MAX_STREET_NAME) nameLen = MAX_STREET_NAME;
    if (length < branchesEnd + 1 + nameLen) return false;

    if (!navStateLock()) return true; // renderer is mid-frame; drop this one, another is 1s away

    navState.maneuver          = (ManeuverType)maneuver;
    navState.distanceToTurnM   = distance;
    navState.speedKmhX10       = speed;
    navState.etaSeconds        = eta;
    navState.remainingDistanceM = (uint32_t)remDam * 10;
    navState.numPoints         = numPoints;

    navState.hasRoute  = (flags & NAV_FLAG_HAS_ROUTE) != 0;
    navState.offRoute  = (flags & NAV_FLAG_OFF_ROUTE) != 0;
    navState.arrived   = (flags & NAV_FLAG_ARRIVED) != 0;
    navState.rerouting = (flags & NAV_FLAG_REROUTING) != 0;

    for (uint8_t i = 0; i < numPoints; i++) {
      size_t offset = HEADER_LEN + (size_t)i * 4;
      int16_t xCm = (int16_t)((uint16_t)data[offset]     | ((uint16_t)data[offset + 1] << 8));
      int16_t yCm = (int16_t)((uint16_t)data[offset + 2] | ((uint16_t)data[offset + 3] << 8));
      navState.route[i].x = xCm / 100.0f;
      navState.route[i].y = yCm / 100.0f;
    }

    navState.numLandmarks = numLandmarks;
    for (uint8_t i = 0; i < numLandmarks; i++) {
      size_t o = pointsEnd + 1 + (size_t)i * 5;
      int16_t lx = (int16_t)((uint16_t)data[o]     | ((uint16_t)data[o + 1] << 8));
      int16_t ly = (int16_t)((uint16_t)data[o + 2] | ((uint16_t)data[o + 3] << 8));
      uint8_t type = data[o + 4];
      if (type >= LANDMARK_TYPE_COUNT) type = LANDMARK_PLACE;
      navState.landmarks[i].x = lx / 100.0f;
      navState.landmarks[i].y = ly / 100.0f;
      navState.landmarks[i].type = type;
    }

    navState.numBranches = numBranches;
    for (uint8_t i = 0; i < numBranches; i++) {
      size_t o = landmarksEnd + 1 + (size_t)i * 6;
      int16_t bx = (int16_t)((uint16_t)data[o]     | ((uint16_t)data[o + 1] << 8));
      int16_t by = (int16_t)((uint16_t)data[o + 2] | ((uint16_t)data[o + 3] << 8));
      navState.branches[i].x = bx / 100.0f;
      navState.branches[i].y = by / 100.0f;
      navState.branches[i].headingDeg = data[o + 4] * 2.0f;
      navState.branches[i].lengthM    = data[o + 5] * 2.0f;
    }

    memcpy(navState.streetName, data + branchesEnd + 1, nameLen);
    navState.streetName[nameLen] = '\0';

    navState.lastPacketMs = millis();

    navStateUnlock();

#if NAV_DEBUG
    Serial.printf(
      "[%u B] %-12s in %5u m | %4.1f km/h | %5u m left | ETA %5u s | %2u pts | %u lm | \"%s\"%s\n",
      (unsigned)length, maneuverName(maneuver), distance, speed / 10.0f,
      (unsigned)((uint32_t)remDam * 10), eta, numPoints, numLandmarks,
      navState.streetName,
      (flags & NAV_FLAG_OFF_ROUTE) ? " OFF-ROUTE" : ""
    );
#endif

    return true;
  }

  // Frames that fail validation are dropped silently above, which is correct at runtime
  // but unhelpful when you're trying to work out why nothing appears. With NAV_DEBUG on,
  // this reports them so a version or MTU mismatch is visible rather than invisible.
  static void reportRejected(const uint8_t* data, size_t length) {
#if NAV_DEBUG
    if (length < HEADER_LEN) {
      Serial.printf("REJECTED: %u bytes, need at least %u\n",
                    (unsigned)length, (unsigned)HEADER_LEN);
    } else if (data[0] != NAV_PROTOCOL_VERSION) {
      Serial.printf("REJECTED: protocol v%u, firmware speaks v%u - update one of them\n",
                    data[0], NAV_PROTOCOL_VERSION);
    } else {
      Serial.printf("REJECTED: truncated frame, %u bytes. MTU too small?\n",
                    (unsigned)length);
    }
#endif
  }

  // ---------- NimBLE callbacks ----------
  // NOTE: NimBLE-Arduino 2.x callback signatures all take an extra NimBLEConnInfo&
  // parameter compared to 1.x, and onDisconnect also gets a reason code. If you
  // downgrade to 1.x these signatures need the NimBLEConnInfo& params dropped again.
  class RouteCharacteristicCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
      std::string value = pCharacteristic->getValue();
      const uint8_t* data = (const uint8_t*)value.data();
      if (!parsePacket(data, value.length())) {
        reportRejected(data, value.length());
      }
    }
  };

  class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
      if (navStateLock()) {
        navState.connected = true;
        navStateUnlock();
      }
      // Ask for a faster connection interval than the default. At the default ~50 ms
      // a nav frame can take several intervals to land, which shows up as the turn
      // countdown visibly lagging the road.
      pServer->updateConnParams(connInfo.getConnHandle(), 12, 24, 0, 200);
    }

    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
      if (navStateLock()) {
        navState.connected = false;
        navState.hasRoute = false; // back to the waiting screen until fresh data arrives
        navStateUnlock();
      }
      NimBLEDevice::startAdvertising(); // resume advertising so the app can reconnect
    }

    // Called once the central negotiates a larger MTU. A v4 frame is up to 178 bytes,
    // so this is what makes full frames possible at all - see BleRouteClient.
    void onMTUChange(uint16_t MTU, NimBLEConnInfo& connInfo) override {
      Serial.printf("MTU negotiated: %u bytes\n", MTU);
    }
  };

  // ---------- Public API ----------
  void begin() {
    NimBLEDevice::init(DEVICE_NAME);

    // Matches the 185 the phone asks for, so the stack doesn't cap it lower.
    NimBLEDevice::setMTU(185);

    server = NimBLEDevice::createServer();
    server->setCallbacks(new ServerCallbacks());

    NimBLEService* service = server->createService(SERVICE_UUID);

    routeCharacteristic = service->createCharacteristic(
      ROUTE_CHAR_UUID,
      NIMBLE_PROPERTY::WRITE
    );
    routeCharacteristic->setCallbacks(new RouteCharacteristicCallbacks());

    server->start(); // in NimBLE 2.x this also starts all registered services

    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    advertising->addServiceUUID(SERVICE_UUID);
    advertising->enableScanResponse(true); // so the 128-bit UUID isn't dropped from the packet
    NimBLEDevice::startAdvertising();
  }

  bool isConnected() {
    bool result = false;
    if (navStateLock()) {
      result = navState.connected;
      navStateUnlock();
    }
    return result;
  }

}
