#include "services/wifi_setup.h"

#include <WiFi.h>
#include "services/wifi_manager_ex.h"

#include <cstdint>
#include <cstring>

#include <Preferences.h>
#include <esp_system.h>
#include <esp_wifi.h>

#ifdef WM_MDNS
#include <ESPmDNS.h>
#endif

#include "config.h"
#include "services/radar_location.h"
#include "services/radar_portal.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

portMUX_TYPE s_boot_mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool s_boot_tap_pending = false;
volatile bool s_boot_is_down = false;
volatile unsigned long s_boot_down_ms = 0;
bool s_boot_interrupt_attached = false;

void IRAM_ATTR onBootButtonIsr() {
  const bool down = digitalRead(config::kBootPin) == LOW;
  const unsigned long now = millis();
  portENTER_CRITICAL_ISR(&s_boot_mux);
  if (down) {
    s_boot_is_down = true;
    s_boot_down_ms = now;
  } else if (s_boot_is_down) {
    const unsigned long held = now - s_boot_down_ms;
    if (held >= config::kBootTapMinMs && held < config::kBootPortalHoldMs) {
      s_boot_tap_pending = true;
    }
    s_boot_is_down = false;
  }
  portEXIT_CRITICAL_ISR(&s_boot_mux);
}

void initBootButton() {
  pinMode(config::kBootPin, INPUT_PULLUP);
  if (s_boot_interrupt_attached) {
    return;
  }
  attachInterrupt(digitalPinToInterrupt(static_cast<uint8_t>(config::kBootPin)),
                  onBootButtonIsr, CHANGE);
  s_boot_interrupt_attached = true;
}

namespace {

/** Separate from planeradar prefs (rangeInit) to avoid NVS handle conflicts. */
constexpr char kWifiPrefsNamespace[] = "wifi";
constexpr char kPrefsForcePortalKey[] = "portal";

bool s_force_config_portal = false;
WiFiManagerEx s_wm;
bool s_wm_configured = false;
bool s_portal_enabled = false;

enum HoldStage : uint8_t {
  kHoldNone = 0,
  kHoldPortal = 1,
  kHoldFactoryReset = 2,
};

enum PortalStatusKind : uint8_t {
  kPortalStatusNone = 0,
  kPortalStatusEnabled,
  kPortalStatusDisabling,
};

bool s_hold_tracking = false;
unsigned long s_hold_down_ms = 0;
HoldStage s_hold_stage = kHoldNone;
bool s_hold_screen_active = false;
bool s_portal_disable_on_release = false;
bool s_pending_radar_redraw = false;
bool s_portal_status_pending_dismiss = false;
unsigned long s_portal_screen_shown_ms = 0;
PortalStatusKind s_portal_status_kind = kPortalStatusNone;
char s_portal_status_ip[16] = {};

void pollPortalStatusDismiss();
void showPortalStatusScreen(PortalStatusKind kind, const char* ip);
void redrawPortalStatusScreen();
void clearPortalStatusScreen();

void enableLanPortal();
void disableLanPortal();
void enterPortalHoldStage();
void enterFactoryResetHoldStage();
void finishHoldOnRelease();

void ensureWifiManager();
void applyApPortalMenu();
void applyLanPortalMenu();
void startLanWebPortal();
void stopLanWebPortal();
bool wifiLinkUp();

static const char* kMenuAp[] = {
    "wifi", "info", "custom", "exit", "sep", "update",
};
static const char* kMenuLan[] = {
    "wifi", "info", "custom", "restart", "sep", "update",
};

void markForceConfigPortal() {
  s_force_config_portal = true;
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, false)) {
    return;
  }
  prefs.putBool(kPrefsForcePortalKey, true);
  prefs.end();
}

bool consumeForceConfigPortal() {
  if (s_force_config_portal) {
    s_force_config_portal = false;
    Preferences prefs;
    if (prefs.begin(kWifiPrefsNamespace, false)) {
      prefs.remove(kPrefsForcePortalKey);
      prefs.end();
    }
    return true;
  }

  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  if (!pending) {
    return false;
  }

  if (prefs.begin(kWifiPrefsNamespace, false)) {
    prefs.remove(kPrefsForcePortalKey);
    prefs.end();
  }
  return true;
}

bool storedWifiCredentials() {
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
    WiFi.mode(WIFI_STA);
    delay(50);
  }

  wifi_config_t conf = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) {
    return false;
  }
  return conf.sta.ssid[0] != '\0';
}

void eraseWifiCredentials() {
  s_portal_enabled = false;
  stopLanWebPortal();
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_OFF);
  delay(100);

  ensureWifiManager();
  WiFi.persistent(true);
  s_wm.resetSettings();
  s_wm.erase();
  WiFi.disconnect(true, true);
  WiFi.persistent(false);

  WiFi.mode(WIFI_OFF);
  delay(100);
}

void resetWifiCredentials() {
  markForceConfigPortal();
  eraseWifiCredentials();
  services::location::clear();
  ui::radar::unitsReset();
  Serial.println("WiFi credentials, location, and units cleared");
}

void onConfigPortalApStarted(WiFiManager*) {
  statusScreenPortal();
#ifdef WM_MDNS
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("Setup portal: http://%s.local (or http://%s)\n",
                  config::kPortalHostname, config::kPortalIp);
  } else {
    Serial.printf("Setup portal: http://%s (mDNS unavailable)\n", config::kPortalIp);
  }
#else
  Serial.printf("Setup portal: http://%s\n", config::kPortalIp);
#endif
}

bool wifiLinkUp() {
  return WiFi.status() == WL_CONNECTED &&
         WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

void applyApPortalMenu() {
  s_wm.setMenu(kMenuAp, sizeof(kMenuAp) / sizeof(kMenuAp[0]));
  s_wm.setCustomMenuHTML(services::radar_portal::menuLinkHtml());
  s_wm.setShowBack(false);
}

void applyLanPortalMenu() {
  s_wm.setMenu(kMenuLan, sizeof(kMenuLan) / sizeof(kMenuLan[0]));
  s_wm.setCustomMenuHTML(services::radar_portal::menuLinkHtml());
  s_wm.setShowBack(true);
}

void ensureWifiManager() {
  if (s_wm_configured) {
    return;
  }
  s_wm.setConfigPortalTimeout(config::kWifiPortalTimeoutSec);
  s_wm.setAPStaticIPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                           IPAddress(255, 255, 255, 0));
  s_wm.setHostname(config::kPortalHostname);
  s_wm.setTitle(config::kPortalTitle);
  s_wm.setAPCallback(onConfigPortalApStarted);
  s_wm.setWebServerCallback([]() { services::radar_portal::registerRoutes(s_wm); });
  s_wm_configured = true;
}

void startLanWebPortal() {
  if (!wifiLinkUp() || s_wm.getWebPortalActive() ||
      s_wm.getConfigPortalActive()) {
    return;
  }
  WiFi.mode(WIFI_STA);
  s_wm.setConfigPortalBlocking(false);
  applyLanPortalMenu();
#ifdef WM_MDNS
  MDNS.end();
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.addService("http", "tcp", 80);
  }
#endif
  s_wm.startWebPortal();
  Serial.printf("LAN config: http://%s.local or http://%s\n",
                config::kPortalHostname, WiFi.localIP().toString().c_str());
}

void stopLanWebPortal() {
  if (!s_wm.getWebPortalActive()) {
    return;
  }
  s_wm.stopWebPortal();
#ifdef WM_MDNS
  MDNS.end();
#endif
}

void enableLanPortal() {
  s_portal_enabled = true;
  if (wifiLinkUp()) {
    startLanWebPortal();
  }
  Serial.println("LAN portal enabled");
}

void disableLanPortal() {
  s_portal_enabled = false;
  stopLanWebPortal();
  Serial.println("LAN portal disabled");
}

void showPortalStatusScreen(PortalStatusKind kind, const char* ip) {
  s_portal_status_kind = kind;
  s_portal_status_ip[0] = '\0';
  if (ip != nullptr && ip[0] != '\0') {
    strncpy(s_portal_status_ip, ip, sizeof(s_portal_status_ip) - 1);
    s_portal_status_ip[sizeof(s_portal_status_ip) - 1] = '\0';
  }
  s_portal_screen_shown_ms = millis();
  if (kind == kPortalStatusEnabled) {
    statusScreenPortalEnabled(s_portal_status_ip);
  } else if (kind == kPortalStatusDisabling) {
    statusScreenPortalDisabling();
  }
}

void redrawPortalStatusScreen() {
  if (s_portal_status_kind == kPortalStatusEnabled) {
    statusScreenPortalEnabled(s_portal_status_ip);
  } else if (s_portal_status_kind == kPortalStatusDisabling) {
    statusScreenPortalDisabling();
  }
}

void clearPortalStatusScreen() {
  s_portal_status_kind = kPortalStatusNone;
  s_portal_status_ip[0] = '\0';
  s_portal_screen_shown_ms = 0;
}

void enterPortalHoldStage() {
  s_hold_stage = kHoldPortal;
  if (!wifiLinkUp()) {
    return;
  }
  s_hold_screen_active = true;
  if (s_portal_enabled) {
    s_portal_disable_on_release = true;
    showPortalStatusScreen(kPortalStatusDisabling, nullptr);
  } else {
    s_portal_disable_on_release = false;
    enableLanPortal();
    static char ip_buf[16];
    WiFi.localIP().toString().toCharArray(ip_buf, sizeof(ip_buf));
    showPortalStatusScreen(kPortalStatusEnabled, ip_buf);
  }
}

void enterFactoryResetHoldStage() {
  s_portal_status_pending_dismiss = false;
  clearPortalStatusScreen();
  s_hold_stage = kHoldFactoryReset;
  s_hold_screen_active = true;
  statusScreenFactoryResetPending();
}

void pollPortalStatusDismiss() {
  if (!s_portal_status_pending_dismiss || s_portal_screen_shown_ms == 0) {
    return;
  }
  redrawPortalStatusScreen();
  if (millis() - s_portal_screen_shown_ms < config::kPortalStatusMinDisplayMs) {
    return;
  }
  s_portal_status_pending_dismiss = false;
  s_hold_screen_active = false;
  clearPortalStatusScreen();
  if (wifiLinkUp()) {
    s_pending_radar_redraw = true;
  }
}

void finishHoldOnRelease() {
  if (s_hold_stage == kHoldFactoryReset) {
    s_portal_status_pending_dismiss = false;
    clearPortalStatusScreen();
    Serial.println("BOOT held — factory reset");
    wifiResetCredentialsAndReboot();
    return;
  }

  if (s_hold_stage == kHoldPortal) {
    if (s_portal_disable_on_release) {
      disableLanPortal();
    }
    if (s_portal_status_kind != kPortalStatusNone) {
      s_portal_status_pending_dismiss = true;
      s_hold_screen_active = true;
      redrawPortalStatusScreen();
    } else {
      s_hold_screen_active = false;
      if (wifiLinkUp()) {
        s_pending_radar_redraw = true;
      }
    }
  } else {
    s_hold_screen_active = false;
  }

  s_hold_stage = kHoldNone;
  s_portal_disable_on_release = false;
}

void prepareSta() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(WIFI_PS_NONE);
  WiFi.setAutoReconnect(true);
}

void startStaConnect(const String& ssid, const String& pass) {
  prepareSta();
  if (ssid.length() > 0) {
    WiFi.begin(ssid.c_str(), pass.c_str());
  } else {
    WiFi.begin();
  }
}

bool waitForLinkWithUi(const char* ssid_for_ui, unsigned long attempt_ms) {
  const unsigned long deadline = millis() + attempt_ms;
  while (millis() < deadline) {
    if (wifiLinkUp()) {
      return true;
    }
    bootButtonPoll();
    if (!bootButtonHoldScreenActive()) {
      statusScreenConnectingTick();
    }
    delay(config::kWifiConnectingFrameMs);
  }
  return wifiLinkUp();
}

bool tryConnectWithUi(const String& ssid, const String& pass, bool show_ui) {
  if (wifiLinkUp()) {
    return true;
  }

  const char* ui_ssid = ssid.length() > 0 ? ssid.c_str() : "network";
  if (show_ui) {
    statusScreenConnectingBegin(ui_ssid);
  }

  for (uint8_t attempt = 1; attempt <= config::kWifiConnectAttempts; ++attempt) {
    if (attempt > 1) {
      Serial.printf("WiFi connect retry %u/%u\n", attempt,
                    config::kWifiConnectAttempts);
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      delay(400);
    }

    startStaConnect(ssid, pass);

    if (waitForLinkWithUi(ui_ssid, config::kWifiConnectAttemptMs)) {
      return true;
    }
  }

  return false;
}

bool connectSavedNetwork(bool show_ui) {
  if (!storedWifiCredentials()) {
    return false;
  }

  ensureWifiManager();
  const String ssid = s_wm.getWiFiSSID();
  if (ssid.length() == 0) {
    return false;
  }
  const String pass = s_wm.getWiFiPass();
  return tryConnectWithUi(ssid, pass, show_ui);
}

bool openConfigPortal() {
  stopLanWebPortal();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(50);
  statusScreenPortal();
  s_wm.setConfigPortalBlocking(false);
  applyApPortalMenu();
  s_wm.startConfigPortal(config::kPortalApName);
  while (s_wm.getConfigPortalActive()) {
    bootButtonPoll();
    if (s_wm.process()) {
      return true;
    }
    delay(10);
  }
  return wifiLinkUp();
}

}  // namespace

bool wifiShowsSetupScreenOnBoot() {
  if (s_force_config_portal) {
    return true;
  }
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  return pending;
}

bool wifiBootButtonPressed() {
  return digitalRead(config::kBootPin) == LOW;
}

void bootButtonInit() { initBootButton(); }

bool bootButtonConsumeTap() {
  portENTER_CRITICAL(&s_boot_mux);
  const bool tap = s_boot_tap_pending;
  if (tap) {
    s_boot_tap_pending = false;
  }
  portEXIT_CRITICAL(&s_boot_mux);
  return tap;
}

void bootButtonPoll() {
  pollPortalStatusDismiss();

  if (wifiBootButtonPressed()) {
    if (!s_hold_tracking) {
      s_hold_tracking = true;
      s_hold_down_ms = millis();
      s_hold_stage = kHoldNone;
      s_portal_disable_on_release = false;
    } else {
      const unsigned long held = millis() - s_hold_down_ms;
      if (s_hold_stage < kHoldFactoryReset &&
          held >= config::kBootFactoryResetHoldMs) {
        enterFactoryResetHoldStage();
      } else if (s_hold_stage < kHoldPortal &&
                 held >= config::kBootPortalHoldMs) {
        enterPortalHoldStage();
      }
    }
    return;
  }

  if (!s_hold_tracking) {
    return;
  }

  s_hold_tracking = false;
  finishHoldOnRelease();
}

bool bootButtonHoldScreenActive() {
  return s_hold_screen_active || s_portal_status_pending_dismiss;
}

bool bootButtonConsumeRadarRedraw() {
  if (!s_pending_radar_redraw) {
    return false;
  }
  s_pending_radar_redraw = false;
  return true;
}

bool wifiLanPortalEnabled() { return s_portal_enabled; }

void wifiResetCredentialsAndReboot() {
  resetWifiCredentials();
  statusScreenWifiReset();
  delay(800);
  esp_restart();
}

bool wifiReconnect() {
  initBootButton();
  Serial.println("WiFi reconnecting...");
  return connectSavedNetwork(true);
}

void wifiLoop() {
  ensureWifiManager();
  pollPortalStatusDismiss();
  if (wifiLinkUp()) {
    if (s_portal_enabled) {
      if (!s_wm.getWebPortalActive() && !s_wm.getConfigPortalActive()) {
        startLanWebPortal();
      }
      if (s_wm.getWebPortalActive() || s_wm.getConfigPortalActive()) {
        bootButtonPoll();
        s_wm.process();
      }
    } else if (s_wm.getWebPortalActive()) {
      stopLanWebPortal();
    }
  } else if (s_wm.getWebPortalActive()) {
    stopLanWebPortal();
  }
}

bool wifiSetupConnect() {
  initBootButton();
  ensureWifiManager();

  const bool force_portal = consumeForceConfigPortal();
  WiFi.setAutoReconnect(false);

  if (force_portal) {
    eraseWifiCredentials();
    WiFi.mode(WIFI_OFF);
    delay(100);
  }

  if (force_portal) {
    Serial.println("Opening WiFi setup portal (after reset)");
    if (openConfigPortal() && wifiLinkUp()) {
      WiFi.setAutoReconnect(true);
      Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str());
      return true;
    }
    Serial.println("WiFi connection failed");
    statusScreenConnectFailed();
    return false;
  }

  Serial.println("Connecting to WiFi (portal opens if needed)...");

  if (wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  if (storedWifiCredentials() && connectSavedNetwork(true)) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  if (storedWifiCredentials()) {
    Serial.println("Saved WiFi could not connect — opening setup portal");
  } else {
    Serial.println("No saved WiFi — opening setup portal");
  }

  if (openConfigPortal() && wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  Serial.println("WiFi connection failed");
  statusScreenConnectFailed();
  return false;
}
