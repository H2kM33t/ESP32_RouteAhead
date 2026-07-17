#include "BleManager.h"
#include <NimBLEDevice.h>

namespace BleManager {

  // These UUIDs must match BleRouteClient.kt on the Android side exactly.
  static const char* SERVICE_UUID    = "4fafc201-1fb5-459e-8fcc-c5c9c331914b";
  static const char* ROUTE_CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8";
  static const char* DEVICE_NAME     = "BikeNav-RouteAhead"; // must match BleRouteClient.DEVICE_NAME

  static NimBLEServer* server = nullptr;
  static NimBLECharacteristic* routeCharacteristic = nullptr;
  static volatile bool connected = false;

  // ---------- Packet parsing ----------
  // Layout (little-endian), matches RoutePacketBuilder.kt:
  //   Byte 0:      maneuverType
  //   Byte 1-2:    distanceToTurn (uint16)
  //   Byte 3:      numPoints
  //   Byte 4+:     per point: x(int16 cm), y(int16 cm)
  static void parsePacket(const uint8_t* data, size_t length) {
    if (length < 4) return; // malformed, ignore rather than crash

    uint8_t maneuverByte = data[0];
    uint16_t distance = data[1] | (data[2] << 8);
    uint8_t numPoints = data[3];

    if (maneuverByte > MANEUVER_ROUNDABOUT) maneuverByte = MANEUVER_STRAIGHT;
    if (numPoints > MAX_ROUTE_POINTS) numPoints = MAX_ROUTE_POINTS;

    size_t expectedLen = 4 + (size_t)numPoints * 4;
    if (length < expectedLen) return; // truncated packet, ignore

    navState.maneuver = (ManeuverType)maneuverByte;
    navState.distanceToTurnM = distance;
    navState.numPoints = numPoints;

    for (int i = 0; i < numPoints; i++) {
      int offset = 4 + i * 4;
      int16_t xCm = data[offset] | (data[offset + 1] << 8);
      int16_t yCm = data[offset + 2] | (data[offset + 3] << 8);
      navState.route[i].x = xCm / 100.0f;
      navState.route[i].y = yCm / 100.0f;
    }

    if (navState.distanceToTurnM < 1000) {
      snprintf(navState.distanceLabel, sizeof(navState.distanceLabel), "%d m", navState.distanceToTurnM);
    } else {
      snprintf(navState.distanceLabel, sizeof(navState.distanceLabel), "%.1f km", navState.distanceToTurnM / 1000.0f);
    }

    navState.hasData = true;
  }

  // ---------- NimBLE callbacks ----------
  // NOTE: NimBLE-Arduino 2.x callback signatures all take an extra NimBLEConnInfo&
  // parameter compared to 1.x, and onDisconnect also gets a reason code. If you
  // later downgrade to 1.x to match your existing BikeNavApp firmware, these
  // signatures need to drop the NimBLEConnInfo& params again.
  class RouteCharacteristicCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
      std::string value = pCharacteristic->getValue();
      parsePacket((const uint8_t*)value.data(), value.length());
    }
  };

  class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
      connected = true;
    }
    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
      connected = false;
      navState.hasData = false; // show "waiting" screen again until fresh data arrives
      NimBLEDevice::startAdvertising(); // resume advertising so the app can reconnect
    }
  };

  // ---------- Public API ----------
  void begin() {
    NimBLEDevice::init(DEVICE_NAME);

    server = NimBLEDevice::createServer();
    server->setCallbacks(new ServerCallbacks());

    NimBLEService* service = server->createService(SERVICE_UUID);

    routeCharacteristic = service->createCharacteristic(
      ROUTE_CHAR_UUID,
      NIMBLE_PROPERTY::WRITE
    );
    routeCharacteristic->setCallbacks(new RouteCharacteristicCallbacks());

    server->start(); // in NimBLE 2.x this also starts all services registered on it - service->start() is a no-op now

    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    advertising->addServiceUUID(SERVICE_UUID);
    advertising->enableScanResponse(true); // needed so the 128-bit UUID doesn't get dropped from the packet
    NimBLEDevice::startAdvertising();
  }

  bool isConnected() {
    return connected;
  }

}
