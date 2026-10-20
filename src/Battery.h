#ifndef BATTERY_H
#define BATTERY_H

#include <Arduino.h>

/**
 * Battery level from a resistor divider on an ADC pin.
 *
 * Wiring: BAT+ -> 100k -> BAT_ADC_PIN -> 100k -> GND, so the pin sees half the cell
 * voltage (a full 4.2 V single-cell Li-ion reads 2.1 V, well inside the ADC's range).
 * Pin and ratio come from build flags in platformio.ini.
 *
 * Read only from loop() - it is not shared with the BLE task, so it needs no lock.
 */
namespace Battery {

  void begin();

  /** Call every loop(); takes a new reading every couple of seconds. */
  void update();

  /** False when nothing sensible is on the pin (running from USB with no cell fitted). */
  bool present();

  /** Smoothed cell voltage, millivolts. */
  uint16_t millivolts();

  /** 0..100, from a single-cell Li-ion discharge curve. */
  uint8_t percent();

  /** The pin reads more than a single cell through the divider could ever give. */
  bool wiringFault();

  /** Last averaged reading at the pin itself, millivolts, and the raw ADC count. */
  uint16_t pinMillivolts();
  uint16_t raw();

}

#endif
