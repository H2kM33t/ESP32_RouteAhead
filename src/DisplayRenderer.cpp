#include "DisplayRenderer.h"
#include <string.h>
#include <math.h>

namespace DisplayRenderer {

  static LGFX tft;

  // Everything is drawn into an off-screen sprite and pushed in one go. Drawing straight
  // to the panel would show the screen being built piece by piece, which reads as
  // flicker - and makes animation impossible.
  static LGFX_Sprite fb(&tft);
  static bool spriteReady = false;

  // LGFX_Sprite and LGFX share the LovyanGFX base, so the whole renderer draws through
  // one pointer. If the framebuffer cannot be allocated this falls back to the panel:
  // a flickering display is bad, a blank one is useless.
  static LovyanGFX* g = &tft;

  // ---------- Layout ----------
  //
  // Reworked after seeing it on real hardware. The previous version crammed the street
  // name and trip figures into one footer strip that collided with the route area, and
  // put the speed unit before its number.
  //
  //   +----------------------------------------+
  //   |  ^   20 m                              |  turn + distance
  //   |      Lalbaug Gate Rd                   |  street you're turning onto
  //   +----------------------------------------+
  //   |               .                        |
  //   |              /                         |  route ribbon (the hero)
  //   |             A                          |
  //   +----------------------------------------+
  //   |  0 km/h  |  130 m  |  0 min            |  three even cells
  //   +----------------------------------------+
  static const int SCREEN_W = 160;
  static const int SCREEN_H = 128;

  static const int HEADER_H     = 36;
  static const int ROUTE_TOP    = 38;
  static const int ROUTE_BOTTOM = 104;
  static const int FOOTER_TOP   = 107;

  // Where the bike sits inside the map area, as a fraction of its height.
  //
  // Not 0.5: the slice is almost entirely road *ahead*, so centring the geometry left
  // the bike wherever the bounding box happened to put it - usually off to one side,
  // which is what made the old screen look lopsided. Anchoring the bike instead puts it
  // on the horizontal centre line every frame, with the road running up from it the way
  // a map app does.
  static const float BIKE_ANCHOR_FRAC = 0.80f;

  static const int ICON_CX = 17;
  static const int ICON_CY = 17;
  static const int ICON_R  = 14;
  static const int TEXT_X  = 36;

  // ---------- Palette (RGB565) ----------
  static const uint16_t C_BG      = 0x0000;
  static const uint16_t C_OUTLINE = 0x2124;
  static const uint16_t C_ACCENT  = 0x07E8; // green - brand colour, used for the turn
  static const uint16_t C_ROUTE   = 0x24DF; // route blue, the map-app convention
  static const uint16_t C_CASING  = 0x1150; // darker blue under the ribbon
  static const uint16_t C_ROAD    = 0x4A69; // neighbouring roads
  static const uint16_t C_ROADCAS = 0x2124; // their casing, so they read as roads not lines
  static const uint16_t C_TEXT    = 0xFFFF;
  static const uint16_t C_DIM     = 0x8410;
  static const uint16_t C_WARN    = 0xFD20;

  static const uint32_t STALE_AFTER_MS = 6000;

  // ---------- Animation state ----------
  // The renderer runs at ~20 fps and eases values toward their targets rather than
  // snapping. A number that jumps 300 -> 250 -> 200 reads as a glitch at a glance;
  // one that slides reads as movement.
  static uint32_t frame = 0;
  static float shownDistance = 0.0f;   // eased distance-to-turn
  static float pulse = 0.0f;           // 0..1, drives the imminent-turn throb
  static ManeuverType lastManeuver = MANEUVER_STRAIGHT;
  static uint32_t maneuverChangedAt = 0;

  /** Eases current toward target. rate is per-frame fraction, 0..1. */
  static float ease(float current, float target, float rate) {
    return current + (target - current) * rate;
  }

  /** Linear blend between two RGB565 colours. t is 0..1. */
  static uint16_t mix(uint16_t a, uint16_t b, float t) {
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    int r = ar + (int)((br - ar) * t);
    int gg = ag + (int)((bg - ag) * t);
    int bl = ab + (int)((bb - ab) * t);
    return (uint16_t)((r << 11) | (gg << 5) | bl);
  }

  // ---------- Formatting ----------
  // Deliberately identical to formatDistance()/formatDuration() in the Android app; if
  // the two disagree the rider sees one number on the phone and another on the bars.

  static void formatDistance(uint32_t metres, char* out, size_t outLen) {
    if (metres < 1000) {
      uint32_t rounded = metres;
      if (metres >= 200)     rounded = (metres / 50) * 50;
      else if (metres >= 20) rounded = (metres / 10) * 10;
      snprintf(out, outLen, "%lu m", (unsigned long)rounded);
    } else if (metres < 10000) {
      snprintf(out, outLen, "%.1f km", metres / 1000.0f);
    } else {
      snprintf(out, outLen, "%lu km", (unsigned long)(metres / 1000));
    }
  }

  static void formatDuration(uint16_t seconds, char* out, size_t outLen) {
    uint16_t minutes = (seconds + 30) / 60;
    if (minutes < 60) snprintf(out, outLen, "%u min", minutes);
    else              snprintf(out, outLen, "%uh%02u", minutes / 60, minutes % 60);
  }

  // ---------- Primitives ----------

  static void arrowHead(int tipX, int tipY, float angleDeg, int len, int w, uint16_t colour) {
    float a = angleDeg * DEG_TO_RAD;
    float s = sinf(a), c = cosf(a);
    auto rx = [&](float x, float y) { return (int)lroundf(tipX + x * c - y * s); };
    auto ry = [&](float x, float y) { return (int)lroundf(tipY + x * s + y * c); };
    g->fillTriangle(rx(0, 0), ry(0, 0), rx(-w, len), ry(-w, len), rx(w, len), ry(w, len), colour);
  }

  static void thickLine(int x0, int y0, int x1, int y1, int thickness, uint16_t colour) {
    int half = thickness / 2;
    for (int i = -half; i <= half; i++) {
      if (abs(x1 - x0) > abs(y1 - y0)) g->drawLine(x0, y0 + i, x1, y1 + i, colour);
      else                             g->drawLine(x0 + i, y0, x1 + i, y1, colour);
    }
  }

  /**
   * One segment of the route ribbon, offset perpendicular to its own direction.
   * Offsetting along a fixed axis instead leaves a notch wherever a diagonal meets a
   * vertical - which on a bending road is exactly where the rider is looking.
   */
  static void ribbonSegment(int x0, int y0, int x1, int y1, int halfWidth, uint16_t colour) {
    float dx = (float)(x1 - x0), dy = (float)(y1 - y0);
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.5f) return;
    float nx = -dy / len, ny = dx / len;
    for (int i = -halfWidth; i <= halfWidth; i++) {
      g->drawLine(x0 + (int)lroundf(nx * i), y0 + (int)lroundf(ny * i),
                  x1 + (int)lroundf(nx * i), y1 + (int)lroundf(ny * i), colour);
    }
  }

  static void drawArrow(ManeuverType type, uint16_t colour) {
    const int cx = ICON_CX, cy = ICON_CY, r = ICON_R;

    switch (type) {
      case MANEUVER_STRAIGHT:
      case MANEUVER_DEPART:
      case MANEUVER_MERGE:
        thickLine(cx, cy + r, cx, cy - r + 6, 5, colour);
        arrowHead(cx, cy - r - 1, 0, 9, 8, colour);
        break;

      case MANEUVER_LEFT:
      case MANEUVER_SHARP_LEFT:
        thickLine(cx + 7, cy + r, cx + 7, cy, 5, colour);
        thickLine(cx + 7, cy, cx - 4, cy, 5, colour);
        arrowHead(cx - r + 2, cy, -90, 9, 8, colour);
        break;

      case MANEUVER_RIGHT:
      case MANEUVER_SHARP_RIGHT:
        thickLine(cx - 7, cy + r, cx - 7, cy, 5, colour);
        thickLine(cx - 7, cy, cx + 4, cy, 5, colour);
        arrowHead(cx + r - 2, cy, 90, 9, 8, colour);
        break;

      case MANEUVER_SLIGHT_LEFT:
        thickLine(cx + 5, cy + r, cx + 5, cy + 2, 5, colour);
        thickLine(cx + 5, cy + 2, cx - 4, cy - 8, 5, colour);
        arrowHead(cx - 8, cy - 12, -40, 9, 8, colour);
        break;

      case MANEUVER_SLIGHT_RIGHT:
        thickLine(cx - 5, cy + r, cx - 5, cy + 2, 5, colour);
        thickLine(cx - 5, cy + 2, cx + 4, cy - 8, 5, colour);
        arrowHead(cx + 8, cy - 12, 40, 9, 8, colour);
        break;

      case MANEUVER_UTURN:
        thickLine(cx - 7, cy + r, cx - 7, cy - 3, 4, colour);
        g->drawCircle(cx - 1, cy - 4, 6, colour);
        g->drawCircle(cx - 1, cy - 4, 7, colour);
        thickLine(cx + 6, cy - 4, cx + 6, cy + 3, 4, colour);
        arrowHead(cx + 6, cy + r - 3, 180, 9, 8, colour);
        break;

      case MANEUVER_ROUNDABOUT:
        g->drawCircle(cx, cy, 8, colour);
        g->drawCircle(cx, cy, 9, colour);
        thickLine(cx, cy + r, cx, cy + 9, 5, colour);
        thickLine(cx + 6, cy - 6, cx + 11, cy - 11, 4, colour);
        arrowHead(cx + r - 1, cy - r + 1, 45, 8, 7, colour);
        break;

      case MANEUVER_FORK_LEFT:
      case MANEUVER_OFF_RAMP:
        thickLine(cx, cy + r, cx, cy + 2, 5, colour);
        thickLine(cx, cy + 2, cx + 8, cy - 6, 2, C_OUTLINE);
        thickLine(cx, cy + 2, cx - 8, cy - 7, 5, colour);
        arrowHead(cx - 11, cy - 10, -40, 9, 8, colour);
        break;

      case MANEUVER_FORK_RIGHT:
      case MANEUVER_ON_RAMP:
        thickLine(cx, cy + r, cx, cy + 2, 5, colour);
        thickLine(cx, cy + 2, cx - 8, cy - 6, 2, C_OUTLINE);
        thickLine(cx, cy + 2, cx + 8, cy - 7, 5, colour);
        arrowHead(cx + 11, cy - 10, 40, 9, 8, colour);
        break;

      case MANEUVER_ARRIVE:
        g->drawCircle(cx, cy - 5, 6, colour);
        g->drawCircle(cx, cy - 5, 7, colour);
        thickLine(cx, cy + 1, cx, cy + 9, 4, colour);
        g->fillCircle(cx, cy + 11, 2, colour);
        break;

      default:
        break;
    }
  }

  // ---------- Landmarks ----------

  /** Tiny glyphs for the places sent alongside the route. Kept to ~7px so they read
      as map pins rather than competing with the road itself. */
  static void drawLandmark(int x, int y, uint8_t type, uint16_t colour) {
    switch (type) {
      case LANDMARK_FUEL:     // droplet
        g->fillTriangle(x, y - 5, x - 3, y + 1, x + 3, y + 1, colour);
        g->fillCircle(x, y + 1, 3, colour);
        break;
      case LANDMARK_FOOD:     // fork + plate
        g->drawFastVLine(x - 2, y - 4, 8, colour);
        g->drawFastVLine(x + 2, y - 4, 8, colour);
        g->drawFastHLine(x - 2, y, 5, colour);
        break;
      case LANDMARK_HOSPITAL: // cross
        g->fillRect(x - 1, y - 5, 3, 10, colour);
        g->fillRect(x - 5, y - 1, 10, 3, colour);
        break;
      case LANDMARK_ATM:      // banknote
        g->drawRect(x - 5, y - 3, 10, 6, colour);
        g->fillCircle(x, y, 1, colour);
        break;
      case LANDMARK_JUNCTION: // hollow diamond
        g->drawLine(x, y - 4, x + 4, y, colour);
        g->drawLine(x + 4, y, x, y + 4, colour);
        g->drawLine(x, y + 4, x - 4, y, colour);
        g->drawLine(x - 4, y, x, y - 4, colour);
        break;
      default:                // generic place pin
        g->drawCircle(x, y - 2, 3, colour);
        g->drawFastVLine(x, y + 1, 4, colour);
        break;
    }
  }

  // ---------- The route ribbon ----------

  /** One projected point, in sprite pixels. */
  struct PPoint { int x; int y; };

  /**
   * A small filled chevron pointing along (dx, dy).
   *
   * Map apps put these on the route line and they do real work: on a 160px screen a
   * bare stroke tells you the road bends but not which end of it you are at. Animating
   * them forward makes the direction of travel readable without a single word.
   */
  static void drawChevron(int x, int y, float dx, float dy, int size, uint16_t colour) {
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.01f) return;
    dx /= len; dy /= len;
    float nx = -dy, ny = dx;               // perpendicular, for the two tails
    int tipX = x + (int)lroundf(dx * size);
    int tipY = y + (int)lroundf(dy * size);
    int aX = x + (int)lroundf(nx * size - dx * size * 0.4f);
    int aY = y + (int)lroundf(ny * size - dy * size * 0.4f);
    int bX = x - (int)lroundf(nx * size + dx * size * 0.4f);
    int bY = y - (int)lroundf(ny * size + dy * size * 0.4f);
    g->fillTriangle(tipX, tipY, aX, aY, x, y, colour);
    g->fillTriangle(tipX, tipY, bX, bY, x, y, colour);
  }

  /** Eased zoom, so a reroute or a new slice glides instead of snapping. */
  static float shownScale = 0.0f;

  /**
   * The road ahead, heading-up, drawn as a map rather than a lone line.
   *
   * Three things changed here after seeing it on the bars:
   *
   *  - The bike is *anchored*, not fitted. The old bounding-box fit centred the
   *    geometry, which meant the bike itself drifted around the frame whenever the road
   *    bent. Now the bike is always on the horizontal centre line and the scale is
   *    chosen so the geometry fits around it.
   *  - Neighbouring roads are drawn underneath in grey. A route line alone in black
   *    gives no sense of place; the side roads are what make a junction look like a
   *    junction.
   *  - The route is a cased ribbon with travelling chevrons on it, the way every map
   *    app draws one, instead of a flat stroke.
   */
  static void drawRoute(const NavState& s) {
    const int x0 = 3, x1 = SCREEN_W - 4;
    const int y0 = ROUTE_TOP, y1 = ROUTE_BOTTOM;

    if (s.numPoints < 2) return;

    const int ax = (x0 + x1) / 2;
    const int ay = y0 + (int)((y1 - y0) * BIKE_ANCHOR_FRAC);

    // How far the geometry reaches in each direction from the bike, in metres. Starts
    // at 1 so a route that never goes (say) left simply places no constraint there
    // rather than dividing by zero.
    float maxR = 1, maxL = 1, maxU = 1, maxD = 1;
    auto consider = [&](float x, float y) {
      if (x > maxR) maxR = x;
      if (-x > maxL) maxL = -x;
      if (y > maxU) maxU = y;
      if (-y > maxD) maxD = -y;
    };

    for (int i = 0; i < s.numPoints; i++) consider(s.route[i].x, s.route[i].y);
    for (int i = 0; i < s.numBranches; i++) {
      const Branch& b = s.branches[i];
      float a = b.headingDeg * DEG_TO_RAD;
      consider(b.x, b.y);
      consider(b.x + sinf(a) * b.lengthM, b.y + cosf(a) * b.lengthM);
    }
    // Landmarks deliberately do NOT drive the fit. A petrol station 200 m off the road
    // would zoom the whole map out to include it; instead they are simply clipped away
    // if they fall outside, which costs nothing that matters.

    const float pad = 5.0f;
    float scale = fminf(
      fminf((x1 - ax - pad) / maxR, (ax - x0 - pad) / maxL),
      fminf((ay - y0 - pad) / maxU, (y1 - ay - 2.0f) / maxD)
    );
    if (scale > 3.0f) scale = 3.0f;
    if (scale < 0.02f) scale = 0.02f;

    // Ease the zoom. Snap on a big change so a reroute doesn't crawl there.
    if (shownScale <= 0.0f || fabsf(scale - shownScale) > shownScale * 0.8f) shownScale = scale;
    else shownScale = ease(shownScale, scale, 0.18f);
    const float k = shownScale;

    auto px = [&](float mx) { return (int)lroundf(ax + mx * k); };
    auto py = [&](float my) { return (int)lroundf(ay - my * k); };

    // --- neighbouring roads, underneath everything ---
    for (int i = 0; i < s.numBranches; i++) {
      const Branch& b = s.branches[i];
      float a = b.headingDeg * DEG_TO_RAD;
      int sx = px(b.x), sy = py(b.y);
      int ex = px(b.x + sinf(a) * b.lengthM), ey = py(b.y + cosf(a) * b.lengthM);
      ribbonSegment(sx, sy, ex, ey, 2, C_ROADCAS);
      ribbonSegment(sx, sy, ex, ey, 1, C_ROAD);
    }

    // --- project the route once, then reuse it ---
    PPoint pts[MAX_ROUTE_POINTS];
    int n = s.numPoints;
    for (int i = 0; i < n; i++) pts[i] = { px(s.route[i].x), py(s.route[i].y) };

    // Casing first, then fill. The dark outline is what stops the ribbon dissolving
    // into the background in daylight, and what makes it read as a road on top of the
    // grey side streets rather than crossing behind them.
    for (int pass = 0; pass < 2; pass++) {
      int halfWidth = (pass == 0) ? 4 : 2;
      uint16_t colour = (pass == 0) ? C_CASING : C_ROUTE;
      for (int i = 1; i < n; i++) {
        ribbonSegment(pts[i - 1].x, pts[i - 1].y, pts[i].x, pts[i].y, halfWidth, colour);
        g->fillCircle(pts[i].x, pts[i].y, halfWidth, colour); // round the joint
      }
      g->fillCircle(pts[0].x, pts[0].y, halfWidth, colour);
    }

    // --- chevrons marching along the ribbon ---
    {
      const float SPACING = 22.0f;                       // pixels between chevrons
      float phase = fmodf(frame * 0.55f, SPACING);       // animates them forward
      float nextAt = phase;
      float walked = 0.0f;
      for (int i = 1; i < n; i++) {
        float dx = (float)(pts[i].x - pts[i - 1].x);
        float dy = (float)(pts[i].y - pts[i - 1].y);
        float seg = sqrtf(dx * dx + dy * dy);
        if (seg < 0.5f) continue;
        while (nextAt <= walked + seg) {
          float t = (nextAt - walked) / seg;
          drawChevron((int)lroundf(pts[i - 1].x + dx * t),
                      (int)lroundf(pts[i - 1].y + dy * t),
                      dx, dy, 3, C_TEXT);
          nextAt += SPACING;
        }
        walked += seg;
      }
    }

    // --- landmarks, dim so they give context without competing with the road ---
    for (int i = 0; i < s.numLandmarks; i++) {
      int lx = px(s.landmarks[i].x), ly = py(s.landmarks[i].y);
      if (lx < x0 + 4 || lx > x1 - 4 || ly < y0 + 5 || ly > y1 - 5) continue;
      drawLandmark(lx, ly, s.landmarks[i].type, C_DIM);
    }

    // --- where the turn actually happens ---
    // Without it the rider can see the road bend but not which bend the countdown
    // refers to. It pulses as the turn gets close.
    if (s.distanceToTurnM > 0) {
      float target = (float)s.distanceToTurnM, walked = 0;
      for (int i = 1; i < s.numPoints; i++) {
        float dx = s.route[i].x - s.route[i - 1].x;
        float dy = s.route[i].y - s.route[i - 1].y;
        float seg = sqrtf(dx * dx + dy * dy);
        if (seg < 0.01f) continue;
        if (walked + seg >= target) {
          float t = (target - walked) / seg;
          int mx = px(s.route[i - 1].x + dx * t);
          int my = py(s.route[i - 1].y + dy * t);
          if (pulse > 0.05f) {
            int halo = 5 + (int)(((sinf(frame / 4.0f) + 1.0f) * 0.5f) * 4.0f * pulse);
            g->drawCircle(mx, my, halo, mix(C_BG, C_ACCENT, pulse));
          }
          g->fillCircle(mx, my, 4, C_BG);
          g->fillCircle(mx, my, 3, C_ACCENT);
          break;
        }
        walked += seg;
      }
    }

    // --- the bike, on the anchor, always pointing up ---
    // The slice arrives already rotated heading-up, so "up" is the direction of travel
    // by construction.
    int bx = px(0.0f), by = py(0.0f);
    g->fillCircle(bx, by, 8, C_BG);                       // knock a hole in the ribbon
    g->fillTriangle(bx, by - 8, bx + 6, by + 6, bx, by + 2, C_TEXT);
    g->fillTriangle(bx, by - 8, bx - 6, by + 6, bx, by + 2, C_TEXT);
  }

  // ---------- Screen sections ----------

  static void drawHeader(const NavState& s, uint16_t accent) {
    drawArrow(s.maneuver, accent);

    char buf[16];
    formatDistance((uint32_t)(shownDistance + 0.5f), buf, sizeof(buf));

    g->setTextDatum(TL_DATUM);
    g->setFont(&fonts::Font4);
    g->setTextColor(accent, C_BG);
    g->drawString(buf, TEXT_X, 1);

    if (s.streetName[0] != '\0') {
      g->setFont(&fonts::Font2);
      g->setTextColor(C_DIM, C_BG);
      char name[MAX_STREET_NAME + 1];
      strncpy(name, s.streetName, sizeof(name));
      name[sizeof(name) - 1] = '\0';
      int available = SCREEN_W - TEXT_X - 3;
      while (name[0] != '\0' && g->textWidth(name) > available) name[strlen(name) - 1] = '\0';
      g->drawString(name, TEXT_X, 19);
    }

    g->drawFastHLine(0, HEADER_H, SCREEN_W, C_OUTLINE);
  }

  /** Three even cells. Number first, unit after - the old layout read "km/h 0". */
  static void drawFooter(const NavState& s) {
    g->drawFastHLine(0, FOOTER_TOP - 3, SCREEN_W, C_OUTLINE);

    char speed[12], dist[12], eta[12];
    snprintf(speed, sizeof(speed), "%u km/h", (unsigned)((s.speedKmhX10 + 5) / 10));
    formatDistance(s.remainingDistanceM, dist, sizeof(dist));
    formatDuration(s.etaSeconds, eta, sizeof(eta));

    const char* cells[3] = { speed, dist, eta };
    const int cellW = SCREEN_W / 3;

    g->setFont(&fonts::Font2);
    g->setTextDatum(TC_DATUM);
    for (int i = 0; i < 3; i++) {
      g->setTextColor(i == 0 ? C_TEXT : C_DIM, C_BG);
      g->drawString(cells[i], cellW * i + cellW / 2, FOOTER_TOP + 2);
      if (i > 0) g->drawFastVLine(cellW * i, FOOTER_TOP, SCREEN_H - FOOTER_TOP, C_OUTLINE);
    }
  }

  static void drawBanner(const char* text, uint16_t colour) {
    g->fillRect(0, 0, SCREEN_W, 18, colour);
    g->setFont(&fonts::Font2);
    g->setTextDatum(TL_DATUM);
    g->setTextColor(C_BG, colour);
    g->drawString(text, 3, 0);
  }

  static void drawCentred(const char* line1, const char* line2, uint16_t colour) {
    g->setTextDatum(MC_DATUM);
    g->setFont(&fonts::Font4);
    g->setTextColor(colour, C_BG);
    g->drawString(line1, SCREEN_W / 2, SCREEN_H / 2 - 12);
    if (line2 != nullptr) {
      g->setFont(&fonts::Font2);
      g->setTextColor(C_DIM, C_BG);
      g->drawString(line2, SCREEN_W / 2, SCREEN_H / 2 + 14);
    }
  }

  /** Waiting screen with a breathing dot, so it never looks frozen. */
  static void drawWaiting(const char* line1, const char* line2, uint16_t colour) {
    drawCentred(line1, line2, colour);
    float t = (sinf(frame / 10.0f) + 1.0f) * 0.5f;
    int r = 2 + (int)(t * 3);
    g->fillCircle(SCREEN_W / 2, SCREEN_H - 16, r, mix(C_OUTLINE, colour, t));
  }

  static void drawArrivedScreen(const NavState& s) {
    // Rings expanding outward from the pin - a small celebration on arrival.
    for (int i = 0; i < 3; i++) {
      float t = fmodf((frame / 30.0f) + i * 0.33f, 1.0f);
      int radius = (int)(t * 44);
      g->drawCircle(SCREEN_W / 2, 52, radius, mix(C_ACCENT, C_BG, t));
    }
    drawArrow(MANEUVER_ARRIVE, C_ACCENT);

    g->setTextDatum(MC_DATUM);
    g->setFont(&fonts::Font4);
    g->setTextColor(C_ACCENT, C_BG);
    g->drawString("Arrived", SCREEN_W / 2, 52);

    if (s.streetName[0] != '\0') {
      g->setFont(&fonts::Font2);
      g->setTextColor(C_DIM, C_BG);
      g->drawString(s.streetName, SCREEN_W / 2, 100);
    }
  }

  // ---------- Startup animation ----------

  /**
   * Plays once at power-on, while BLE is still coming up.
   *
   * Worth the ~1.6s: it proves the panel is alive before any phone is involved, and a
   * device that shows nothing until it is connected feels broken when it isn't.
   */
  static void splash() {
    if (!spriteReady) return;

    const int cx = SCREEN_W / 2, cy = 46;

    // Phase 1: chevron draws itself in, rising from below.
    for (int step = 0; step <= 16; step++) {
      float t = step / 16.0f;
      fb.fillSprite(C_BG);
      int y = cy + (int)((1.0f - t) * 30);
      uint16_t c = mix(C_BG, C_ACCENT, t);
      int size = 10 + (int)(t * 8);
      fb.fillTriangle(cx, y - size, cx + size, y + size, cx, y + size / 2, c);
      fb.fillTriangle(cx, y - size, cx - size, y + size, cx, y + size / 2, c);
      fb.pushSprite(0, 0);
      delay(18);
    }

    // Phase 2: wordmark wipes in from the left under the mark.
    fb.setFont(&fonts::Font4);
    fb.setTextDatum(MC_DATUM);
    const char* word = "RouteAhead";
    int wordW = fb.textWidth(word);

    for (int step = 0; step <= 14; step++) {
      float t = step / 14.0f;
      fb.fillSprite(C_BG);
      fb.fillTriangle(cx, cy - 18, cx + 18, cy + 18, cx, cy + 9, C_ACCENT);
      fb.fillTriangle(cx, cy - 18, cx - 18, cy + 18, cx, cy + 9, C_ACCENT);

      fb.setTextColor(C_TEXT, C_BG);
      fb.drawString(word, cx, 86);
      // Mask the not-yet-revealed part rather than clipping text, which LovyanGFX
      // cannot do mid-string.
      int reveal = (int)(wordW * t);
      fb.fillRect(cx - wordW / 2 + reveal, 74, wordW - reveal + 2, 24, C_BG);

      fb.pushSprite(0, 0);
      delay(20);
    }

    // Phase 3: a sweep along the bottom while the radio starts.
    for (int step = 0; step <= 20; step++) {
      float t = step / 20.0f;
      fb.fillSprite(C_BG);
      fb.fillTriangle(cx, cy - 18, cx + 18, cy + 18, cx, cy + 9, C_ACCENT);
      fb.fillTriangle(cx, cy - 18, cx - 18, cy + 18, cx, cy + 9, C_ACCENT);
      fb.setFont(&fonts::Font4);
      fb.setTextDatum(MC_DATUM);
      fb.setTextColor(C_TEXT, C_BG);
      fb.drawString(word, cx, 86);

      fb.drawFastHLine(20, 112, SCREEN_W - 40, C_OUTLINE);
      fb.fillRect(20, 111, (int)((SCREEN_W - 40) * t), 3, C_ACCENT);
      fb.pushSprite(0, 0);
      delay(16);
    }
  }

  // ---------- Public API ----------

  void begin() {
    tft.init();
    tft.setRotation(1); // landscape, 160x128
#if TFT_BL >= 0
    tft.setBrightness(255);
#endif
    tft.fillScreen(C_BG);

    // 16-bit sprite is 160*128*2 = 40 KB. Falls back to 8-bit rather than losing
    // double-buffering: a flickering nav display is worse than a coarser palette.
    fb.setColorDepth(16);
    spriteReady = (fb.createSprite(SCREEN_W, SCREEN_H) != nullptr);
    if (!spriteReady) {
      Serial.println("16-bit sprite alloc failed, retrying at 8-bit");
      fb.setColorDepth(8);
      spriteReady = (fb.createSprite(SCREEN_W, SCREEN_H) != nullptr);
    }
    if (!spriteReady) {
      Serial.println("Sprite alloc failed - drawing direct to panel, expect flicker");
    }

    g = spriteReady ? static_cast<LovyanGFX*>(&fb) : static_cast<LovyanGFX*>(&tft);

    splash();
  }

  void render() {
    NavState s;
    if (navStateLock(pdMS_TO_TICKS(20))) {
      s = navState;
      navStateUnlock();
    } else {
      return; // writer holds it; skip rather than draw a torn frame
    }

    frame++;

    // Ease the displayed distance toward the real one. A jump from 300 to 250 reads as
    // a glitch; a slide reads as movement. Snap on big jumps (a reroute) so it never
    // crawls across a genuine discontinuity.
    float targetDistance = (float)s.distanceToTurnM;
    if (fabsf(targetDistance - shownDistance) > 400.0f) shownDistance = targetDistance;
    else shownDistance = ease(shownDistance, targetDistance, 0.25f);

    if (s.maneuver != lastManeuver) {
      lastManeuver = s.maneuver;
      maneuverChangedAt = millis();
    }

    bool stale = s.hasRoute && (millis() - s.lastPacketMs > STALE_AFTER_MS);

    if (spriteReady) fb.fillSprite(C_BG); else tft.fillScreen(C_BG);

    if (!s.connected) {
      drawWaiting("RouteAhead", "Waiting for phone", C_TEXT);
    } else if (stale) {
      drawWaiting("No signal", "Check the app", C_WARN);
    } else if (s.arrived) {
      drawArrivedScreen(s);
    } else if (!s.hasRoute) {
      drawWaiting("Ready", "Pick a destination", C_ACCENT);
    } else {
      // Inside 80 m the turn throbs between white and green, so an imminent turn is
      // obvious from peripheral vision before the number has been read.
      bool imminent = (s.distanceToTurnM > 0 && s.distanceToTurnM <= 80);
      pulse = ease(pulse, imminent ? 1.0f : 0.0f, 0.2f);
      float throb = imminent ? (sinf(frame / 4.0f) + 1.0f) * 0.5f : 0.0f;
      uint16_t accent = mix(C_TEXT, C_ACCENT, pulse * (0.55f + 0.45f * throb));

      // A newly-changed maneuver flashes its icon briefly, so a turn appearing is
      // noticeable even if the distance barely moved.
      if (millis() - maneuverChangedAt < 600 && ((millis() / 150) % 2) == 0) {
        accent = C_ACCENT;
      }

      drawHeader(s, accent);
      drawRoute(s);
      drawFooter(s);

      if (s.rerouting || s.offRoute) drawBanner("Off route - rerouting", C_WARN);
    }

    if (spriteReady) fb.pushSprite(0, 0);
  }

}
