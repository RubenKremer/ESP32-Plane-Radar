#pragma once

#include <cmath>
#include <cstdlib>

#include "services/radar_location.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"

namespace ui::radar {

/** Flat-earth scale used by the radar (matches existing screen math). */
constexpr float kKmPerDeg = 111.0f;

inline void offsetKmFromCenter(float lat, float lon, float* dx_km, float* dy_km,
                               float* dist_km) {
  *dx_km = static_cast<float>(lon - services::location::lon()) * kKmPerDeg;
  *dy_km = static_cast<float>(lat - services::location::lat()) * kKmPerDeg;
  *dist_km = sqrtf((*dx_km) * (*dx_km) + (*dy_km) * (*dy_km));
}

inline int distSqFromCenter(int x, int y) {
  const int dx = x - kCenterX;
  const int dy = y - kCenterY;
  return dx * dx + dy * dy;
}

/** Rotate geographic east/north km so heading-at-top points screen-up. */
inline void rotateMapOffset(float east_km, float north_km, float* screen_east,
                             float* screen_north) {
  const uint16_t heading = headingAtTopDeg();
  if (heading == 0) {
    *screen_east = east_km;
    *screen_north = north_km;
    return;
  }
  constexpr float kDegToRad = 0.01745329252f;
  const float angle = static_cast<float>(heading) * kDegToRad;
  const float sin_a = sinf(angle);
  const float cos_a = cosf(angle);
  *screen_east = east_km * cos_a - north_km * sin_a;
  *screen_north = north_km * cos_a + east_km * sin_a;
}

/** Geographic heading to screen-relative degrees (0 = up). */
inline float headingToScreen(float heading_deg) {
  float rotated = heading_deg - static_cast<float>(headingAtTopDeg());
  while (rotated < 0.0f) {
    rotated += 360.0f;
  }
  while (rotated >= 360.0f) {
    rotated -= 360.0f;
  }
  return rotated;
}

inline void latLonToScreen(float lat, float lon, int* out_x, int* out_y) {
  const float px_per_km =
      static_cast<float>(kGridOuterRadius) / rangeCurrent().outer_km;

  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);

  float screen_east = 0.0f;
  float screen_north = 0.0f;
  rotateMapOffset(dx_km, dy_km, &screen_east, &screen_north);

  *out_x = kCenterX + static_cast<int>(lroundf(screen_east * px_per_km));
  *out_y = kCenterY - static_cast<int>(lroundf(screen_north * px_per_km));
}

/** Move (*x1,*y1) toward (x0,y0) until it lies on the outer grid ring. */
inline void clipPointToOuterRing(int x0, int y0, int* x1, int* y1) {
  const int r = kGridOuterRadius;
  const int r_sq = r * r;
  if (distSqFromCenter(*x1, *y1) <= r_sq) {
    return;
  }

  const float dx = static_cast<float>(*x1 - x0);
  const float dy = static_cast<float>(*y1 - y0);
  const float fx = static_cast<float>(x0 - kCenterX);
  const float fy = static_cast<float>(y0 - kCenterY);
  const float a = dx * dx + dy * dy;
  if (a < 1.0f) {
    *x1 = x0;
    *y1 = y0;
    return;
  }

  const float b = 2.0f * (fx * dx + fy * dy);
  const float c = fx * fx + fy * fy - static_cast<float>(r_sq);
  const float disc = b * b - 4.0f * a * c;
  if (disc < 0.0f) {
    *x1 = x0;
    *y1 = y0;
    return;
  }

  const float sqrt_disc = sqrtf(disc);
  const float inv = 0.5f / a;
  const float t0 = (-b - sqrt_disc) * inv;
  const float t1 = (-b + sqrt_disc) * inv;

  float t = -1.0f;
  if (t0 >= 0.0f && t0 <= 1.0f) {
    t = t0;
  }
  if (t1 >= 0.0f && t1 <= 1.0f && t1 > t) {
    t = t1;
  }
  if (t < 0.0f) {
    *x1 = x0;
    *y1 = y0;
    return;
  }

  *x1 = x0 + static_cast<int>(lroundf(dx * t));
  *y1 = y0 + static_cast<int>(lroundf(dy * t));
}

}  // namespace ui::radar
