#include "DisplayRenderer.h"

namespace DisplayRenderer {

  // ---- Change this if your OLED is SSD1306 instead of SH1106 ----
  static U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

  // ---------- Layout constants ----------
  static const int SCREEN_W = 128;
  static const int SCREEN_H = 64;

  static const int ROUTE_AREA_TOP = 0;
  static const int ROUTE_AREA_BOTTOM = 42;
  static const int BOTTOM_BAR_TOP = 44;
  static const int BOTTOM_BAR_H = SCREEN_H - BOTTOM_BAR_TOP;

  static const float MAX_FORWARD_M = 150.0;
  static const float LATERAL_EXAGGERATION = 3.5;

  static const int BIKE_SCREEN_X = SCREEN_W / 2;
  static const int BIKE_SCREEN_Y = ROUTE_AREA_BOTTOM - 2;

  static const int ICON_CENTER_X = 20;
  static const int ICON_CENTER_Y = BOTTOM_BAR_TOP + BOTTOM_BAR_H / 2;
  static const int ICON_SIZE = 16;

  // ---------- Internal helpers ----------
  static void projectPoint(RoutePoint p, int &screenX, int &screenY) {
    float pixelsPerMetreY = (float)(ROUTE_AREA_BOTTOM - ROUTE_AREA_TOP - 4) / MAX_FORWARD_M;
    float scaledX = p.x * LATERAL_EXAGGERATION;
    screenX = BIKE_SCREEN_X + (int)(scaledX * pixelsPerMetreY);
    screenY = BIKE_SCREEN_Y - (int)(p.y * pixelsPerMetreY);
  }

  static void drawRouteAhead() {
    if (navState.numPoints < 2) return;

    int prevX, prevY;
    projectPoint(navState.route[0], prevX, prevY);

    for (int i = 1; i < navState.numPoints; i++) {
      int x, y;
      projectPoint(navState.route[i], x, y);
      if (y < ROUTE_AREA_TOP) break;
      u8g2.drawLine(prevX, prevY, x, y);
      prevX = x;
      prevY = y;
    }
  }

  static void drawBikeMarker() {
    u8g2.drawTriangle(
      BIKE_SCREEN_X - 4, BIKE_SCREEN_Y + 4,
      BIKE_SCREEN_X + 4, BIKE_SCREEN_Y + 4,
      BIKE_SCREEN_X,     BIKE_SCREEN_Y - 4
    );
  }

  static void drawArrowIcon(ManeuverType type) {
    int cx = ICON_CENTER_X;
    int cy = ICON_CENTER_Y;
    int s = ICON_SIZE / 2;

    switch (type) {
      case MANEUVER_STRAIGHT:
        u8g2.drawLine(cx, cy + s, cx, cy - s);
        u8g2.drawTriangle(cx - 4, cy - s + 5, cx + 4, cy - s + 5, cx, cy - s - 3);
        break;

      case MANEUVER_LEFT:
      case MANEUVER_SLIGHT_LEFT:
      case MANEUVER_SHARP_LEFT:
        u8g2.drawLine(cx + s, cy + s, cx, cy);
        u8g2.drawLine(cx, cy, cx - s, cy - 2);
        u8g2.drawTriangle(cx - s + 6, cy - 6, cx - s + 6, cy + 2, cx - s - 2, cy - 2);
        break;

      case MANEUVER_RIGHT:
      case MANEUVER_SLIGHT_RIGHT:
      case MANEUVER_SHARP_RIGHT:
        u8g2.drawLine(cx - s, cy + s, cx, cy);
        u8g2.drawLine(cx, cy, cx + s, cy - 2);
        u8g2.drawTriangle(cx + s - 6, cy - 6, cx + s - 6, cy + 2, cx + s + 2, cy - 2);
        break;

      case MANEUVER_UTURN_RIGHT:
        u8g2.drawLine(cx - 4, cy + s, cx - 4, cy - 2);
        u8g2.drawCircle(cx, cy - 2, 4);
        u8g2.drawTriangle(cx + 4, cy - 6, cx + 4, cy + 2, cx + 10, cy - 2);
        break;

      case MANEUVER_ROUNDABOUT:
        u8g2.drawCircle(cx, cy, s);
        u8g2.drawLine(cx, cy - s, cx, cy - s - 5);
        u8g2.drawTriangle(cx - 3, cy - s - 3, cx + 3, cy - s - 3, cx, cy - s - 8);
        break;
    }
  }

  static void drawDistanceText() {
    u8g2.setFont(u8g2_font_helvB12_tr);
    int x = ICON_CENTER_X + ICON_SIZE + 10;
    int y = BOTTOM_BAR_TOP + BOTTOM_BAR_H / 2 + 5;
    u8g2.drawStr(x, y, navState.distanceLabel);
  }

  static void drawDivider() {
    u8g2.drawHLine(0, BOTTOM_BAR_TOP - 2, SCREEN_W);
  }

  static void drawWaitingScreen() {
    u8g2.setFont(u8g2_font_helvB10_tr);
    const char* text = "Waiting for app...";
    int w = u8g2.getStrWidth(text);
    u8g2.drawStr((SCREEN_W - w) / 2, SCREEN_H / 2, text);
  }

  // ---------- Public API ----------
  void begin() {
    u8g2.begin();
  }

  void render() {
    u8g2.clearBuffer();

    if (!navState.hasData) {
      drawWaitingScreen();
    } else {
      drawRouteAhead();
      drawBikeMarker();
      drawDivider();
      drawArrowIcon(navState.maneuver);
      drawDistanceText();
    }

    u8g2.sendBuffer();
  }

}
