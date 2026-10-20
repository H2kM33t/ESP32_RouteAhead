#include "DisplayRenderer.h"
#include "Battery.h"
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
  // Deliberately minimal per spec: the route ribbon on top (as large as the panel
  // allows), and below it just the turn icon beside the distance to that turn.
  // Nothing else - no street name, no trip stats, no dial arc - so the two things a
  // rider actually needs mid-ride get all the space instead of sharing it with
  // secondary readouts.
  //
  //   +----------------------------------------+
  //   |               .                        |
  //   |              /                         |
  //   |             A                          |  route ribbon (the hero), big
  //   |            /                           |
  //   +----------------------------------------+
  //   |   ^                                    |
  //   |  /_\        300 m                      |  turn icon beside the distance
  //   +----------------------------------------+
  static const int SCREEN_W = 160;
  static const int SCREEN_H = 128;

  static const int ROUTE_TOP    = 3;
  static const int ROUTE_BOTTOM = 90;
  static const int PANEL_TOP    = 94;

  // Where the bike sits inside the map area, as a fraction of its height.
  //
  // Not 0.5: the slice is almost entirely road *ahead*, so centring the geometry left
  // the bike wherever the bounding box happened to put it - usually off to one side,
  // which is what made the old screen look lopsided. Anchoring the bike instead puts it
  // on the horizontal centre line every frame, with the road running up from it the way
  // a map app does.
  static const float BIKE_ANCHOR_FRAC = 0.88f;

  // Turn icon + distance, centred as a pair in the strip below the route.
  static const int ICON_CX = 32;
  static const int ICON_CY = 111;
  static const int ICON_R  = 16;
  static const int NUM_CX  = 104;
  static const int NUM_CY  = 111;

  // ---------- Palette (RGB565) ----------
  static const uint16_t C_BG      = 0x0000;
  static const uint16_t C_OUTLINE = 0x2124;
  static const uint16_t C_ACCENT  = 0x07E8; // green - brand colour, used for the turn
  static const uint16_t C_ROUTE   = 0xDEDB; // pale, near-white - a dial line, not a map layer
  static const uint16_t C_CASING  = 0x3186; // soft grey halo, just enough to lift it off black
  static const uint16_t C_TEXT    = 0xFFFF;
  static const uint16_t C_DIM     = 0x8410;
  static const uint16_t C_WARN    = 0xFD20;
  static const uint16_t C_LOW     = 0xF800; // red - battery nearly flat

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
  // Deliberately identical to formatDistance() in the Android app; if the two
  // disagree the rider sees one number on the phone and another on the bars.

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

  /**
   * The turn icon, centred on (cx, cy).
   *
   * [turnAngle] is the real angle the road turns through (+right, -left, +/-180 for a
   * U-turn) when [hasAngle]; it steers the two icons that used to be one fixed picture
   * whatever the junction looked like - the roundabout exit and the side of a U-turn.
   */
  static void drawArrow(ManeuverType type, uint16_t colour, int cx, int cy,
                        int turnAngle = 0, bool hasAngle = false, uint8_t exitNo = 0) {
    const int r = ICON_R;

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

      case MANEUVER_UTURN: {
        // Up one leg, over a solid half-ring, back down the other with the head
        // pointing back toward the rider. The old version drew two full circles, which
        // read as a lollipop rather than a hairpin. Right-hand by default (left-hand
        // traffic); mirrored when the phone says the U-turn goes left.
        const int m = (hasAngle && turnAngle < 0) ? -1 : 1;
        const int legX = 7, topY = cy - 5;
        // Half-ring as disc minus hole minus lower half: unambiguous on every LovyanGFX
        // version, unlike fillArc's angle conventions. Nothing else is drawn here yet.
        g->fillCircle(cx, topY, 9, colour);
        g->fillCircle(cx, topY, 4, C_BG);
        g->fillRect(cx - 10, topY + 1, 21, 10, C_BG);
        thickLine(cx - legX * m, cy + r, cx - legX * m, topY, 5, colour);
        thickLine(cx + legX * m, topY, cx + legX * m, cy + 5, 5, colour);
        arrowHead(cx + legX * m, cy + r, 180, 9, 8, colour);
        break;
      }

      case MANEUVER_ROUNDABOUT: {
        // Ring, entry from below, and the exit leaving at the angle the road really
        // takes - previously every roundabout showed the same upper-right exit, so a
        // first-exit left and a third-exit right looked identical. The exit number sits
        // inside the ring.
        const int rcy = cy - 1;
        const int ringOuter = 11, ringInner = 8;
        float a = (hasAngle ? (float)turnAngle : 45.0f);
        if (a > 170.0f) a = 170.0f;
        if (a < -170.0f) a = -170.0f;
        const float ar = a * DEG_TO_RAD;
        const float dx = sinf(ar), dy = -cosf(ar);

        uint16_t ring = mix(C_BG, colour, 0.6f);
        g->fillCircle(cx, rcy, ringOuter, ring);
        g->fillCircle(cx, rcy, ringInner, C_BG);
        thickLine(cx, cy + r, cx, rcy + ringOuter - 1, 5, colour);

        // The exit: a short spoke leaving the ring, capped with the arrowhead.
        thickLine(cx + (int)lroundf(dx * (ringOuter - 2)), rcy + (int)lroundf(dy * (ringOuter - 2)),
                  cx + (int)lroundf(dx * (ringOuter + 3)), rcy + (int)lroundf(dy * (ringOuter + 3)),
                  5, colour);
        arrowHead(cx + (int)lroundf(dx * (ringOuter + 9)), rcy + (int)lroundf(dy * (ringOuter + 9)),
                  a, 8, 7, colour);

        if (exitNo > 0 && exitNo < 10) {
          char num[2] = { (char)('0' + exitNo), '\0' };
          g->setFont(&fonts::Font2);
          g->setTextDatum(MC_DATUM);
          g->setTextColor(colour, C_BG);
          g->drawString(num, cx, rcy + 1);
        }
        break;
      }

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

  // ---------- The route ribbon ----------

  /** One projected point, in sprite pixels. */
  struct PPoint { int x; int y; };

  /** Eased zoom, so a reroute or a new slice glides instead of snapping. */
  static float shownScale = 0.0f;

  /**
   * The road ahead, heading-up, drawn as a map rather than a lone line.
   *
   * The bike is *anchored*, not fitted. The old bounding-box fit centred the
   * geometry, which meant the bike itself drifted around the frame whenever the road
   * bent. Now the bike is always on the horizontal centre line and the scale is
   * chosen so the geometry fits around it.
   *
   * Deliberately just the route line, the turn marker, and the bike - no neighbouring
   * roads and no landmark glyphs, so nothing beside the route competes with it.
   */
  static void drawRoute(const NavState& s) {
    const int x0 = 3, x1 = SCREEN_W - 4;
    const int y0 = ROUTE_TOP, y1 = ROUTE_BOTTOM;

    if (s.numPoints < 2) return;

    const int ax = (x0 + x1) / 2;
    const int ay = y0 + (int)((y1 - y0) * BIKE_ANCHOR_FRAC);

    const int n = s.numPoints;

    // --- split the route at the maneuver ---
    // Everything up to the turn is "the road you are on"; everything after it is
    // context. Finding that split first matters for two things below: the zoom, and
    // drawing the two halves differently.
    //
    // turnSeg/turnT: the turn lies on segment (turnSeg-1 -> turnSeg) at fraction turnT.
    // turnSeg == 0 means no turn inside the slice (it is further away than the slice
    // reaches), in which case the whole slice is "the road you are on".
    // Finds the point [target] metres along the slice. Returns the segment index (0 if the
    // slice is shorter than that) and writes the point.
    auto locate = [&](float target, float& ox, float& oy) -> int {
      float walked = 0;
      for (int i = 1; i < n; i++) {
        float dx = s.route[i].x - s.route[i - 1].x;
        float dy = s.route[i].y - s.route[i - 1].y;
        float seg = sqrtf(dx * dx + dy * dy);
        if (seg < 0.01f) continue;
        if (walked + seg >= target) {
          float t = (target - walked) / seg;
          ox = s.route[i - 1].x + dx * t;
          oy = s.route[i - 1].y + dy * t;
          return i;
        }
        walked += seg;
      }
      return 0;
    };

    // The countdown point (green dot) is where the maneuver starts. The split between
    // "your road" and "context" is normally the same point - but approaching a
    // roundabout it is the EXIT. Splitting at the entry used to show the ring as a dim
    // 35 m stub veering left (traffic goes clockwise), right under an icon saying the
    // exit was to the right: the map and the arrow seemed to disagree.
    float entryX = 0.0f, entryY = 0.0f;
    int entrySeg = (s.distanceToTurnM > 0) ? locate((float)s.distanceToTurnM, entryX, entryY) : 0;

    int turnSeg = entrySeg;
    float turnX = entryX, turnY = entryY;
    const bool showRing = entrySeg > 0 && s.maneuver == MANEUVER_ROUNDABOUT && s.roundaboutRingM > 0;
    if (showRing) {
      // Exit beyond the end of the slice: the whole slice is "your road".
      turnSeg = locate((float)(s.distanceToTurnM + s.roundaboutRingM), turnX, turnY);
    }

    // --- zoom ---
    // How far the road reaches in each direction from the bike, in metres. Starts at 1
    // so a direction the road never goes places no constraint rather than dividing by 0.
    //
    // Two fixes against the old fit-everything zoom:
    //  - Only the road up to the turn, plus a short stub past it, drives the zoom. The
    //    turn is what the rider is looking for; fitting the whole 250 m slice kept a
    //    junction 40 m ahead as a tiny kink in the middle of the screen.
    //  - Road BEHIND the bike no longer drives it. The anchor leaves ~10 px below the
    //    bike, so a U-turn (whose return leg runs back past the bike) or a roundabout
    //    loop used to force the scale down until the whole map was a dot. That road is
    //    now simply clipped at the bottom of the map area.
    float maxR = 1, maxL = 1, maxU = 1;
    auto consider = [&](float x, float y) {
      if (y < 0) y = 0;
      if (x > maxR) maxR = x;
      if (-x > maxL) maxL = -x;
      if (y > maxU) maxU = y;
    };

    const float PAST_TURN_M = 35.0f;
    if (turnSeg > 0) {
      for (int i = 0; i < turnSeg; i++) consider(s.route[i].x, s.route[i].y);
      consider(turnX, turnY);
      // A stub past the turn, so the direction the road leaves in is on screen.
      float walked = 0, px0 = turnX, py0 = turnY;
      for (int i = turnSeg; i < n && walked < PAST_TURN_M; i++) {
        float dx = s.route[i].x - px0, dy = s.route[i].y - py0;
        float seg = sqrtf(dx * dx + dy * dy);
        if (seg < 0.01f) continue;
        if (walked + seg > PAST_TURN_M) {
          float t = (PAST_TURN_M - walked) / seg;
          consider(px0 + dx * t, py0 + dy * t);
          break;
        }
        consider(s.route[i].x, s.route[i].y);
        walked += seg;
        px0 = s.route[i].x; py0 = s.route[i].y;
      }
    } else {
      for (int i = 0; i < n; i++) consider(s.route[i].x, s.route[i].y);
    }

    const float pad = 6.0f;
    float scale = fminf(
      fminf((x1 - ax - pad) / maxR, (ax - x0 - pad) / maxL),
      (ay - y0 - pad) / maxU
    );
    if (scale > 3.0f) scale = 3.0f;
    if (scale < 0.02f) scale = 0.02f;

    // Ease the zoom. Snap on a big change so a reroute doesn't crawl there.
    if (shownScale <= 0.0f || fabsf(scale - shownScale) > shownScale * 0.8f) shownScale = scale;
    else shownScale = ease(shownScale, scale, 0.18f);
    const float k = shownScale;

    auto px = [&](float mx) { return (int)lroundf(ax + mx * k); };
    auto py = [&](float my) { return (int)lroundf(ay - my * k); };

    // Nothing in the map may spill into the turn panel - road clipped by the new zoom
    // would otherwise be drawn straight over the distance readout.
    g->setClipRect(0, y0, SCREEN_W, y1 - y0 + 2);

    // --- project the route once, with the turn point spliced in ---
    PPoint pts[MAX_ROUTE_POINTS + 1];
    int np = 0;
    int splitAt = n - 1;                      // index in pts[] of the turn point
    for (int i = 0; i < n; i++) {
      if (turnSeg > 0 && i == turnSeg) {
        splitAt = np;
        pts[np++] = { px(turnX), py(turnY) };
      }
      pts[np++] = { px(s.route[i].x), py(s.route[i].y) };
    }

    auto stroke = [&](int from, int to, int halfWidth, uint16_t colour) {
      for (int i = from + 1; i <= to; i++) {
        ribbonSegment(pts[i - 1].x, pts[i - 1].y, pts[i].x, pts[i].y, halfWidth, colour);
        g->fillCircle(pts[i].x, pts[i].y, halfWidth, colour); // round the joint
      }
      g->fillCircle(pts[from].x, pts[from].y, halfWidth, colour);
    };

    // Road after the turn: thinner and dimmer, drawn first so it sits underneath. On a
    // U-turn the way back runs right alongside the way out, and round a roundabout the
    // path crosses its own approach - drawn identically, the two merged into one
    // unreadable line and the rider could not tell which part was "now".
    if (splitAt < np - 1) {
      stroke(splitAt, np - 1, 2, C_OUTLINE);
      stroke(splitAt, np - 1, 0, C_DIM);
    }

    // Road up to the turn: a slim pale line with a barely-there halo, closer to a
    // watch-face dial marking than a map layer.
    stroke(0, splitAt, 2, C_CASING);
    stroke(0, splitAt, 1, C_ROUTE);

    // --- where the turn actually happens ---
    // Without it the rider can see the road bend but not which bend the countdown
    // refers to. It pulses as the turn gets close.
    // The roundabout's exit: a small white ring where the rider leaves the circle.
    if (showRing && turnSeg > 0) {
      int ex = px(turnX), ey = py(turnY);
      g->fillCircle(ex, ey, 4, C_BG);
      g->drawCircle(ex, ey, 3, C_TEXT);
      g->drawCircle(ex, ey, 2, C_TEXT);
    }

    if (entrySeg > 0) {
      int mx = px(entryX), my = py(entryY);
      if (pulse > 0.05f) {
        int halo = 5 + (int)(((sinf(frame / 4.0f) + 1.0f) * 0.5f) * 4.0f * pulse);
        g->drawCircle(mx, my, halo, mix(C_BG, C_ACCENT, pulse));
      }
      g->fillCircle(mx, my, 4, C_BG);
      g->fillCircle(mx, my, 3, C_ACCENT);
    }

    // --- the bike, on the anchor, always pointing up ---
    // The slice arrives already rotated heading-up, so "up" is the direction of travel
    // by construction.
    int bx = px(0.0f), by = py(0.0f);
    g->fillCircle(bx, by, 8, C_BG);                       // knock a hole in the ribbon
    g->fillTriangle(bx, by - 8, bx + 6, by + 6, bx, by + 2, C_TEXT);
    g->fillTriangle(bx, by - 8, bx - 6, by + 6, bx, by + 2, C_TEXT);

    g->clearClipRect();
  }

  // ---------- Screen sections ----------

  /**
   * The turn panel: icon on the left, the distance to that turn beside it.
   * Nothing else - no arc, no trip stats - so it's the biggest, simplest thing on
   * the screen after the route itself.
   */
  static void drawTurnPanel(const NavState& s, uint16_t accent) {
    g->drawFastHLine(6, PANEL_TOP, SCREEN_W - 12, C_OUTLINE);

    drawArrow(s.maneuver, accent, ICON_CX, ICON_CY, s.turnAngleDeg, s.hasTurnAngle,
              s.roundaboutExit);

    char buf[16];
    formatDistance((uint32_t)(shownDistance + 0.5f), buf, sizeof(buf));

    g->setTextDatum(MC_DATUM);
    g->setFont(&fonts::Font4);
    g->setTextColor(accent, C_BG);
    g->drawString(buf, NUM_CX, NUM_CY);
  }

  /**
   * Battery level, top-right: a small cell icon with the percentage beside it.
   *
   * Kept dim while there's plenty of charge so it never competes with the route, then
   * amber at 20% and a blinking red at 10% - by which point a Li-ion cell is minutes
   * from cutting out, and losing the display mid-ride is worth noticing.
   */
  static void drawBattery() {
    if (!Battery::present()) return;   // on USB with no cell fitted
    uint8_t pct = Battery::percent();

    uint16_t colour = C_DIM;
    if (pct <= 10) {
      if ((millis() / 500) % 2) return;   // blink
      colour = C_LOW;
    } else if (pct <= 20) {
      colour = C_WARN;
    }

    const int w = 16, h = 8;
    const int x = SCREEN_W - w - 4, y = 3;
    g->fillRect(x - 24, y - 1, w + 28, h + 2, C_BG);        // keep the route off it
    g->drawRect(x, y, w, h, colour);
    g->fillRect(x + w, y + 2, 2, h - 4, colour);           // the terminal nub
    int fill = (int)((w - 4) * pct / 100.0f + 0.5f);
    if (fill > 0) g->fillRect(x + 2, y + 2, fill, h - 4, colour);

    char buf[6];
    snprintf(buf, sizeof(buf), "%u%%", pct);
    g->setFont(&fonts::Font0);
    g->setTextDatum(TR_DATUM);
    g->setTextColor(colour, C_BG);
    g->drawString(buf, x - 3, y);
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
    // Rings expanding outward from the pin - a small celebration on arrival. The pin
    // used to be drawn down in the turn-panel corner with "Arrived" printed over the
    // middle of the rings; now the pin is the centre of the rings and the text sits
    // clear below it.
    const int pinY = 40;
    for (int i = 0; i < 3; i++) {
      float t = fmodf((frame / 30.0f) + i * 0.33f, 1.0f);
      int radius = 10 + (int)(t * 32);
      g->drawCircle(SCREEN_W / 2, pinY - 4, radius, mix(C_ACCENT, C_BG, t));
    }
    drawArrow(MANEUVER_ARRIVE, C_ACCENT, SCREEN_W / 2, pinY);

    g->setTextDatum(MC_DATUM);
    g->setFont(&fonts::Font4);
    g->setTextColor(C_ACCENT, C_BG);
    g->drawString("Arrived", SCREEN_W / 2, 88);

    if (s.streetName[0] != '\0') {
      g->setFont(&fonts::Font2);
      g->setTextColor(C_DIM, C_BG);
      g->drawString(s.streetName, SCREEN_W / 2, 112);
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

      drawRoute(s);
      drawTurnPanel(s, accent);

      if (s.rerouting || s.offRoute) drawBanner("Off route - rerouting", C_WARN);
    }

    // The banner spans the top edge, so the battery gives way to it.
    bool banner = s.connected && !stale && !s.arrived && s.hasRoute && (s.rerouting || s.offRoute);
    if (!banner) drawBattery();

    if (spriteReady) fb.pushSprite(0, 0);
  }

}
