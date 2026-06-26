#include "rtcbat.h"
#include "../rtc_pcf85063.h"
#include "../battery.h"
#include <Arduino.h>
#include <time.h>

// Combined firmware RTC/battery shim.
//
// Capsule Radar initializes and owns the shared I2C RTC/PMU services. TamaPoke
// reads/writes through those wrappers instead of creating a second SensorLib/PMU
// instance on the same bus.

bool rtcBegin() {
  return rtc_present();
}

uint32_t rtcEpoch() {
  struct tm tmv = {};
  if (!rtc_read(&tmv)) return 0;
  if (tmv.tm_year + 1900 < 2025 || tmv.tm_year + 1900 > 2120) return 0;
  time_t e = mktime(&tmv);
  return e > 0 ? (uint32_t)e : 0;
}

void rtcSetEpoch(uint32_t e) {
  time_t tt = (time_t)e;
  struct tm tmv = {};
  gmtime_r(&tt, &tmv);
  rtc_write(&tmv);
}

bool batBegin() {
  return true;
}

void pmuEnablePanel() {
  // display.cpp has already initialized and powered the panel in the host app.
}

int batPercent() {
  return battery_present() ? battery_percent() : -1;
}

bool batCharging() {
  return battery_charging();
}

bool usbPresent() {
  return battery_external_power() || !battery_present() || battery_charging();
}

void pwrSetup() {
  // Leave PMU IRQ configuration to the host firmware.
}

bool pwrShortPressed() {
  return false;
}
