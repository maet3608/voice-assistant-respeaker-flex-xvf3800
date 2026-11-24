// The LED, the AIC3104 codec and WiFi.

#pragma once

#include <Arduino.h>

void setLed(bool on);

void setCodecVolumeDb(float db);

void setupCodec();
bool connectWiFi();
