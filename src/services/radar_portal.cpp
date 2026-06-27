#include "services/radar_portal.h"

#include <WiFiManager.h>

#include "ui/radar_range.h"

#include <cerrno>
#include <cstdlib>

namespace services::radar_portal {

namespace {

constexpr char kPath[] = "/radar-settings";

static const char kMenuLink[] =
    "<form action='/radar-settings' method='get'><button>Radar settings</button>"
    "</form><br/>\n";

static const char kPageStyle[] PROGMEM =
    "<style>"
    ".c,body,h1{text-align:center;font-family:verdana}"
    "div,select{padding:5px;font-size:1em;margin:5px 0;box-sizing:border-box;width:100%}"
    "button,select{border-radius:.3rem}"
    "button{background-color:#1fa3ec;color:#fff;line-height:2.4rem;font-size:1.2rem;"
    "cursor:pointer;border:0;width:100%}"
    ".wrap{text-align:left;display:inline-block;min-width:260px;max-width:500px}"
    "a{color:#000;font-weight:700;text-decoration:none}"
    "a:hover{color:#1fa3ec;text-decoration:underline}"
    ".msg{padding:20px;margin:20px 0;border:1px solid #eee;border-left-width:5px;"
    "border-left-color:#777}"
    ".msg.D{border-left-color:#dc3630}"
    ".msg.S{border-left-color:#5cb85c}"
    "</style>";

WiFiManager* s_wm = nullptr;

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

String pageFoot() { return F("<br/><a href=\"/\">&laquo; Back</a></div></body></html>"); }

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

String buildFormPage(uint8_t range_idx, uint8_t poll_idx, const char* error_msg) {
  String page = pageHead("Radar settings");
  if (error_msg != nullptr && error_msg[0] != '\0') {
    page += F("<div class=\"msg D\">");
    page += error_msg;
    page += F("</div>");
  }
  page += F("<form method=\"POST\" action=\"");
  page += kPath;
  page += F("\"><label for=\"range_idx\">Range ring label</label>"
            "<select name=\"range_idx\" id=\"range_idx\">");
  page += buildRangeOptions(range_idx);
  page += F("</select><br/><label for=\"poll_idx\">Poll interval</label>"
            "<select name=\"poll_idx\" id=\"poll_idx\">");
  page += buildPollOptions(poll_idx);
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

void handleGet() {
  if (s_wm == nullptr || s_wm->server == nullptr) {
    return;
  }
  s_wm->server->send(
      200, "text/html",
      buildFormPage(ui::radar::rangeIndex(), ui::radar::pollIntervalIndex(), nullptr));
}

void handlePost() {
  if (s_wm == nullptr || s_wm->server == nullptr) {
    return;
  }

  uint8_t range_idx = 0;
  uint8_t poll_idx = 0;
  const bool range_ok = parseRangeIndex(s_wm->server->arg("range_idx"), &range_idx);
  const bool poll_ok = parsePollIndex(s_wm->server->arg("poll_idx"), &poll_idx);

  if (!range_ok || !poll_ok) {
    const uint8_t display_range =
        range_ok ? range_idx : ui::radar::rangeIndex();
    const uint8_t display_poll =
        poll_ok ? poll_idx : ui::radar::pollIntervalIndex();
    s_wm->server->send(200, "text/html",
                        buildFormPage(display_range, display_poll, "Invalid settings."));
    return;
  }

  ui::radar::setRangeIndex(range_idx);
  ui::radar::setPollIntervalIndex(poll_idx);
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
