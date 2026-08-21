#include "ui/aircraft_trails.h"

#include <LovyanGFX.hpp>
#include <cmath>
#include <cstring>

#include "services/adsb_client.h"
#include "ui/radar_geo.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"

namespace ui::trails {
namespace {

constexpr size_t kMaxTrailPointsStorage = 16;
constexpr size_t kMaxTrailSlots = services::adsb::kMaxAircraft;
constexpr float kMinTrailMoveKm = 0.05f;
constexpr uint8_t kMaxUnseenPolls = 3;

struct TrailPoint {
  float lat = 0.0f;
  float lon = 0.0f;
};

struct TrailSlot {
  char hex[7] = {};
  TrailPoint points[kMaxTrailPointsStorage];
  uint8_t count = 0;
  uint8_t unseen_polls = 0;
};

TrailSlot s_slots[kMaxTrailSlots];
size_t s_slot_count = 0;

float innerRingMaxKm() {
  const float outer_km = radar::rangeCurrent().outer_km;
  return outer_km *
         (static_cast<float>(radar::kGridOuterRadius -
                             radar::kAircraftInsideRingInsetPx) /
          static_cast<float>(radar::kGridOuterRadius));
}

bool aircraftInsideInnerRing(const services::adsb::Aircraft& ac) {
  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  radar::offsetKmFromCenter(ac.lat, ac.lon, &dx_km, &dy_km, &dist_km);
  return dist_km <= innerRingMaxKm();
}

bool hexValid(const char* hex) { return hex != nullptr && hex[0] != '\0'; }

bool hexEqual(const char* a, const char* b) { return strcmp(a, b) == 0; }

float pointDistanceKm(float lat0, float lon0, float lat1, float lon1) {
  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  const float lat_mid = (lat0 + lat1) * 0.5f;
  dx_km = (lon1 - lon0) * radar::kKmPerDeg;
  dy_km = (lat1 - lat0) * radar::kKmPerDeg;
  dist_km = sqrtf(dx_km * dx_km + dy_km * dy_km);
  (void)lat_mid;
  return dist_km;
}

TrailSlot* findSlot(const char* hex) {
  for (size_t i = 0; i < s_slot_count; ++i) {
    if (hexEqual(s_slots[i].hex, hex)) {
      return &s_slots[i];
    }
  }
  return nullptr;
}

TrailSlot* allocSlot(const char* hex) {
  TrailSlot* slot = findSlot(hex);
  if (slot != nullptr) {
    return slot;
  }
  if (s_slot_count >= kMaxTrailSlots) {
    return nullptr;
  }
  slot = &s_slots[s_slot_count++];
  memset(slot, 0, sizeof(*slot));
  strncpy(slot->hex, hex, sizeof(slot->hex) - 1);
  return slot;
}

void trimSlotToLimit(TrailSlot* slot, uint8_t max_points) {
  if (max_points == 0 || slot->count <= max_points) {
    return;
  }
  const uint8_t drop = static_cast<uint8_t>(slot->count - max_points);
  memmove(slot->points, slot->points + drop,
          static_cast<size_t>(max_points) * sizeof(TrailPoint));
  slot->count = max_points;
}

void appendPoint(TrailSlot* slot, float lat, float lon, uint8_t max_points) {
  if (max_points == 0) {
    return;
  }
  if (slot->count > 0) {
    const TrailPoint& last = slot->points[slot->count - 1];
    if (pointDistanceKm(last.lat, last.lon, lat, lon) < kMinTrailMoveKm) {
      return;
    }
  }
  if (slot->count >= max_points) {
    memmove(slot->points, slot->points + 1,
            static_cast<size_t>(max_points - 1) * sizeof(TrailPoint));
    slot->count = static_cast<uint8_t>(max_points - 1);
  }
  slot->points[slot->count].lat = lat;
  slot->points[slot->count].lon = lon;
  ++slot->count;
}

void compactSlots() {
  size_t write = 0;
  for (size_t read = 0; read < s_slot_count; ++read) {
    if (s_slots[read].unseen_polls >= kMaxUnseenPolls) {
      continue;
    }
    if (write != read) {
      s_slots[write] = s_slots[read];
    }
    ++write;
  }
  s_slot_count = write;
}

}  // namespace

void clear() {
  s_slot_count = 0;
  memset(s_slots, 0, sizeof(s_slots));
}

void onFetchComplete() {
  const uint8_t max_points = radar::trailPointCount();
  if (max_points == 0) {
    clear();
    return;
  }

  const size_t n = services::adsb::aircraftCount();
  const services::adsb::Aircraft* planes = services::adsb::aircraftList();

  for (size_t i = 0; i < n; ++i) {
    const services::adsb::Aircraft& ac = planes[i];
    if (!hexValid(ac.hex) || !aircraftInsideInnerRing(ac)) {
      continue;
    }
    TrailSlot* slot = allocSlot(ac.hex);
    if (slot == nullptr) {
      continue;
    }
    slot->unseen_polls = 0;
    trimSlotToLimit(slot, max_points);
    appendPoint(slot, ac.lat, ac.lon, max_points);
  }

  for (size_t s = 0; s < s_slot_count; ++s) {
    bool matched = false;
    for (size_t i = 0; i < n; ++i) {
      if (hexValid(planes[i].hex) && hexEqual(s_slots[s].hex, planes[i].hex) &&
          aircraftInsideInnerRing(planes[i])) {
        matched = true;
        break;
      }
    }
    if (!matched) {
      ++s_slots[s].unseen_polls;
    }
  }
  compactSlots();
}

void trimToPointCount(uint8_t max_points) {
  if (max_points == 0) {
    clear();
    return;
  }
  for (size_t s = 0; s < s_slot_count; ++s) {
    trimSlotToLimit(&s_slots[s], max_points);
  }
}

uint16_t trailColorForAge(lgfx::LGFXBase& gfx, size_t segment_i,
                          size_t segment_count) {
  float t = 1.0f;
  if (segment_count > 1) {
    t = static_cast<float>(segment_i) /
        static_cast<float>(segment_count - 1);
  }
  const float mix =
      radar::kTrailFadeFloor + (1.0f - radar::kTrailFadeFloor) * t;
  const uint8_t r =
      static_cast<uint8_t>(lroundf(static_cast<float>(radar::kTrailR) * mix));
  const uint8_t g =
      static_cast<uint8_t>(lroundf(static_cast<float>(radar::kTrailG) * mix));
  const uint8_t b =
      static_cast<uint8_t>(lroundf(static_cast<float>(radar::kTrailB) * mix));
  return gfx.color565(r, g, b);
}

void draw(lgfx::LGFXBase& gfx) {
  const uint8_t max_points = radar::trailPointCount();
  if (max_points == 0 || s_slot_count == 0) {
    return;
  }

  for (size_t s = 0; s < s_slot_count; ++s) {
    const TrailSlot& slot = s_slots[s];
    if (slot.count < 2) {
      continue;
    }
    const uint8_t draw_count =
        slot.count > max_points ? max_points : slot.count;
    const size_t start = slot.count - draw_count;
    const size_t segment_count = draw_count - 1;
    int prev_x = 0;
    int prev_y = 0;
    radar::latLonToScreen(slot.points[start].lat, slot.points[start].lon,
                          &prev_x, &prev_y);
    size_t segment_i = 0;
    for (size_t p = start + 1; p < slot.count; ++p) {
      int x = 0;
      int y = 0;
      radar::latLonToScreen(slot.points[p].lat, slot.points[p].lon, &x, &y);
      int x0 = prev_x;
      int y0 = prev_y;
      int x1 = x;
      int y1 = y;
      radar::clipPointToOuterRing(x1, y1, &x0, &y0);
      radar::clipPointToOuterRing(x0, y0, &x1, &y1);
      gfx.drawWideLine(x0, y0, x1, y1, radar::kTrailLineHalfWidth,
                       trailColorForAge(gfx, segment_i, segment_count));
      ++segment_i;
      prev_x = x;
      prev_y = y;
    }
  }
}

}  // namespace ui::trails
