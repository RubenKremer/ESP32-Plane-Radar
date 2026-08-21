#include "debug/perf_log.h"

#include <Arduino.h>
#include <WiFi.h>

namespace debug::perf {
namespace {

Sample s_sample;

}  // namespace

Sample& current() { return s_sample; }

void reset() { s_sample = Sample{}; }

void logCycle(const char* tag) {
  if (!config::kEnablePerfLog) {
    return;
  }

  Sample& s = s_sample;
  s.heap_free = ESP.getFreeHeap();
  s.heap_min = ESP.getMinFreeHeap();
  s.heap_max_alloc = ESP.getMaxAllocHeap();
  if (WiFi.status() == WL_CONNECTED) {
    s.rssi = static_cast<int8_t>(WiFi.RSSI());
  }

  Serial.printf(
      "perf %s: total=%lu http_begin=%lu http_get=%lu json=%lu "
      "layout=%lu grid=%lu items=%lu blit=%lu skip=%u ok=%u "
      "ac=%u drawn=%u dots=%u http=%d len=%d "
      "heap=%lu min=%lu maxalloc=%lu rssi=%d\n",
      tag != nullptr ? tag : "-",
      static_cast<unsigned long>(s.total_ms),
      static_cast<unsigned long>(s.http_begin_ms),
      static_cast<unsigned long>(s.http_get_ms),
      static_cast<unsigned long>(s.json_ms),
      static_cast<unsigned long>(s.layout_ms),
      static_cast<unsigned long>(s.grid_ms),
      static_cast<unsigned long>(s.items_ms),
      static_cast<unsigned long>(s.blit_ms),
      s.skipped ? 1U : 0U, s.fetch_ok ? 1U : 0U,
      static_cast<unsigned>(s.aircraft), static_cast<unsigned>(s.drawn),
      static_cast<unsigned>(s.dots), s.http_code, s.content_len,
      static_cast<unsigned long>(s.heap_free),
      static_cast<unsigned long>(s.heap_min),
      static_cast<unsigned long>(s.heap_max_alloc),
      static_cast<int>(s.rssi));
}

}  // namespace debug::perf
