/**
 * Plane Radar — WiFi setup, then radar UI on the round GC9A01 display.
 */

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "hardware/display.h"
#include "services/adsb_client.h"
#include "services/radar_location.h"
#include "services/wifi_setup.h"
#include "services/wifi_radio.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

namespace {

bool g_radar_visible = false;
unsigned long g_wifi_down_since = 0;
unsigned long g_last_reconnect_ms = 0;
unsigned long g_last_adsb_fetch_ms = 0;

void applyPendingRangeRedraw() {
  if (!ui::radar::rangeDirty()) {
    return;
  }
  if (!g_radar_visible || WiFi.status() != WL_CONNECTED ||
      bootButtonHoldScreenActive()) {
    return;
  }
  ui::radar::clearRangeDirty();
  ui::radarDisplayDraw();
}

void showRadarIfConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    g_radar_visible = false;
    return;
  }
  if (bootButtonHoldScreenActive()) {
    return;
  }
  ui::radarDisplayDraw();
  ui::radar::clearRangeDirty();
  g_radar_visible = true;
}

void onRangeTap() { ui::radar::rangeNext(); }

void handleBootButton() {
  bootButtonPoll();
  if (bootButtonConsumeRadarRedraw() && g_radar_visible &&
      !bootButtonHoldScreenActive()) {
    ui::radarDisplayDraw();
  }
  if (bootButtonConsumeTap() && !bootButtonHoldScreenActive()) {
    onRangeTap();
  }
}

void fetchAndDrawAircraft() {
  const float fetch_km = ui::radar::fetchRadiusKm();
  if (!services::adsb::fetchUpdate(services::location::lat(),
                                   services::location::lon(), fetch_km)) {
    handleBootButton();
    return;
  }
  if (!bootButtonHoldScreenActive()) {
    ui::radarDisplayRefreshAircraft();
  }
  handleBootButton();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("Plane Radar");

  bootButtonInit();
  displayInit();
  if (wifiShowsSetupScreenOnBoot()) {
    statusScreenPortal();
  }
  services::location::init();
  services::wifi_radio::wifiRadioInit();
  ui::radar::rangeInit();
  services::adsb::setPollFn([]() {
    wifiLoop();
    bootButtonPoll();
  });
  services::adsb::setAbortFn(wifiBootButtonPressed);

  if (wifiSetupConnect()) {
    showRadarIfConnected();
  }
}

void loop() {
  handleBootButton();
  wifiLoop();
  applyPendingRangeRedraw();

  if (WiFi.status() != WL_CONNECTED) {
    if (g_radar_visible) {
      Serial.println("WiFi lost — will reconnect");
      g_radar_visible = false;
    }

    if (g_wifi_down_since == 0) {
      g_wifi_down_since = millis();
    }

    const unsigned long down_ms = millis() - g_wifi_down_since;
    if (down_ms >= config::kWifiDownGraceMs &&
        millis() - g_last_reconnect_ms >= config::kWifiReconnectIntervalMs) {
      g_last_reconnect_ms = millis();
      if (wifiReconnect()) {
        g_wifi_down_since = 0;
        showRadarIfConnected();
      }
    }
  } else {
    g_wifi_down_since = 0;
    if (!g_radar_visible) {
      showRadarIfConnected();
    } else if (g_radar_visible && !bootButtonHoldScreenActive()) {
      if (ui::radar::pollTimerReset()) {
        ui::radar::clearPollTimerReset();
        g_last_adsb_fetch_ms = millis();
      }
      if (millis() - g_last_adsb_fetch_ms >= ui::radar::pollIntervalMs() &&
          !wifiBootButtonPressed()) {
        g_last_adsb_fetch_ms = millis();
        fetchAndDrawAircraft();
      }
    }
  }

  delay(10);
}
