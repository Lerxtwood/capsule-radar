#pragma once
// Fetches nearby aircraft from airplanes.live (fallback adsb.lol) and parses
// the readsb JSON into a vector<Aircraft>. See docs/DATA_SOURCE.md.
#include <vector>
#include "config.h"
#include "aircraft.h"

class AdsbClient {
public:
    enum class FailureKind : uint8_t {
        None = 0,
        RateLimited,
        Transport,
        Other
    };

    void begin(double homeLat, double homeLon, float rangeKm);
    void setHome(double lat, double lon) { _lat = lat; _lon = lon; }
    void setRange(float km) { _rangeKm = km; }
    void setLiveAircraftCap(size_t cap) { _liveAircraftCap = cap; }

    // Fetch + parse. Returns true on success and fills `out` (replaces contents).
    // On failure, leaves `out` untouched and returns false (caller keeps last good).
    bool poll(std::vector<Aircraft>& out);

    uint32_t lastOkMs() const { return _lastOkMs; }
    bool cooldownActive() const;
    uint32_t cooldownRemainingMs() const;
    FailureKind lastFailureKind() const { return _lastFailureKind; }

private:
    bool fetchFrom(const char* host, std::vector<Aircraft>& out);   // one host, one attempt
    bool hostCooldownActive(uint32_t untilMs) const;
    uint32_t hostCooldownRemainingMs(uint32_t untilMs) const;
    FailureKind cooldownFailureKind(bool primaryCooling, bool fallbackCooling) const;

    double _lat = 0, _lon = 0;
    float  _rangeKm = 15.0f;
    size_t _liveAircraftCap = ADSB_LIVE_AIRCRAFT_CAP;
    uint32_t _lastOkMs = 0;
    uint32_t _primaryCooldownUntilMs = 0;
    uint32_t _fallbackCooldownUntilMs = 0;
    FailureKind _primaryCooldownKind = FailureKind::None;
    FailureKind _fallbackCooldownKind = FailureKind::None;
    FailureKind _lastFailureKind = FailureKind::None;
};
