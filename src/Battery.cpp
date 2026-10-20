#include "Battery.h"

#ifndef BAT_ADC_PIN
#define BAT_ADC_PIN 0
#endif

// Cell voltage / pin voltage. 2.0 for two equal resistors.
#ifndef BAT_DIVIDER
#define BAT_DIVIDER 2.0f
#endif

namespace Battery {

  static const uint32_t SAMPLE_EVERY_MS = 2000;
  static const int SAMPLES = 16;

  // Below this the pin is floating or tied low - no cell connected.
  static const float NO_BATTERY_MV = 2500.0f;

  // A single cell tops out at 4.2 V, i.e. 2.1 V at the pin. Anything near the ADC's
  // ~3 V ceiling means the divider isn't dividing (bottom resistor not reaching GND, or
  // the sense wire on the battery side of it). Report that rather than a confident 100%.
  static const float MAX_PLAUSIBLE_PIN_MV = 2600.0f;

  static float smoothedMv = 0.0f;
  static float lastPinMv = 0.0f;
  static uint16_t lastRaw = 0;
  static uint32_t lastSampleMs = 0;
  static bool havePrimed = false;

  // Single-cell Li-ion under light load: voltage -> percent. The curve is flat through
  // the middle and drops fast at the bottom, so a straight 3.0..4.2 V line would read
  // 50% when the cell is nearly empty.
  struct Point { float mv; float pct; };
  static const Point CURVE[] = {
    { 3300,   0 }, { 3500,   5 }, { 3600,  12 }, { 3700,  28 }, { 3800,  45 },
    { 3900,  62 }, { 4000,  78 }, { 4100,  90 }, { 4200, 100 },
  };
  static const int CURVE_LEN = sizeof(CURVE) / sizeof(CURVE[0]);

  static float readMv() {
    // The divider's 50k source impedance is high for the ADC's sample capacitor, so a
    // single read is noisy. Average a burst; a 100 nF cap from the pin to GND helps too.
    uint32_t sum = 0, rawSum = 0;
    for (int i = 0; i < SAMPLES; i++) {
      sum += analogReadMilliVolts(BAT_ADC_PIN);
      rawSum += analogRead(BAT_ADC_PIN);
    }
    lastPinMv = sum / (float)SAMPLES;
    lastRaw = (uint16_t)(rawSum / SAMPLES);
    return lastPinMv * BAT_DIVIDER;
  }

  bool wiringFault() { return lastPinMv > MAX_PLAUSIBLE_PIN_MV; }
  uint16_t pinMillivolts() { return (uint16_t)(lastPinMv + 0.5f); }
  uint16_t raw() { return lastRaw; }

  // The percentage on screen only moves once the real value is more than this many
  // points away from it. Around 24% the curve is steep enough that a few millivolts of
  // ADC noise (worse without C1) flipped the number between 23, 24 and 25 every couple
  // of seconds; a battery level that twitches reads as broken.
  static const int DEADBAND_PCT = 2;
  static int shownPct = -1;

  static float curvePercent(float mv) {
    if (mv <= CURVE[0].mv) return 0;
    if (mv >= CURVE[CURVE_LEN - 1].mv) return 100;
    for (int i = 1; i < CURVE_LEN; i++) {
      if (mv <= CURVE[i].mv) {
        float t = (mv - CURVE[i - 1].mv) / (CURVE[i].mv - CURVE[i - 1].mv);
        return CURVE[i - 1].pct + t * (CURVE[i].pct - CURVE[i - 1].pct);
      }
    }
    return 100;
  }

  static void updateShownPercent(bool snap) {
    int actual = (int)(curvePercent(smoothedMv) + 0.5f);
    if (snap || shownPct < 0) shownPct = actual;
    else if (actual > shownPct + DEADBAND_PCT) shownPct = actual - DEADBAND_PCT;
    else if (actual < shownPct - DEADBAND_PCT) shownPct = actual + DEADBAND_PCT;
    // Never show 0 or 100 because of the band - the ends are where the rider looks.
    if (actual == 0 || actual == 100) shownPct = actual;
  }

  void begin() {
    pinMode(BAT_ADC_PIN, INPUT);
    analogReadResolution(12);
    analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);   // full range, ~0..2.5 V on the C3
    smoothedMv = readMv();
    havePrimed = true;
    lastSampleMs = millis();
    updateShownPercent(true);
  }

  void update() {
    uint32_t now = millis();
    if (havePrimed && now - lastSampleMs < SAMPLE_EVERY_MS) return;
    lastSampleMs = now;
    float mv = readMv();
    // Ease slowly toward new readings - a time constant of ~20 s at one sample every
    // 2 s - but snap on a big jump (battery plugged in or pulled out).
    bool jump = !havePrimed || fabsf(mv - smoothedMv) > 400.0f;
    if (jump) smoothedMv = mv;
    else smoothedMv += (mv - smoothedMv) * 0.1f;
    havePrimed = true;
    updateShownPercent(jump);
  }

  bool present() { return smoothedMv >= NO_BATTERY_MV && !wiringFault(); }

  uint16_t millivolts() { return (uint16_t)(smoothedMv + 0.5f); }

  uint8_t percent() { return (uint8_t)(shownPct < 0 ? 0 : shownPct); }

}
