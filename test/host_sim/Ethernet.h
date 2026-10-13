#pragma once
#include "Arduino.h"
enum { LinkON = 1, LinkOFF = 0 };
struct NetworkInterface {};
struct EthernetClass { int link = 1; int linkStatus() { return link; } NetworkInterface* getNetwork() { return nullptr; }
  template<class... A> void begin(A...) {} };
extern EthernetClass Ethernet;
struct EthernetClient : Client {};
