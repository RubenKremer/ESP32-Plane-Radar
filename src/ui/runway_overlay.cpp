#include "ui/runway_overlay.h"

#include <lgfx/v1/lgfx_fonts.hpp>

#include <cmath>
#include <cstdlib>

#include "data/large_airports.h"
#include "hardware/display_font.h"
#include "services/radar_location.h"
#include "ui/radar_geo.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"

namespace ui::runway {
namespace {

constexpr size_t kMaxAirportLabels = 32;
constexpr size_t kMaxCachedRunways = 96;

bool s_in_range[data::large_airports::kAirportCount];

bool s_runway_label_ready = false;
bool s_runway_label_use_vlw = false;
float s_runway_label_vlw_size = 0.38f;
const lgfx::GFXfont* s_runway_label_gfx = &fonts::FreeSansBold12pt7b;

uint16_t s_cached_runways[kMaxCachedRunways];
size_t s_cached_runway_count = 0;
uint32_t s_cache_sig = 0;
bool s_cache_valid = false;

int measureVlwHeight(lgfx::LGFXBase& gfx, float size) {
  gfx.setTextSize(size);
  return gfx.fontHeight();
}

float findVlwSizeForHeight(lgfx::LGFXBase& gfx, int target_px) {
  float lo = 0.2f;
  float hi = 1.2f;
  for (int i = 0; i < 14; ++i) {
    const float mid = (lo + hi) * 0.5f;
    if (measureVlwHeight(gfx, mid) < target_px) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return hi;
}

void initRunwayLabelStyle(lgfx::LGFXBase& gfx) {
  if (s_runway_label_ready) {
    return;
  }

  const int target = radar::kRunwayLabelHeightPx;
  if (displayFontIsSmooth()) {
    s_runway_label_use_vlw = true;
    s_runway_label_vlw_size = findVlwSizeForHeight(gfx, target);
  } else {
    s_runway_label_gfx = &fonts::FreeSansBold12pt7b;
    s_runway_label_use_vlw = false;
  }
  s_runway_label_ready = true;
}

void applyRunwayLabelStyle(lgfx::LGFXBase& gfx) {
  if (s_runway_label_use_vlw) {
    displayFontSetSmoothSize(gfx, s_runway_label_vlw_size);
  } else {
    displayFontSetBitmap(gfx, s_runway_label_gfx);
  }
}

float e7ToDeg(int32_t e7) { return static_cast<float>(e7) * 1e-7f; }

uint32_t hashMix(uint32_t h, uint32_t v) {
  return h ^ (v + 0x9e3779b9U + (h << 6) + (h >> 2));
}

uint32_t visibilitySignature() {
  uint32_t h = 0;
  h = hashMix(h, radar::rangeIndex());
  h = hashMix(h, static_cast<uint32_t>(
                     lround(services::location::lat() * 1e7)));
  h = hashMix(h, static_cast<uint32_t>(
                     lround(services::location::lon() * 1e7)));
  return h;
}

bool segmentIntersectsDisc(int x0, int y0, int x1, int y1) {
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int r = radar::kGridOuterRadius;
  const int r_sq = r * r;

  if (radar::distSqFromCenter(x0, y0) <= r_sq ||
      radar::distSqFromCenter(x1, y1) <= r_sq) {
    return true;
  }

  const int dx = x1 - x0;
  const int dy = y1 - y0;
  const int fx = x0 - cx;
  const int fy = y0 - cy;
  const int a = dx * dx + dy * dy;
  if (a == 0) {
    return false;
  }
  const int b = 2 * (fx * dx + fy * dy);
  const int c = fx * fx + fy * fy - r_sq;
  int disc = b * b - 4 * a * c;
  if (disc < 0) {
    return false;
  }
  disc = static_cast<int>(sqrtf(static_cast<float>(disc)));
  const float inv2a = 1.0f / (2.0f * static_cast<float>(a));
  const float t0 = (-static_cast<float>(b) - disc) * inv2a;
  const float t1 = (-static_cast<float>(b) + disc) * inv2a;
  return (t0 >= 0.0f && t0 <= 1.0f) || (t1 >= 0.0f && t1 <= 1.0f);
}

void drawBoldRunwayLabel(lgfx::LGFXBase& gfx, const char* ident, int mx, int my) {
  const int tw = gfx.textWidth(ident);
  const int th = gfx.fontHeight();
  constexpr int kPadX = 2;
  constexpr int kPadY = 1;

  gfx.setTextDatum(textdatum_t::bottom_center);
  const int left = mx - tw / 2 - kPadX;
  const int top = my - th - kPadY;
  gfx.fillRect(left, top, tw + kPadX * 2, th + kPadY, radar::kColorBackground);
  gfx.setTextColor(radar::kColorRunwayLabel, radar::kColorBackground);
  gfx.drawString(ident, mx, my);
}

bool drawRunwayLine(lgfx::LGFXBase& gfx, const data::large_airports::Runway& rw) {
  const float le_lat = e7ToDeg(rw.le_lat_e7);
  const float le_lon = e7ToDeg(rw.le_lon_e7);
  const float he_lat = e7ToDeg(rw.he_lat_e7);
  const float he_lon = e7ToDeg(rw.he_lon_e7);

  int x0 = 0;
  int y0 = 0;
  int x1 = 0;
  int y1 = 0;
  radar::latLonToScreen(le_lat, le_lon, &x0, &y0);
  radar::latLonToScreen(he_lat, he_lon, &x1, &y1);

  if (!segmentIntersectsDisc(x0, y0, x1, y1)) {
    return false;
  }

  radar::clipPointToOuterRing(x0, y0, &x1, &y1);
  radar::clipPointToOuterRing(x1, y1, &x0, &y0);

  gfx.drawWideLine(x0, y0, x1, y1, radar::kRunwayLineHalfWidth,
                   radar::kColorRunway);
  return true;
}

void offsetLabelFromCenter(int ax, int ay, int* lx, int* ly) {
  const int dx = ax - radar::kCenterX;
  const int dy = ay - radar::kCenterY;
  const float len = sqrtf(static_cast<float>(dx * dx + dy * dy));
  const int gap = radar::kRunwayLabelGapPx;
  if (len < 1.0f) {
    *lx = ax;
    *ly = ay - gap;
    return;
  }
  *lx = ax + static_cast<int>(lroundf(dx / len * static_cast<float>(gap)));
  *ly = ay + static_cast<int>(lroundf(dy / len * static_cast<float>(gap)));
}

void clipPointOntoOuterRing(int* x, int* y) {
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int r = radar::kGridOuterRadius;
  const int dx = *x - cx;
  const int dy = *y - cy;
  const int d_sq = dx * dx + dy * dy;
  const int r_sq = r * r;
  if (d_sq <= r_sq || d_sq == 0) {
    return;
  }
  const float scale = static_cast<float>(r) / sqrtf(static_cast<float>(d_sq));
  *x = cx + static_cast<int>(lroundf(static_cast<float>(dx) * scale));
  *y = cy + static_cast<int>(lroundf(static_cast<float>(dy) * scale));
}

void drawAirportLabel(lgfx::LGFXBase& gfx,
                      const data::large_airports::Airport& ap) {
  int ax = 0;
  int ay = 0;
  radar::latLonToScreen(e7ToDeg(ap.lat_e7), e7ToDeg(ap.lon_e7), &ax, &ay);
  clipPointOntoOuterRing(&ax, &ay);

  int lx = 0;
  int ly = 0;
  offsetLabelFromCenter(ax, ay, &lx, &ly);
  drawBoldRunwayLabel(gfx, ap.ident, lx, ly);
}

bool airportInFetchRadius(const data::large_airports::Airport& ap,
                          float radius_km, float lat_min, float lat_max,
                          float lon_min, float lon_max) {
  const float lat = e7ToDeg(ap.lat_e7);
  const float lon = e7ToDeg(ap.lon_e7);
  if (lat < lat_min || lat > lat_max || lon < lon_min || lon > lon_max) {
    return false;
  }
  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  radar::offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);
  return dist_km <= radius_km;
}

void rebuildVisibilityCache() {
  const float radius_km = radar::fetchRadiusKm();
  const float lat = static_cast<float>(services::location::lat());
  const float lon = static_cast<float>(services::location::lon());
  const float lat_pad = radius_km / radar::kKmPerDeg;
  float cos_lat = cosf(lat * 0.01745329252f);
  if (cos_lat < 0.2f) {
    cos_lat = 0.2f;
  }
  const float lon_pad = radius_km / (radar::kKmPerDeg * cos_lat);
  const float lat_min = lat - lat_pad;
  const float lat_max = lat + lat_pad;
  const float lon_min = lon - lon_pad;
  const float lon_max = lon + lon_pad;

  for (size_t i = 0; i < data::large_airports::kAirportCount; ++i) {
    s_in_range[i] = airportInFetchRadius(data::large_airports::kAirports[i],
                                         radius_km, lat_min, lat_max, lon_min,
                                         lon_max);
  }

  s_cached_runway_count = 0;
  for (size_t i = 0; i < data::large_airports::kRunwayCount; ++i) {
    const uint16_t ap_idx = data::large_airports::kRunways[i].airport_idx;
    if (!s_in_range[ap_idx]) {
      continue;
    }
    if (s_cached_runway_count >= kMaxCachedRunways) {
      break;
    }
    s_cached_runways[s_cached_runway_count++] = static_cast<uint16_t>(i);
  }

  s_cache_sig = visibilitySignature();
  s_cache_valid = true;
}

}  // namespace

void drawLargeAirportRunways(lgfx::LGFXBase& gfx) {
  if (!radar::showRunways()) {
    return;
  }
  displayFontEnsureLoaded(gfx);

  const uint32_t sig = visibilitySignature();
  if (!s_cache_valid || sig != s_cache_sig) {
    rebuildVisibilityCache();
  }

  uint16_t label_airports[kMaxAirportLabels];
  size_t label_count = 0;

  for (size_t i = 0; i < s_cached_runway_count; ++i) {
    const uint16_t rw_idx = s_cached_runways[i];
    const auto& rw = data::large_airports::kRunways[rw_idx];
    if (!drawRunwayLine(gfx, rw)) {
      continue;
    }
    const uint16_t ap_idx = rw.airport_idx;
    if (label_count >= kMaxAirportLabels) {
      continue;
    }
    bool already = false;
    for (size_t l = 0; l < label_count; ++l) {
      if (label_airports[l] == ap_idx) {
        already = true;
        break;
      }
    }
    if (!already) {
      label_airports[label_count++] = ap_idx;
    }
  }

  if (label_count == 0 || !radar::showAirportLabels()) {
    return;
  }

  initRunwayLabelStyle(gfx);
  applyRunwayLabelStyle(gfx);
  for (size_t i = 0; i < label_count; ++i) {
    drawAirportLabel(gfx, data::large_airports::kAirports[label_airports[i]]);
  }
}

}  // namespace ui::runway
