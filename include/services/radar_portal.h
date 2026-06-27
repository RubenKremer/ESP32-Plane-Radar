#pragma once

class WiFiManager;

namespace services::radar_portal {

void registerRoutes(WiFiManager& wm);

/** Static PROGMEM literal for setCustomMenuHTML(). */
const char* menuLinkHtml();

}  // namespace services::radar_portal
