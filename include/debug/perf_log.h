#pragma once

#include <Arduino.h>
#include <cstdint>

#include "config.h"

namespace debug::perf {

struct Sample {
  uint32_t http_begin_ms = 0;
  uint32_t http_get_ms = 0;
  uint32_t json_ms = 0;
  uint32_t layout_ms = 0;
  uint32_t grid_ms = 0;
  uint32_t items_ms = 0;
  uint32_t blit_ms = 0;
  uint32_t total_ms = 0;
  uint32_t heap_free = 0;
  uint32_t heap_min = 0;
  uint32_t heap_max_alloc = 0;
  int http_code = 0;
  int content_len = -1;
  uint16_t aircraft = 0;
  uint16_t drawn = 0;
  uint16_t dots = 0;
  int8_t rssi = 0;
  bool skipped = false;
  bool fetch_ok = false;
};

Sample& current();
void reset();
void logCycle(const char* tag);

inline uint32_t nowMs() { return static_cast<uint32_t>(millis()); }

}  // namespace debug::perf
