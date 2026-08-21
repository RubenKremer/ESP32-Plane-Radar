#include "services/adsb_client.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>

#include "config.h"
#include "debug/perf_log.h"
#include "ui/radar_range.h"

namespace services::adsb {

namespace {

constexpr char kApiBase[] = "https://opendata.adsb.fi/api/v3/lat/";
constexpr float kKmPerNm = 1.852f;
constexpr int kConnectAttemptMs = 200;
constexpr unsigned long kRequestTimeoutMs = 10000;

Aircraft s_aircraft[kMaxAircraft];
size_t s_aircraft_count = 0;
PollFn s_poll_fn = nullptr;
AbortFn s_abort_fn = nullptr;

constexpr int kFetchAborted = -1000;

void pollNetwork() {
  if (s_poll_fn != nullptr) {
    s_poll_fn();
  }
}

bool fetchAborted() {
  return s_abort_fn != nullptr && s_abort_fn();
}

int performGetWithPoll(HTTPClient& http) {
  http.setConnectTimeout(kConnectAttemptMs);
  const unsigned long deadline = millis() + kRequestTimeoutMs;
  while (millis() < deadline) {
    if (fetchAborted()) {
      return kFetchAborted;
    }
    pollNetwork();
    const int code = http.GET();
    if (code > 0) {
      return code;
    }
    if (code != HTTPC_ERROR_CONNECTION_REFUSED &&
        code != HTTPC_ERROR_NOT_CONNECTED) {
      return code;
    }
    delay(5);
  }
  return HTTPC_ERROR_READ_TIMEOUT;
}

/** Stream wrapper so ArduinoJson can parse the body without a full RAM copy. */
class PollingStream : public Stream {
 public:
  PollingStream(WiFiClient& client, unsigned long deadline)
      : client_(client), deadline_(deadline) {
    setTimeout(kRequestTimeoutMs);
  }

  int available() override {
    pump();
    return aborted_ ? 0 : client_.available();
  }

  int read() override {
    if (!waitAvailable()) {
      return -1;
    }
    return client_.read();
  }

  int peek() override {
    if (!waitAvailable()) {
      return -1;
    }
    return client_.peek();
  }

  size_t readBytes(char* buffer, size_t length) override {
    size_t n = 0;
    while (n < length) {
      if (!waitAvailable()) {
        break;
      }
      const int avail = client_.available();
      size_t chunk = static_cast<size_t>(avail);
      if (chunk > length - n) {
        chunk = length - n;
      }
      const int got =
          client_.read(reinterpret_cast<uint8_t*>(buffer + n), chunk);
      if (got <= 0) {
        break;
      }
      n += static_cast<size_t>(got);
    }
    return n;
  }

  size_t write(uint8_t) override { return 0; }
  void flush() override {}
  bool aborted() const { return aborted_; }

 private:
  void pump() {
    if (aborted_) {
      return;
    }
    if (fetchAborted() || millis() > deadline_) {
      aborted_ = true;
      return;
    }
    pollNetwork();
  }

  bool waitAvailable() {
    while (!aborted_ && client_.available() <= 0) {
      pump();
      if (aborted_) {
        return false;
      }
      if (!client_.connected() && client_.available() <= 0) {
        return false;
      }
      delay(1);
    }
    pump();
    return !aborted_ && client_.available() > 0;
  }

  WiFiClient& client_;
  unsigned long deadline_;
  bool aborted_ = false;
};

void buildAircraftFilter(JsonDocument& filter) {
  filter["ac"][0]["lat"] = true;
  filter["ac"][0]["lon"] = true;
  filter["ac"][0]["true_heading"] = true;
  filter["ac"][0]["mag_heading"] = true;
  filter["ac"][0]["track"] = true;
  filter["ac"][0]["dir"] = true;
  filter["ac"][0]["gs"] = true;
  filter["ac"][0]["tas"] = true;
  filter["ac"][0]["ias"] = true;
  filter["ac"][0]["flight"] = true;
  filter["ac"][0]["hex"] = true;
  filter["ac"][0]["t"] = true;
  filter["ac"][0]["alt_baro"] = true;
  filter["ac"][0]["alt_geom"] = true;
}

float kmToNauticalMiles(float km) { return km / kKmPerNm; }

bool readJsonFloat(const JsonObject& obj, const char* key, float* out) {
  if (obj[key].is<float>() || obj[key].is<double>() || obj[key].is<int>()) {
    *out = obj[key].as<float>();
    return true;
  }
  return false;
}

float pickNoseHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickTrackHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickGroundSpeed(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "gs", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "tas", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "ias", &v)) {
    return v;
  }
  return 0.0f;
}

bool isOnGround(const JsonObject& plane) {
  if (!plane["alt_baro"].is<const char*>()) {
    return false;
  }
  return strcmp(plane["alt_baro"].as<const char*>(), "ground") == 0;
}

void copyJsonStringTrimmed(const JsonObject& obj, const char* key, char* out,
                           size_t out_len) {
  out[0] = '\0';
  if (out_len == 0 || !obj[key].is<const char*>()) {
    return;
  }
  const char* s = obj[key].as<const char*>();
  size_t n = strnlen(s, out_len - 1);
  while (n > 0 && s[n - 1] == ' ') {
    --n;
  }
  memcpy(out, s, n);
  out[n] = '\0';
}

void formatAltitudeTag(const JsonObject& plane, char* out, size_t out_len) {
  out[0] = '\0';
  if (out_len == 0) {
    return;
  }

  if (plane["alt_baro"].is<const char*>()) {
    const char* s = plane["alt_baro"].as<const char*>();
    if (strcmp(s, "ground") == 0) {
      strncpy(out, "GND", out_len - 1);
      out[out_len - 1] = '\0';
      return;
    }
  }

  float alt = 0.0f;
  if (readJsonFloat(plane, "alt_baro", &alt) ||
      readJsonFloat(plane, "alt_geom", &alt)) {
    if (ui::radar::useFeetForAltitude()) {
      snprintf(out, out_len, "%d ft", static_cast<int>(lroundf(alt)));
    } else {
      const int meters =
          static_cast<int>(lroundf(alt * 0.3048f));
      snprintf(out, out_len, "%d m", meters);
    }
  }
}

void fillTagFields(Aircraft* ac, const JsonObject& plane) {
  copyJsonStringTrimmed(plane, "flight", ac->callsign, sizeof(ac->callsign));
  if (ac->callsign[0] == '\0') {
    copyJsonStringTrimmed(plane, "hex", ac->callsign, sizeof(ac->callsign));
  }

  copyJsonStringTrimmed(plane, "t", ac->type, sizeof(ac->type));
  formatAltitudeTag(plane, ac->alt, sizeof(ac->alt));
}

}  // namespace

void setPollFn(PollFn fn) { s_poll_fn = fn; }

void setAbortFn(AbortFn fn) { s_abort_fn = fn; }

size_t aircraftCount() { return s_aircraft_count; }

const Aircraft* aircraftList() { return s_aircraft; }

bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km) {
  const float dist_nm = kmToNauticalMiles(fetch_radius_km);
  auto& perf = debug::perf::current();

  char url[128];
  snprintf(url, sizeof(url), "%s%.6f/lon/%.6f/dist/%.1f", kApiBase, center_lat,
           center_lon, dist_nm);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  uint32_t t0 = debug::perf::nowMs();
  if (!http.begin(client, url)) {
    perf.http_begin_ms = debug::perf::nowMs() - t0;
    Serial.println("adsb: http.begin failed");
    return false;
  }
  perf.http_begin_ms = debug::perf::nowMs() - t0;

  http.setTimeout(kRequestTimeoutMs);
  t0 = debug::perf::nowMs();
  const int code = performGetWithPoll(http);
  perf.http_get_ms = debug::perf::nowMs() - t0;
  perf.http_code = code;
  if (code == kFetchAborted) {
    http.end();
    return false;
  }
  if (code != HTTP_CODE_OK) {
    Serial.printf("adsb: HTTP %d\n", code);
    http.end();
    return false;
  }

  perf.content_len = http.getSize();

  WiFiClient* raw = http.getStreamPtr();
  if (raw == nullptr) {
    http.end();
    Serial.println("adsb: empty response");
    return false;
  }

  JsonDocument filter;
  buildAircraftFilter(filter);

  PollingStream stream(*raw, millis() + kRequestTimeoutMs);
  JsonDocument doc;
  t0 = debug::perf::nowMs();
  const DeserializationError err =
      deserializeJson(doc, stream, DeserializationOption::Filter(filter));
  perf.json_ms = debug::perf::nowMs() - t0;
  http.end();

  if (stream.aborted()) {
    return false;
  }
  if (err) {
    Serial.printf("adsb: JSON parse error: %s\n", err.c_str());
    return false;
  }

  JsonArray ac = doc["ac"].as<JsonArray>();
  if (ac.isNull()) {
    s_aircraft_count = 0;
    perf.aircraft = 0;
    perf.fetch_ok = true;
    return true;
  }

  size_t n = 0;
  for (JsonObject plane : ac) {
    if (n >= kMaxAircraft) {
      break;
    }
    float lat = 0.0f;
    float lon = 0.0f;
    if (!readJsonFloat(plane, "lat", &lat) ||
        !readJsonFloat(plane, "lon", &lon)) {
      continue;
    }
    if (isOnGround(plane) && !config::kAdsbShowGroundAircraft) {
      continue;
    }

    s_aircraft[n].lat = lat;
    s_aircraft[n].lon = lon;
    s_aircraft[n].nose_deg = pickNoseHeading(plane);
    s_aircraft[n].track_deg = pickTrackHeading(plane);
    s_aircraft[n].gs_knots = pickGroundSpeed(plane);
    fillTagFields(&s_aircraft[n], plane);
    ++n;
  }

  s_aircraft_count = n;
  perf.aircraft = static_cast<uint16_t>(n);
  perf.fetch_ok = true;
  if (!config::kEnablePerfLog) {
    Serial.printf("adsb: %u aircraft\n", static_cast<unsigned>(n));
  }
  return true;
}

}  // namespace services::adsb
