#pragma once
#include "Arduino.h"
struct MqttClient {
  MqttClient(Client&) {}
  void setId(const char*) {} void setUsernamePassword(const char*, const char*) {}
  void setKeepAliveInterval(unsigned long) {} void setConnectionTimeout(unsigned long) {}
  void onMessage(void (*)(int)) {}
  int connected() { return 1; } int connect(IPAddress, uint16_t) { return 1; } int connectError() { return 0; }
  void stop() {} int subscribe(const char*, int) { return 1; }
  int beginMessage(const char*, size_t, bool, int) { return 1; } size_t print(const char* s) { return strlen(s); }
  int endMessage() { return 1; } void poll() {}
  bool messageRetain() { return false; } String messageTopic() { return String(""); }
  int available() { return 0; } int read() { return -1; }
};
