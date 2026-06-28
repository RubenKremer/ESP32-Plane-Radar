#include "services/wifi_radio.h"

#include <WiFi.h>

#include <Preferences.h>
#include <cstdio>
#include <cstdlib>

namespace services::wifi_radio {

namespace {

constexpr char kPrefsNamespace[] = "wifi";
constexpr char kPrefsTxPowerKey[] = "txPwrIdx";

struct TxPowerPreset {
  const char* label;
  wifi_power_t power;
};

constexpr TxPowerPreset kTxPowerPresets[] = {
    {"High (19.5 dBm)", WIFI_POWER_19_5dBm},
    {"Medium (15 dBm)", WIFI_POWER_15dBm},
    {"Low (11 dBm)", WIFI_POWER_11dBm},
    {"Minimum (8.5 dBm)", WIFI_POWER_8_5dBm},
};

uint8_t s_tx_power_index = kDefaultTxPowerIndex;

void saveTxPowerIndex() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  prefs.putUChar(kPrefsTxPowerKey, s_tx_power_index);
  prefs.end();
}

}  // namespace

void wifiRadioInit() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, true)) {
    return;
  }
  const uint8_t saved = prefs.getUChar(kPrefsTxPowerKey, kDefaultTxPowerIndex);
  prefs.end();
  s_tx_power_index =
      (saved < kTxPowerPresetCount) ? saved : kDefaultTxPowerIndex;
}

uint8_t txPowerIndex() { return s_tx_power_index; }

bool setTxPowerIndex(uint8_t idx) {
  if (idx >= kTxPowerPresetCount) {
    return false;
  }
  if (idx == s_tx_power_index) {
    applyTxPower();
    return true;
  }
  s_tx_power_index = idx;
  saveTxPowerIndex();
  applyTxPower();
  Serial.printf("WiFi TX power: %s\n", kTxPowerPresets[idx].label);
  return true;
}

void applyTxPower() {
  if (WiFi.getMode() == WIFI_OFF) {
    return;
  }
  const wifi_power_t power = kTxPowerPresets[s_tx_power_index].power;
  if (!WiFi.setTxPower(power)) {
    Serial.println("WiFi setTxPower failed");
  }
}

void formatTxPowerOption(char* buf, size_t len, uint8_t idx) {
  if (buf == nullptr || len == 0) {
    return;
  }
  if (idx >= kTxPowerPresetCount) {
    buf[0] = '\0';
    return;
  }
  snprintf(buf, len, "%s", kTxPowerPresets[idx].label);
}

bool parseTxPowerIndex(const char* arg, uint8_t* out) {
  if (arg == nullptr || out == nullptr || arg[0] == '\0') {
    return false;
  }
  char* end = nullptr;
  const unsigned long val = strtoul(arg, &end, 10);
  if (end == arg || (end != nullptr && *end != '\0') ||
      val >= kTxPowerPresetCount) {
    return false;
  }
  *out = static_cast<uint8_t>(val);
  return true;
}

void buildTxPowerSelectHtml(char* buf, size_t len) {
  if (buf == nullptr || len == 0) {
    return;
  }
  size_t used = 0;
  int n = snprintf(buf, len,
                   "<label for=\"tx_pwr_idx\">Radio TX power</label>"
                   "<select name=\"tx_pwr_idx\" id=\"tx_pwr_idx\">");
  if (n < 0 || static_cast<size_t>(n) >= len) {
    buf[0] = '\0';
    return;
  }
  used = static_cast<size_t>(n);

  char label[32];
  for (size_t i = 0; i < kTxPowerPresetCount; ++i) {
    formatTxPowerOption(label, sizeof(label), static_cast<uint8_t>(i));
    n = snprintf(buf + used, len - used,
                 "<option value=\"%u\"%s>%s</option>", static_cast<unsigned>(i),
                 (i == s_tx_power_index) ? " selected" : "", label);
    if (n < 0 || used + static_cast<size_t>(n) >= len) {
      buf[0] = '\0';
      return;
    }
    used += static_cast<size_t>(n);
  }

  snprintf(buf + used, len - used, "</select><br/>");
}

bool saveTxPowerFromPortalForm(const char* arg) {
  uint8_t idx = 0;
  if (!parseTxPowerIndex(arg, &idx)) {
    return false;
  }
  return setTxPowerIndex(idx);
}

}  // namespace services::wifi_radio
