#pragma once

class WiFiManager;

namespace services::radar_portal {

void registerRoutes(WiFiManager& wm);

/** Static PROGMEM literal for setCustomMenuHTML() on the setup AP. */
const char* menuLinkHtml();
/** Setup AP menu plus LAN-only reset control. */
const char* menuLanLinkHtml();

}  // namespace services::radar_portal
