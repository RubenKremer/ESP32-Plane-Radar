#pragma once

#include <WiFiManager.h>

/** WiFiManager with access to protected options not exposed upstream. */
class WiFiManagerEx : public WiFiManager {
 public:
  void setShowBack(bool show) { _showBack = show; }
};
