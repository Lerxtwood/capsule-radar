#include "audio.h"
#include "../audio.h"
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
  Serial.println("TamaPoke audio shim ready (SFX routed to shared codec)");
}

void sfxPlay(uint8_t id) {
  if (!gOn || !audio_present()) return;
  switch (id) {
    case SFX_TAP:    audio_play(AUDIO_TAMA_TAP); break;
    case SFX_EAT:    audio_play(AUDIO_TAMA_EAT); break;
    case SFX_PLAY:   audio_play(AUDIO_TAMA_PLAY); break;
    case SFX_HEART:  audio_play(AUDIO_TAMA_HEART); break;
    case SFX_HATCH:  audio_play(AUDIO_TAMA_HATCH); break;
    case SFX_EVOLVE: audio_play(AUDIO_TAMA_EVOLVE); break;
    case SFX_MEDAL:  audio_play(AUDIO_TAMA_MEDAL); break;
    case SFX_DENY:   audio_play(AUDIO_TAMA_DENY); break;
    case SFX_BYE:    audio_play(AUDIO_TAMA_BYE); break;
    case SFX_LEVEL:  audio_play(AUDIO_TAMA_LEVEL); break;
    default: break;
  }
}

void audioSetEnabled(bool on) {
  gOn = on;
  Preferences p;
  p.begin("tamapoke", false);
  p.putBool("snd", on);
  p.end();
}

bool audioEnabled() { return gOn; }
