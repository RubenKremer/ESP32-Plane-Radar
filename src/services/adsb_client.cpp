#include "services/adsb_client.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "debug/perf_log.h"
#include "services/http_body_framing.h"
#include "ui/radar_range.h"

namespace services::adsb {

namespace {

constexpr char kApiBase[] = "https://opendata.adsb.fi/api/v3/lat/";
constexpr float kKmPerNm = 1.852f;
// A TLS handshake rarely completes in 200 ms; the old value made nearly every
// fresh connect fail and retry. A stalled connect now waits once, and the next
// poll tries again rather than retrying in a loop.
constexpr int kConnectTimeoutMs = 5000;
constexpr unsigned long kRequestTimeoutMs = 6000;

/** Dead-reckoning: cap the fix age taken from seen_pos, and the total time a
 *  position is projected forward, so a stale or turning aircraft isn't flung
 *  far off its real position (e.g. with a long poll interval). */
constexpr uint32_t kMaxPosAgeMs = 30000;
constexpr unsigned long kMaxExtrapolationMs = 60000;

Aircraft s_aircraft[kMaxAircraft];
size_t s_aircraft_count = 0;
unsigned long s_last_update_ms = 0;
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

/** Stream wrapper so ArduinoJson can parse the body without a full RAM copy. */
class PollingStream : public Stream {
 public:
  PollingStream(WiFiClient& client, unsigned long deadline)
      : client_(client), deadline_(deadline) {
    setTimeout(kRequestTimeoutMs);
  }

  size_t bytesRead() const { return bytes_read_; }

  int available() override {
    pump();
    return aborted_ ? 0 : client_.available();
  }

  int read() override {
    if (!waitAvailable()) {
      return -1;
    }
    const int c = client_.read();
    if (c >= 0) {
      ++bytes_read_;
    }
    return c;
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
      bytes_read_ += static_cast<size_t>(got);
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
  size_t bytes_read_ = 0;
  bool aborted_ = false;
};

struct AdsbSession {
  WiFiClientSecure tls;
  HTTPClient http;
  bool configured = false;

  void configure() {
    if (configured) {
      return;
    }
    tls.setInsecure();
    http.setReuse(true);
    http.setTimeout(kRequestTimeoutMs);
    http.setConnectTimeout(kConnectTimeoutMs);
    configured = true;
  }

  void drop() { tls.stop(); }

  bool beginUrl(const char* url) {
    configure();
    return http.begin(tls, url);
  }
};

AdsbSession s_session;

int performGet(const char* url) {
  auto attempt = [&]() -> int {
    if (fetchAborted()) {
      return kFetchAborted;
    }
    pollNetwork();
    if (!s_session.beginUrl(url)) {
      return HTTPC_ERROR_CONNECTION_REFUSED;
    }
    // HTTPClient only records Transfer-Encoding in the collected headers when
    // asked for it up front; its own _transferEncoding is private.
    static const char* kWantedHeaders[] = {"Transfer-Encoding"};
    s_session.http.collectHeaders(kWantedHeaders, 1);
    return s_session.http.GET();
  };

  // A kept-alive connection the server has since closed fails immediately;
  // that case is worth one reconnect. A fresh connect that failed already
  // waited up to kConnectTimeoutMs, so it is left for the next poll.
  const bool reusing_connection = s_session.tls.connected();

  int code = attempt();
  if (code == kFetchAborted) {
    s_session.drop();
    return code;
  }
  if (code > 0) {
    return code;
  }
  if (!reusing_connection ||
      (code != HTTPC_ERROR_CONNECTION_REFUSED &&
       code != HTTPC_ERROR_NOT_CONNECTED &&
       code != HTTPC_ERROR_CONNECTION_LOST)) {
    s_session.drop();
    return code;
  }

  s_session.drop();
  code = attempt();
  if (code == kFetchAborted || code <= 0) {
    s_session.drop();
  }
  return code;
}

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
  filter["ac"][0]["seen_pos"] = true;  // fix age, for dead-reckoning
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
  copyJsonStringTrimmed(plane, "hex", ac->hex, sizeof(ac->hex));
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

unsigned long lastUpdateMs() { return s_last_update_ms; }

void estimatedPosition(const Aircraft& plane, unsigned long now_ms, float* lat,
                       float* lon) {
  *lat = plane.lat;
  *lon = plane.lon;
  if (s_last_update_ms == 0 || plane.gs_knots <= 0.0f) {
    return;
  }
  unsigned long elapsed_ms = (now_ms - s_last_update_ms) + plane.pos_age_ms;
  if (elapsed_ms > kMaxExtrapolationMs) {
    elapsed_ms = kMaxExtrapolationMs;
  }
  const float dist_km = plane.gs_knots * kKmPerNm *
                        (static_cast<float>(elapsed_ms) / 3600000.0f);
  if (dist_km <= 0.0f) {
    return;
  }
  constexpr float kKmPerDeg = 111.0f;
  constexpr float kDegToRad = 0.01745329252f;
  const float track_rad = plane.track_deg * kDegToRad;  // 0 = N, 90 = E
  // A degree of longitude shrinks by cos(latitude).
  float cos_lat = cosf(plane.lat * kDegToRad);
  if (cos_lat < 0.01f) {
    cos_lat = 0.01f;
  }
  *lat = plane.lat + (dist_km * cosf(track_rad)) / kKmPerDeg;
  *lon = plane.lon + (dist_km * sinf(track_rad)) / (kKmPerDeg * cos_lat);
}

bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km) {
  const float dist_nm = kmToNauticalMiles(fetch_radius_km);
  auto& perf = debug::perf::current();

  if (WiFi.status() != WL_CONNECTED) {
    s_session.drop();
    return false;
  }

  char url[128];
  snprintf(url, sizeof(url), "%s%.6f/lon/%.6f/dist/%.1f", kApiBase, center_lat,
           center_lon, dist_nm);

  uint32_t t0 = debug::perf::nowMs();
  const int code = performGet(url);
  perf.http_begin_ms = 0;
  perf.http_get_ms = debug::perf::nowMs() - t0;
  perf.http_code = code;
  if (code == kFetchAborted) {
    s_session.drop();
    return false;
  }
  if (code != HTTP_CODE_OK) {
    Serial.printf("adsb: HTTP %d\n", code);
    s_session.drop();
    s_session.http.end();
    return false;
  }

  const int content_len = s_session.http.getSize();
  perf.content_len = content_len;

  WiFiClient* raw = s_session.http.getStreamPtr();
  if (raw == nullptr) {
    Serial.println("adsb: empty response");
    s_session.drop();
    s_session.http.end();
    return false;
  }

  JsonDocument filter;
  buildAircraftFilter(filter);

  // adsb.fi (behind Cloudflare) answers with Transfer-Encoding: chunked and no
  // Content-Length. getStreamPtr() is the raw socket, chunk-size lines and all,
  // so BodyFramer strips that framing before ArduinoJson sees it.
  const bool chunked =
      s_session.http.header("Transfer-Encoding").equalsIgnoreCase("chunked");
  const services::http::BodyFraming framing =
      chunked ? services::http::BodyFraming::kChunked
              : services::http::BodyFraming::kIdentity;

  const unsigned long body_deadline = millis() + kRequestTimeoutMs;
  PollingStream stream(*raw, body_deadline);
  services::http::BodyFramer<PollingStream> body(stream, framing, content_len);
  JsonDocument doc;
  t0 = debug::perf::nowMs();
  const DeserializationError err =
      deserializeJson(doc, body, DeserializationOption::Filter(filter));
  perf.json_ms = debug::perf::nowMs() - t0;

  if (stream.aborted() || err) {
    s_session.drop();
    s_session.http.end();
    if (stream.aborted()) {
      return false;
    }
    if (body.framingError()) {
      Serial.println("adsb: malformed chunked body");
    } else if (body.bytesRead() == 0) {
      Serial.println("adsb: empty response");
    } else {
      Serial.printf("adsb: JSON parse error: %s\n", err.c_str());
    }
    return false;
  }

  // The parser stops at the closing brace; consume the rest of the body (the
  // terminating 0-chunk, or the remaining Content-Length bytes) so the kept-
  // alive TLS connection is positioned at the start of the next response.
  // A close-delimited body can't be reused, so drop the connection then.
  if (!chunked && content_len < 0) {
    s_session.drop();
  } else {
    body.drain();
    if (!body.complete()) {
      Serial.println("adsb: body drain failed — dropping TLS");
      s_session.drop();
    }
  }
  s_session.http.end();

  // Base time for dead-reckoning: the fetched positions are current as of now
  // (less each aircraft's own pos_age_ms).
  s_last_update_ms = millis();

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

    s_aircraft[n].hex[0] = '\0';
    s_aircraft[n].lat = lat;
    s_aircraft[n].lon = lon;
    s_aircraft[n].nose_deg = pickNoseHeading(plane);
    s_aircraft[n].track_deg = pickTrackHeading(plane);
    s_aircraft[n].gs_knots = pickGroundSpeed(plane);

    // seen_pos: seconds since this position was measured (often 10-30 s).
    float seen_pos_s = 0.0f;
    readJsonFloat(plane, "seen_pos", &seen_pos_s);
    uint32_t age_ms =
        seen_pos_s > 0.0f ? static_cast<uint32_t>(seen_pos_s * 1000.0f) : 0;
    if (age_ms > kMaxPosAgeMs) {
      age_ms = kMaxPosAgeMs;
    }
    s_aircraft[n].pos_age_ms = age_ms;

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
