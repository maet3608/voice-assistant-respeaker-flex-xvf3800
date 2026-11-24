#include "hardware.h"

#include "WiFi.h"
#include "Wire.h"
#include "config.h"
#include "credentials.h"

namespace {

uint8_t aic3104Write(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(aic3104Address);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission();
}

}

void setLed(bool on) { digitalWrite(pinLed, on ? LOW : HIGH); }

void setCodecVolumeDb(float db) {
  int steps = constrain((int)lroundf(-db * 2.0f), 0, 127);
  aic3104Write(aic3104LeftDac, (uint8_t)steps);
  aic3104Write(aic3104RightDac, (uint8_t)steps);
}

void setupCodec() {
  Wire.begin(i2cSda, i2cScl);

  uint8_t failures = 0;
  failures += aic3104Write(aic3104PageCtrl, 0x00) != 0;
  failures += aic3104Write(aic3104HpLeft, 0x0D) != 0;
  failures += aic3104Write(aic3104HpRight, 0x0D) != 0;
  failures += aic3104Write(aic3104LineLeft, 0x0B) != 0;
  failures += aic3104Write(aic3104LineRight, 0x0B) != 0;
  setCodecVolumeDb(codecVolumeDb);

  if (failures > 0) {
    Serial.printf("AIC3104: %u I2C writes failed, check SDA/SCL\n", failures);
  } else {
    Serial.printf("AIC3104: volume %.1f dB\n", codecVolumeDb);
  }
}

bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(wifiSsid, wifiPassword);

  Serial.printf("Connecting to %s\n", wifiSsid);
  if (WiFi.waitForConnectResult(15000) != WL_CONNECTED) {
    Serial.println("WiFi failed, check ssid and password");
    return false;
  }
  Serial.printf("IP %s\n", WiFi.localIP().toString().c_str());
  return true;
}
