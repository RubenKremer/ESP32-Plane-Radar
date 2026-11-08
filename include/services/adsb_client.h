#pragma once

#include <cstddef>
#include <cstdint>

namespace services::adsb {

struct Aircraft {
  float lat;
  float lon;
  float nose_deg;
  float track_deg;
  float gs_knots;
  /** Age of the position fix at fetch time (ms), from the feed's seen_pos.
   *  Added to the time since the fetch when dead-reckoning, so the estimate
   *  starts from when the position was measured, not when it was fetched. */
  uint32_t pos_age_ms;
  char hex[7];
  char callsign[9];
  char type[5];
  char alt[12];
};

constexpr size_t kMaxAircraft = 64;

size_t aircraftCount();
const Aircraft* aircraftList();

/**
 * millis() of the last successful fetch (0 before the first). The stored
 * lat/lon are as of this time minus each aircraft's pos_age_ms.
 */
unsigned long lastUpdateMs();

/**
 * Dead-reckoned position of `plane` at `now_ms`: its last fix moved along its
 * ground track at its ground speed for the time since the fix was measured.
 * Returns the fix unchanged when there is no speed or no fetch yet.
 */
void estimatedPosition(const Aircraft& plane, unsigned long now_ms, float* lat,
                       float* lon);

/** Hook invoked during long HTTP I/O (e.g. wifiLoop). Optional. */
using PollFn = void (*)();
void setPollFn(PollFn fn);

/** Return true to cancel an in-flight fetch (e.g. BOOT held). Optional. */
using AbortFn = bool (*)();
void setAbortFn(AbortFn fn);

/** Fetch aircraft within fetch_radius_km of center_lat/lon from adsb.fi. */
bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km);

}  // namespace services::adsb
