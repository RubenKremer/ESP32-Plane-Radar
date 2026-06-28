#pragma once

#include <cstddef>
#include <cstdint>

namespace services::wifi_radio {

constexpr size_t kTxPowerPresetCount = 4;
constexpr uint8_t kDefaultTxPowerIndex = 0;

void wifiRadioInit();
uint8_t txPowerIndex();
bool setTxPowerIndex(uint8_t idx);
void applyTxPower();
void formatTxPowerOption(char* buf, size_t len, uint8_t idx);
/** HTML <select> for the Configure WiFi page (name=\"tx_pwr_idx\"). */
void buildTxPowerSelectHtml(char* buf, size_t len);
bool parseTxPowerIndex(const char* arg, uint8_t* out);
bool saveTxPowerFromPortalForm(const char* arg);

}  // namespace services::wifi_radio
