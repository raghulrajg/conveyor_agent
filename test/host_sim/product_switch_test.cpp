// PC test of the deploy sketch product-switch sequence. Run: sh test/host_sim/run.sh
#include "Arduino.h"
unsigned long g_millis = 1000;
int g_pins[64];
SerialT Serial;
#include "Ethernet.h"
#include "WiFi.h"
EthernetClass Ethernet;
WiFiClass WiFi;
#include SKETCH
#include <cassert>
#include <iostream>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("\n*** CHECK FAILED line %d: %s\n", __LINE__, #c); fails++; } } while (0)

void run(unsigned long ms) { for (unsigned long t = 0; t < ms; t += 5) { g_millis += 5; loop(); } }
void sensors(int a, int b, int c) { g_pins[A0] = a; g_pins[A1] = b; g_pins[A2] = c; }
std::vector<std::pair<int, std::string>> drainMqtt() {
  std::vector<std::pair<int, std::string>> v;
  while (mqttQCount) { v.push_back({mqttQTopic[mqttQHead], mqttQ[mqttQHead]}); mqttQHead = (mqttQHead + 1) % MQTT_QUEUE_SIZE; mqttQCount--; }
  return v;
}
void camera(const char* text) { netPost(NM_DATA, millis(), "%s", text); }
void mqttProduct(const char* text) { netPost(NM_PRODUCT, millis(), "%s", text); }
void cmd(const char* c) { Serial.in += c; Serial.in += "\n"; }
bool has(const std::vector<std::pair<int, std::string>>& v, int topic, const char* sub) {
  for (auto& e : v) if (e.first == topic && e.second.find(sub) != std::string::npos) return true;
  return false;
}
// one part from S1 to the camera (settled)
void partToCamera() {
  sensors(1, 0, 0); run(700); CHECK(state == ST_RUN_TO_CAMERA);
  sensors(0, 1, 0); run(500);
}
void partToExitAndAway() {
  sensors(0, 0, 1); run(100); CHECK(state == ST_AT_EXIT);
  sensors(0, 0, 0); run(700); CHECK(state == ST_IDLE);
}

int main() {
  g_pins[A3] = 1; g_pins[A5] = 0; g_pins[BTN_USER] = 1;
  setup();
  camConnected = true;
  run(100);
  CHECK(state == ST_IDLE);

  puts("\n=== 1. product over MQTT (scheduler JSON), switch ok, result carries product");
  mqttProduct("{\"request_id\": \"t-5#1\", \"subtask_id\": \"t-5\", \"product\": \"ABCD\", \"station\": \"quality_station\"}");
  run(20);
  CHECK(strcmp(pendingProduct, "ABCD") == 0);
  partToCamera();
  CHECK(state == ST_SWITCH_PROGRAM);
  CHECK(camSwitchRequest && strcmp(camSwitchProduct, "ABCD") == 0);
  camSwitchRequest = false;
  camera("12;3;4;OK;");           // data while switching: ignored
  run(50); CHECK(state == ST_SWITCH_PROGRAM);
  camera("OK");                   // ack, case-insensitive
  run(20); CHECK(state == ST_WAIT_RESULT); CHECK(switchOk);
  drainMqtt();
  camera("12.5;30.2;45.0;OK;");
  run(20); CHECK(state == ST_RUN_TO_EXIT);
  auto m = drainMqtt();
  CHECK(has(m, MQ_RESULT, "\"result\":\"OK\"") && has(m, MQ_RESULT, "\"product\":\"ABCD\",\"switch_ok\":true"));
  for (auto& e : m) printf("MQTT[%d] %s\n", e.first, e.second.c_str());
  partToExitAndAway();
  CHECK(cycleProduct[0] == 0 && pendingProduct[0] == 0);

  puts("\n=== 2. product needed again next cycle; no ok -> warning, cycle continues");
  partToCamera();
  CHECK(state == ST_WAIT_PRODUCT);
  run(3000); CHECK(state == ST_WAIT_PRODUCT);
  cmd("PRODUCT Widget B-2");
  run(50); CHECK(state == ST_SWITCH_PROGRAM); CHECK(strcmp(cycleProduct, "Widget B-2") == 0);
  run(9000); CHECK(state == ST_SWITCH_PROGRAM);
  run(1100); CHECK(state == ST_WAIT_RESULT); CHECK(switchWarn); CHECK(warnings & W_SWITCH_NO_ACK);
  m = drainMqtt();
  CHECK(has(m, MQ_WARNING, "\"warning\":\"SWITCH_NO_ACK\"") && has(m, MQ_WARNING, "\"product\":\"Widget B-2\""));
  for (auto& e : m) printf("MQTT[%d] %s\n", e.first, e.second.c_str());
  camera("ok");                   // late ack: not a result
  run(50); CHECK(state == ST_WAIT_RESULT);
  camera("1;2;3;NG;");
  run(20); CHECK(state == ST_RUN_TO_EXIT);
  m = drainMqtt();
  CHECK(has(m, MQ_RESULT, "\"result\":\"NG\"") && has(m, MQ_RESULT, "\"switch_ok\":false"));
  for (auto& e : m) printf("MQTT[%d] %s\n", e.first, e.second.c_str());
  partToExitAndAway();
  CHECK(!switchWarn);

  puts("\n=== 3. no product name -> FAULT NO_PRODUCT");
  partToCamera();
  run(30100);
  CHECK(state == ST_FAULT && fault == F_NO_PRODUCT);
  m = drainMqtt(); CHECK(has(m, MQ_WARNING, "NO_PRODUCT"));
  puts("--- reset with the part still at the camera, then product arrives");
  cmd("RESET"); run(50); CHECK(state == ST_WAIT_PRODUCT || state == ST_SETTLE);
  mqttProduct("ABCD"); run(500); CHECK(state == ST_SWITCH_PROGRAM);
  cmd("SIM ok"); run(20); CHECK(state == ST_WAIT_RESULT);
  cmd("SIM 1;2;3;OK;"); run(20); CHECK(state == ST_RUN_TO_EXIT);
  partToExitAndAway(); drainMqtt();

  puts("\n=== 4. bad product names are rejected, retained/garbage no product");
  mqttProduct("{\"product\": \"AB\\\"CD;rm\"}"); run(20); CHECK(pendingProduct[0] == 0);
  mqttProduct("{\"product\": null}"); run(20); CHECK(pendingProduct[0] == 0);
  mqttProduct("   "); run(20); CHECK(pendingProduct[0] == 0);
  m = drainMqtt(); CHECK(has(m, MQ_WARNING, "PRODUCT_REJECTED"));
  for (auto& e : m) printf("MQTT[%d] %s\n", e.first, e.second.c_str());

  puts("\n=== 5. stale product is not used");
  mqttProduct("OLD1"); run(20);
  run(121000);
  partToCamera();
  CHECK(state == ST_WAIT_PRODUCT); CHECK(pendingProduct[0] == 0);
  m = drainMqtt(); CHECK(has(m, MQ_WARNING, "PRODUCT_STALE"));
  puts("--- STOP during the switch, restart keeps the product");
  mqttProduct("NEW1"); run(50); CHECK(state == ST_SWITCH_PROGRAM);
  cmd("STOP"); run(20); CHECK(state == ST_STOPPED);
  cmd("START"); run(1000); CHECK(state == ST_SWITCH_PROGRAM); CHECK(strcmp(cycleProduct, "NEW1") == 0);
  cmd("SIM ok"); run(20); cmd("SIM 1;2;3;OK;"); run(20); partToExitAndAway();

  puts("\n=== 6. camera offline at switch time: sent once it connects");
  mqttProduct("P9"); run(20);
  camConnected = false;
  partToCamera(); CHECK(state == ST_SWITCH_PROGRAM); CHECK(!switchSent);
  camConnected = true; run(20); CHECK(switchSent && camSwitchRequest);
  cmd("STATUS"); run(10);

  printf("\n%s (%d failed checks)\n", fails ? "FAILED" : "ALL PASSED", fails);
  return fails ? 1 : 0;
}
