#include "ui/radar_range.h"

#include "ui/radar_theme.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ui::radar {

namespace {

constexpr char kPrefsNamespace[] = "planeradar";
constexpr char kPrefsRangeKey[] = "rangeIdx";
constexpr char kPrefsMilesKey[] = "useMiles";
constexpr char kPrefsAltFeetKey[] = "useAltFt";
constexpr char kPrefsSpeedKnotsKey[] = "useSpdKt";
constexpr char kPrefsRunwaysKey[] = "showRwys";
constexpr char kPrefsAirportLabelsKey[] = "showApLbl";
constexpr char kPrefsTagCallsignKey[] = "tagCall";
constexpr char kPrefsTagTypeKey[] = "tagType";
constexpr char kPrefsTagAltKey[] = "tagAlt";
constexpr char kPrefsTagSpeedKey[] = "tagSpd";
constexpr char kPrefsPollKey[] = "pollIdx";
constexpr uint8_t kDefaultRangeIndex = 1;  // 10 km ring
constexpr float kKmPerMile = 1.609344f;

Preferences s_prefs;
uint8_t s_range_index = kDefaultRangeIndex;
uint8_t s_poll_index = kDefaultPollIntervalIndex;
bool s_use_miles = false;
bool s_use_feet_for_altitude = true;
bool s_use_knots_for_speed = true;
bool s_show_runways = true;
bool s_show_airport_labels = true;
bool s_show_aircraft_callsign = true;
bool s_show_aircraft_type = true;
bool s_show_aircraft_altitude = true;
bool s_show_aircraft_speed = false;
bool s_range_dirty = false;
bool s_poll_timer_reset = false;

void saveRangeIndex() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putUChar(kPrefsRangeKey, s_range_index);
  s_prefs.end();
}

void saveUseMiles() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(kPrefsMilesKey, s_use_miles);
  s_prefs.end();
}

void saveUseFeetForAltitude() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(kPrefsAltFeetKey, s_use_feet_for_altitude);
  s_prefs.end();
}

void saveUseKnotsForSpeed() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(kPrefsSpeedKnotsKey, s_use_knots_for_speed);
  s_prefs.end();
}

void saveShowRunways() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(kPrefsRunwaysKey, s_show_runways);
  s_prefs.end();
}

void saveShowAirportLabels() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(kPrefsAirportLabelsKey, s_show_airport_labels);
  s_prefs.end();
}

void saveAircraftLabelPrefs() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(kPrefsTagCallsignKey, s_show_aircraft_callsign);
  s_prefs.putBool(kPrefsTagTypeKey, s_show_aircraft_type);
  s_prefs.putBool(kPrefsTagAltKey, s_show_aircraft_altitude);
  s_prefs.putBool(kPrefsTagSpeedKey, s_show_aircraft_speed);
  s_prefs.end();
}

void savePollIndex() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putUChar(kPrefsPollKey, s_poll_index);
  s_prefs.end();
}

bool portalCheckboxChecked(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  // WiFiManager checkbox submits its value= attribute ("T", or "F" if we prefilled F).
  if ((value[0] == 'T' || value[0] == 't' || value[0] == 'F' || value[0] == 'f') &&
      value[1] == '\0') {
    return true;
  }
  return strcmp(value, "on") == 0;
}

}  // namespace

void rangeInit() {
  if (!s_prefs.begin(kPrefsNamespace, true)) {
    return;
  }
  const uint8_t saved = s_prefs.getUChar(kPrefsRangeKey, kDefaultRangeIndex);
  s_range_index =
      (saved < kRangePresetCount) ? saved : kDefaultRangeIndex;
  s_use_miles = s_prefs.getBool(kPrefsMilesKey, false);
  s_use_feet_for_altitude = s_prefs.getBool(kPrefsAltFeetKey, true);
  s_use_knots_for_speed = s_prefs.getBool(kPrefsSpeedKnotsKey, true);
  s_show_runways = s_prefs.getBool(kPrefsRunwaysKey, true);
  s_show_airport_labels = s_prefs.getBool(kPrefsAirportLabelsKey, true);
  s_show_aircraft_callsign = s_prefs.getBool(kPrefsTagCallsignKey, true);
  s_show_aircraft_type = s_prefs.getBool(kPrefsTagTypeKey, true);
  s_show_aircraft_altitude = s_prefs.getBool(kPrefsTagAltKey, true);
  s_show_aircraft_speed = s_prefs.getBool(kPrefsTagSpeedKey, false);
  const uint8_t saved_poll =
      s_prefs.getUChar(kPrefsPollKey, kDefaultPollIntervalIndex);
  s_poll_index = (saved_poll < kPollIntervalPresetCount)
                     ? saved_poll
                     : kDefaultPollIntervalIndex;
  s_prefs.end();
}

void rangeNext() {
  setRangeIndex(static_cast<uint8_t>((s_range_index + 1) % kRangePresetCount));
}

bool setRangeIndex(uint8_t idx) {
  if (idx >= kRangePresetCount) {
    return false;
  }
  if (idx == s_range_index) {
    s_range_dirty = false;
    return true;
  }
  s_range_index = idx;
  saveRangeIndex();
  s_range_dirty = true;
  char range_label[12];
  formatCurrentRing3Label(range_label, sizeof(range_label));
  Serial.printf("Range: %s (outer ~%.0f km)\n", range_label,
                rangeCurrent().outer_km);
  return true;
}

bool rangeDirty() { return s_range_dirty; }

void clearRangeDirty() { s_range_dirty = false; }

const RangePreset& rangeCurrent() { return kRangePresets[s_range_index]; }

uint8_t rangeIndex() { return s_range_index; }

float fetchRadiusKm() {
  const float outer_km = rangeCurrent().outer_km;
  const float screen_r_px =
      static_cast<float>(kCenterX - kBeyondRingScreenMarginPx);
  return outer_km * (screen_r_px / static_cast<float>(kGridOuterRadius));
}

bool useMiles() { return s_use_miles; }

bool useFeetForAltitude() { return s_use_feet_for_altitude; }

bool useKnotsForSpeed() { return s_use_knots_for_speed; }

bool showRunways() { return s_show_runways; }

bool showAirportLabels() { return s_show_airport_labels; }

bool showAircraftCallsign() { return s_show_aircraft_callsign; }

bool showAircraftType() { return s_show_aircraft_type; }

bool showAircraftAltitude() { return s_show_aircraft_altitude; }

bool showAircraftSpeed() { return s_show_aircraft_speed; }

bool anyAircraftTagEnabled() {
  return s_show_aircraft_callsign || s_show_aircraft_type ||
         s_show_aircraft_altitude || s_show_aircraft_speed;
}

void saveMilesFromPortal(const char* checkbox_value) {
  s_use_miles = portalCheckboxChecked(checkbox_value);
  saveUseMiles();
  Serial.printf("Distance units: %s\n", s_use_miles ? "miles" : "km");
}

void saveAltitudeFeetFromPortal(const char* checkbox_value) {
  s_use_feet_for_altitude = portalCheckboxChecked(checkbox_value);
  saveUseFeetForAltitude();
  Serial.printf("Altitude units: %s\n",
                s_use_feet_for_altitude ? "feet" : "meters");
}

void saveSpeedKnotsFromPortal(const char* checkbox_value) {
  s_use_knots_for_speed = portalCheckboxChecked(checkbox_value);
  saveUseKnotsForSpeed();
  Serial.printf("Speed units: %s\n",
                s_use_knots_for_speed ? "knots" : "km/h");
}

void saveRunwaysFromPortal(const char* checkbox_value) {
  s_show_runways = portalCheckboxChecked(checkbox_value);
  saveShowRunways();
  Serial.printf("Runway overlay: %s\n", s_show_runways ? "on" : "off");
}

void saveAirportLabelsFromPortal(const char* checkbox_value) {
  s_show_airport_labels = portalCheckboxChecked(checkbox_value);
  saveShowAirportLabels();
  Serial.printf("Airport labels: %s\n", s_show_airport_labels ? "on" : "off");
}

void saveAircraftTagsFromPortal(bool callsign, bool type, bool altitude,
                                bool speed) {
  s_show_aircraft_callsign = callsign;
  s_show_aircraft_type = type;
  s_show_aircraft_altitude = altitude;
  s_show_aircraft_speed = speed;
  saveAircraftLabelPrefs();
  Serial.printf("Aircraft tags: call=%s type=%s alt=%s spd=%s\n",
                s_show_aircraft_callsign ? "on" : "off",
                s_show_aircraft_type ? "on" : "off",
                s_show_aircraft_altitude ? "on" : "off",
                s_show_aircraft_speed ? "on" : "off");
}

void formatRing3Label(char* buf, size_t len, float ring3_km, bool use_miles) {
  if (use_miles) {
    const int mi = static_cast<int>(lroundf(ring3_km / kKmPerMile));
    snprintf(buf, len, "%dmi", mi);
  } else {
    const int km = static_cast<int>(lroundf(ring3_km));
    snprintf(buf, len, "%dkm", km);
  }
}

void formatCurrentRing3Label(char* buf, size_t len) {
  formatRing3Label(buf, len, rangeCurrent().ring3_km, s_use_miles);
}

void formatRing3PresetOption(char* buf, size_t len, float ring3_km) {
  const int km = static_cast<int>(lroundf(ring3_km));
  const int mi = static_cast<int>(lroundf(ring3_km / kKmPerMile));
  snprintf(buf, len, "%d km / %d mi", km, mi);
}

unsigned long pollIntervalMs() {
  return kPollIntervalPresetsMs[s_poll_index];
}

uint8_t pollIntervalIndex() { return s_poll_index; }

bool setPollIntervalIndex(uint8_t idx) {
  if (idx >= kPollIntervalPresetCount) {
    return false;
  }
  if (idx == s_poll_index) {
    return true;
  }
  s_poll_index = idx;
  savePollIndex();
  s_poll_timer_reset = true;
  Serial.printf("Poll interval: %lu s\n", pollIntervalMs() / 1000UL);
  return true;
}

bool pollTimerReset() { return s_poll_timer_reset; }

void clearPollTimerReset() { s_poll_timer_reset = false; }

void formatPollIntervalOption(char* buf, size_t len, unsigned long interval_ms) {
  const unsigned long sec = interval_ms / 1000UL;
  snprintf(buf, len, "%lu second%s", sec, sec == 1UL ? "" : "s");
}

void formatAircraftSpeedLabel(char* buf, size_t len, float gs_knots) {
  if (len == 0) {
    return;
  }
  buf[0] = '\0';
  if (gs_knots <= 0.0f) {
    return;
  }
  if (s_use_knots_for_speed) {
    snprintf(buf, len, "%d kt", static_cast<int>(lroundf(gs_knots)));
  } else {
    constexpr float kKnotsToKmh = 1.852f;
    snprintf(buf, len, "%d km/h",
             static_cast<int>(lroundf(gs_knots * kKnotsToKmh)));
  }
}

void unitsReset() {
  s_use_miles = false;
  s_use_feet_for_altitude = true;
  s_use_knots_for_speed = true;
  s_show_runways = true;
  s_show_airport_labels = true;
  s_show_aircraft_callsign = true;
  s_show_aircraft_type = true;
  s_show_aircraft_altitude = true;
  s_show_aircraft_speed = false;
  if (s_prefs.begin(kPrefsNamespace, false)) {
    s_prefs.remove(kPrefsMilesKey);
    s_prefs.remove(kPrefsAltFeetKey);
    s_prefs.remove(kPrefsSpeedKnotsKey);
    s_prefs.remove(kPrefsRunwaysKey);
    s_prefs.remove(kPrefsAirportLabelsKey);
    s_prefs.remove(kPrefsTagCallsignKey);
    s_prefs.remove(kPrefsTagTypeKey);
    s_prefs.remove(kPrefsTagAltKey);
    s_prefs.remove(kPrefsTagSpeedKey);
    s_prefs.end();
  }
}

}  // namespace ui::radar
