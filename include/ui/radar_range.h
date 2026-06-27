#pragma once

#include <cstddef>
#include <cstdint>

namespace ui::radar {

/**
 * Range presets (label on ring 3 = ¾ of outer radius).
 *
 * Recommended for ADS-B on a 1.28″ display:
 *   5 km  — pattern / very local (airfield vicinity)
 *  10 km  — default; neighborhood spotting
 *  15 km  — wider local area
 *  25 km  — metro / regional picture
 *
 * Outer radius (for aircraft math) is ring-3 distance ÷ 0.75.
 */
struct RangePreset {
  /** Distance shown on ring 3 (¾ of outer radius), always stored in km. */
  float ring3_km;
  float outer_km;
};

constexpr float kRing3ToOuterKm = 4.0f / 3.0f;

constexpr RangePreset kRangePresets[] = {
    {5.0f, 5.0f * kRing3ToOuterKm},
    {10.0f, 10.0f * kRing3ToOuterKm},
    {15.0f, 15.0f * kRing3ToOuterKm},
    {25.0f, 25.0f * kRing3ToOuterKm},
};

constexpr size_t kRangePresetCount =
    sizeof(kRangePresets) / sizeof(kRangePresets[0]);

/** ADS-B poll interval presets (minimum spacing between fetch attempts). */
constexpr unsigned long kPollIntervalPresetsMs[] = {3000, 5000, 10000, 15000,
                                                    30000};
constexpr size_t kPollIntervalPresetCount =
    sizeof(kPollIntervalPresetsMs) / sizeof(kPollIntervalPresetsMs[0]);
constexpr uint8_t kDefaultPollIntervalIndex = 0;  // 3 s

/** Load saved range, units, runways, and poll interval from flash. Call once after boot. */
void rangeInit();
/** Cycle preset and save to flash. */
void rangeNext();
/** Set preset index; persists, logs, and marks dirty on change. Returns false if idx invalid. */
bool setRangeIndex(uint8_t idx);
bool rangeDirty();
void clearRangeDirty();
const RangePreset& rangeCurrent();
uint8_t rangeIndex();
/** ADSB fetch radius (km): scaled to screen edge so beyond-ring dots have data. */
float fetchRadiusKm();

bool useMiles();
bool showRunways();
bool showAirportLabels();
/** WiFi portal checkbox: "T" = miles, otherwise km. */
void saveMilesFromPortal(const char* checkbox_value);
void saveRunwaysFromPortal(const char* checkbox_value);
void saveAirportLabelsFromPortal(const char* checkbox_value);
void formatRing3Label(char* buf, size_t len, float ring3_km, bool use_miles);
void formatCurrentRing3Label(char* buf, size_t len);
/** Portal dropdown: dual-unit spaced label, e.g. "10 km / 6 mi". */
void formatRing3PresetOption(char* buf, size_t len, float ring3_km);

unsigned long pollIntervalMs();
uint8_t pollIntervalIndex();
bool setPollIntervalIndex(uint8_t idx);
bool pollTimerReset();
void clearPollTimerReset();
/** Portal dropdown label, e.g. "3 seconds". */
void formatPollIntervalOption(char* buf, size_t len, unsigned long interval_ms);

/** Reset distance units to km (e.g. with WiFi credential wipe). */
void unitsReset();

}  // namespace ui::radar
