#include "services/radar_portal.h"

#include <WiFiManager.h>

#include "services/radar_location.h"
#include "ui/radar_range.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>

namespace services::radar_portal {

namespace {

constexpr char kPath[] = "/radar-settings";
constexpr size_t kCoordFieldLen = 20;

static const char kMenuLink[] =
    "<form action='/radar-settings' method='get'><button>Radar settings</button>"
    "</form><br/>\n";

static const char kPageStyle[] PROGMEM =
    "<style>"
    ".c,body,h1{text-align:center;font-family:verdana}"
    "div,select,input:not([type=checkbox]){padding:5px;font-size:1em;margin:5px 0;"
    "box-sizing:border-box;width:100%}"
    "button,select,input:not([type=checkbox]){border-radius:.3rem}"
    "input:not([type=checkbox]){border:1px solid #ccc}"
    "button{background-color:#1fa3ec;color:#fff;line-height:2.4rem;font-size:1.2rem;"
    "cursor:pointer;border:0;width:100%}"
    ".wrap{text-align:left;display:inline-block;min-width:260px;max-width:500px}"
    "label.cb{display:block;margin:8px 0}"
    "label.cb input{width:auto;margin-right:8px}"
    ".msg{padding:20px;margin:20px 0;border:1px solid #eee;border-left-width:5px;"
    "border-left-color:#777}"
    ".msg.D{border-left-color:#dc3630}"
    ".msg.S{border-left-color:#5cb85c}"
    "</style>";

struct FormState {
  uint8_t range_idx;
  uint8_t poll_idx;
  char lat[kCoordFieldLen + 1];
  char lon[kCoordFieldLen + 1];
  bool use_miles;
  bool show_runways;
  bool show_airport_labels;
};

WiFiManager* s_wm = nullptr;

void loadFormDefaults(FormState* state) {
  state->range_idx = ui::radar::rangeIndex();
  state->poll_idx = ui::radar::pollIntervalIndex();
  snprintf(state->lat, sizeof(state->lat), "%.6f", services::location::lat());
  snprintf(state->lon, sizeof(state->lon), "%.6f", services::location::lon());
  state->use_miles = ui::radar::useMiles();
  state->show_runways = ui::radar::showRunways();
  state->show_airport_labels = ui::radar::showAirportLabels();
}

String pageHead(const char* title) {
  String page = F("<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
                  "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
  page += FPSTR(kPageStyle);
  page += F("<title>");
  page += title;
  page += F("</title></head><body><div class=\"wrap\"><h1>");
  page += title;
  page += F("</h1>");
  return page;
}

String pageFoot() {
  return F("<hr><br/><form action='/' method='get'><button>Back</button></form>"
           "</div></body></html>");
}

String buildRangeOptions(uint8_t selected) {
  String opts;
  char label[24];
  for (size_t i = 0; i < ui::radar::kRangePresetCount; ++i) {
    ui::radar::formatRing3PresetOption(label, sizeof(label),
                                       ui::radar::kRangePresets[i].ring3_km);
    opts += F("<option value=\"");
    opts += static_cast<int>(i);
    opts += F("\"");
    if (i == selected) {
      opts += F(" selected");
    }
    opts += F(">");
    opts += label;
    opts += F("</option>");
  }
  return opts;
}

String buildPollOptions(uint8_t selected) {
  String opts;
  char label[16];
  for (size_t i = 0; i < ui::radar::kPollIntervalPresetCount; ++i) {
    ui::radar::formatPollIntervalOption(
        label, sizeof(label), ui::radar::kPollIntervalPresetsMs[i]);
    opts += F("<option value=\"");
    opts += static_cast<int>(i);
    opts += F("\"");
    if (i == selected) {
      opts += F(" selected");
    }
    opts += F(">");
    opts += label;
    opts += F("</option>");
  }
  return opts;
}

String buildFormPage(const FormState& state, const char* error_msg) {
  String page = pageHead("Radar settings");
  if (error_msg != nullptr && error_msg[0] != '\0') {
    page += F("<div class=\"msg D\">");
    page += error_msg;
    page += F("</div>");
  }
  page += F("<form method=\"POST\" action=\"");
  page += kPath;
  page += F("\"><label for=\"radar_lat\">Latitude (deg)</label>"
            "<input id=\"radar_lat\" name=\"radar_lat\" type=\"number\" step=\"0.000001\" "
            "maxlength=\"");
  page += static_cast<int>(kCoordFieldLen);
  page += F("\" value=\"");
  page += state.lat;
  page += F("\"><label for=\"radar_lon\">Longitude (deg)</label>"
            "<input id=\"radar_lon\" name=\"radar_lon\" type=\"number\" step=\"0.000001\" "
            "maxlength=\"");
  page += static_cast<int>(kCoordFieldLen);
  page += F("\" value=\"");
  page += state.lon;
  page += F("\"><label class=\"cb\"><input type=\"checkbox\" name=\"use_miles\" value=\"T\"");
  if (state.use_miles) {
    page += F(" checked");
  }
  page += F("> Display distances in miles</label>"
            "<label class=\"cb\"><input type=\"checkbox\" name=\"show_runways\" value=\"T\"");
  if (state.show_runways) {
    page += F(" checked");
  }
  page += F("> Show airport runways</label>"
            "<label class=\"cb\"><input type=\"checkbox\" name=\"show_airport_labels\" value=\"T\"");
  if (state.show_airport_labels) {
    page += F(" checked");
  }
  page += F("> Show airport ICAO labels</label>"
            "<label for=\"range_idx\">Range ring label</label>"
            "<select name=\"range_idx\" id=\"range_idx\">");
  page += buildRangeOptions(state.range_idx);
  page += F("</select><label for=\"poll_idx\">Poll interval</label>"
            "<select name=\"poll_idx\" id=\"poll_idx\">");
  page += buildPollOptions(state.poll_idx);
  page += F("</select><br/><button type=\"submit\">Save</button></form>");
  page += pageFoot();
  return page;
}

String buildSavedPage() {
  String page = pageHead("Radar settings");
  page += F("<div class=\"msg S\">Saved</div>");
  page += pageFoot();
  return page;
}

bool parseRangeIndex(const String& arg, uint8_t* out) {
  if (arg.length() == 0) {
    return false;
  }
  char* end = nullptr;
  errno = 0;
  const unsigned long val = strtoul(arg.c_str(), &end, 10);
  if (end == arg.c_str() || (end != nullptr && *end != '\0') || errno == ERANGE ||
      val >= ui::radar::kRangePresetCount) {
    return false;
  }
  *out = static_cast<uint8_t>(val);
  return true;
}

bool parsePollIndex(const String& arg, uint8_t* out) {
  if (arg.length() == 0) {
    return false;
  }
  char* end = nullptr;
  errno = 0;
  const unsigned long val = strtoul(arg.c_str(), &end, 10);
  if (end == arg.c_str() || (end != nullptr && *end != '\0') || errno == ERANGE ||
      val >= ui::radar::kPollIntervalPresetCount) {
    return false;
  }
  *out = static_cast<uint8_t>(val);
  return true;
}

void copyField(const String& arg, char* dest, size_t dest_len) {
  if (dest_len == 0) {
    return;
  }
  arg.toCharArray(dest, dest_len);
}

bool readPostForm(FormState* state, bool* range_ok, bool* poll_ok, bool* location_ok) {
  FormState defaults = {};
  loadFormDefaults(&defaults);
  *state = defaults;

  *range_ok = parseRangeIndex(s_wm->server->arg("range_idx"), &state->range_idx);
  *poll_ok = parsePollIndex(s_wm->server->arg("poll_idx"), &state->poll_idx);

  copyField(s_wm->server->arg("radar_lat"), state->lat, sizeof(state->lat));
  copyField(s_wm->server->arg("radar_lon"), state->lon, sizeof(state->lon));
  *location_ok = services::location::validateStrings(state->lat, state->lon);

  state->use_miles = s_wm->server->hasArg("use_miles");
  state->show_runways = s_wm->server->hasArg("show_runways");
  state->show_airport_labels = s_wm->server->hasArg("show_airport_labels");
  return true;
}

FormState mergeDisplayState(const FormState& submitted, bool range_ok, bool poll_ok) {
  FormState display = submitted;
  FormState defaults = {};
  loadFormDefaults(&defaults);
  if (!range_ok) {
    display.range_idx = defaults.range_idx;
  }
  if (!poll_ok) {
    display.poll_idx = defaults.poll_idx;
  }
  return display;
}

void saveFormState(const FormState& state) {
  services::location::saveFromStrings(state.lat, state.lon);
  ui::radar::saveMilesFromPortal(state.use_miles ? "T" : "");
  ui::radar::saveRunwaysFromPortal(state.show_runways ? "T" : "");
  ui::radar::saveAirportLabelsFromPortal(state.show_airport_labels ? "T" : "");
  ui::radar::setRangeIndex(state.range_idx);
  ui::radar::setPollIntervalIndex(state.poll_idx);
}

void handleGet() {
  if (s_wm == nullptr || s_wm->server == nullptr) {
    return;
  }
  FormState state = {};
  loadFormDefaults(&state);
  s_wm->server->send(200, "text/html", buildFormPage(state, nullptr));
}

void handlePost() {
  if (s_wm == nullptr || s_wm->server == nullptr) {
    return;
  }

  FormState submitted = {};
  bool range_ok = false;
  bool poll_ok = false;
  bool location_ok = false;
  readPostForm(&submitted, &range_ok, &poll_ok, &location_ok);

  if (!range_ok || !poll_ok || !location_ok) {
    const FormState display = mergeDisplayState(submitted, range_ok, poll_ok);
    s_wm->server->send(200, "text/html",
                        buildFormPage(display, "Invalid settings."));
    return;
  }

  saveFormState(submitted);
  s_wm->server->send(200, "text/html", buildSavedPage());
}

}  // namespace

const char* menuLinkHtml() { return kMenuLink; }

void registerRoutes(WiFiManager& wm) {
  s_wm = &wm;
  if (wm.server == nullptr) {
    return;
  }
  wm.server->on(kPath, HTTP_GET, handleGet);
  wm.server->on(kPath, HTTP_POST, handlePost);
}

}  // namespace services::radar_portal
