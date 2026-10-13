#pragma once
#include "Arduino.h"
#define WL_CONNECTED 3
struct WiFiClass { int status() { return WL_CONNECTED; } int begin(const char*, const char*) { return WL_CONNECTED; } IPAddress localIP() { return IPAddress(); } };
extern WiFiClass WiFi;
struct WiFiClient : Client {};
