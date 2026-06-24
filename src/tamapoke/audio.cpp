#include "audio.h"
#include <Arduino.h>
#include <Preferences.h>

// Combined firmware audio shim.
//
// Capsule Radar owns the ES8311/I2S driver. Keep TamaPoke's SFX API available so
// the game logic and settings screen work, but do not initialize or touch I2S here.
// Later we can map selected TamaPoke SFX IDs onto the radar audio task.

static bool gOn = true;

void audioBegin() {
  Preferences p;
  p.begin("tamapoke", true);
  gOn = p.getBool("snd", true);
  p.end();
  Serial.println("TamaPoke audio shim ready (SFX routed off)");
}

void sfxPlay(uint8_t) {
  // no-op for now: prevents a second I2S driver from destabilizing radar audio
}

void audioSetEnabled(bool on) {
  gOn = on;
  Preferences p;
  p.begin("tamapoke", false);
  p.putBool("snd", on);
  p.end();
}

bool audioEnabled() { return gOn; }
