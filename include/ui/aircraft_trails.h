#pragma once

#include <LovyanGFX.hpp>
#include <cstdint>

namespace ui::trails {

/** Update trail history after a successful ADS-B fetch. */
void onFetchComplete();

/** Draw dark-grey trail segments (call before aircraft symbols). */
void draw(lgfx::LGFXBase& gfx);

/** Drop all cached trails (e.g. when trails are disabled). */
void clear();

/** Trim every trail to at most max_points (oldest dropped first). */
void trimToPointCount(uint8_t max_points);

}  // namespace ui::trails
