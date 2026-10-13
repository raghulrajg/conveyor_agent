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

void ackSwitch() { CHECK(camSwitchRequest); camSwitchRequest = false; camera("ok"); run(20); }

int main() {
  g_pins[A3] = 1; g_pins[A5] = 0; g_pins[BTN_USER] = 1;
  setup();
  camConnected = true;
  run(100);
  CHECK(state == ST_IDLE);

  puts("\n=== 1. product name before the part: no switch on an empty belt, switch at S1, no wait at S2");
  mqttProduct("{\"request_id\": \"t-5#1\", \"subtask_id\": \"t-5\", \"product\": \"ABCD\", \"station\": \"quality_station\"}");
  run(2000);
  CHECK(strcmp(pendingProduct, "ABCD") == 0);
  CHECK(state == ST_IDLE && !camSwitchRequest && switchState == SW_NONE);   // the job may still be cancelled
  sensors(1, 0, 0); run(700); CHECK(state == ST_RUN_TO_CAMERA);
  CHECK(camSwitchRequest && strcmp(camSwitchProduct, "ABCD") == 0);       // S1 seen: switch now
  CHECK(switchState == SW_WAITING);
  camSwitchRequest = false;
  camera("OK");                   // ack, case-insensitive
  run(20); CHECK(switchState == SW_OK); CHECK(state == ST_RUN_TO_CAMERA);
  sensors(0, 1, 0); run(500);     // debounce + settle 400 ms
  CHECK(state == ST_WAIT_RESULT); // program already loaded: straight on
  CHECK(!camSwitchRequest);       // no second switch
  drainMqtt();
  camera("12.5;30.2;45.0;OK;");
  run(20); CHECK(state == ST_RUN_TO_EXIT);
  auto m = drainMqtt();
  CHECK(has(m, MQ_RESULT, "\"result\":\"OK\"") && has(m, MQ_RESULT, "\"product\":\"ABCD\",\"switch_ok\":true"));
  for (auto& e : m) printf("MQTT[%d] %s\n", e.first, e.second.c_str());
  partToExitAndAway();
  CHECK(cycleProduct[0] == 0 && pendingProduct[0] == 0);

  puts("\n=== 2. name arrives while the part travels; ok comes at S2 -> waits only for the ok");
  sensors(1, 0, 0); run(700); CHECK(state == ST_RUN_TO_CAMERA);
  mqttProduct("Widget B-2"); run(20);
  CHECK(camSwitchRequest && strcmp(camSwitchProduct, "Widget B-2") == 0);
  camSwitchRequest = false;
  sensors(0, 1, 0); run(500);
  CHECK(state == ST_SWITCH_PROGRAM);
  camera("12;3;4;OK;");           // data while waiting for the ok: ignored
  run(50); CHECK(state == ST_SWITCH_PROGRAM);
  camera("ok"); run(20); CHECK(state == ST_WAIT_RESULT);
  camera("1;2;3;OK;"); run(20); CHECK(state == ST_RUN_TO_EXIT);
  m = drainMqtt(); CHECK(has(m, MQ_RESULT, "\"product\":\"Widget B-2\",\"switch_ok\":true"));
  partToExitAndAway();

  puts("\n=== 3. no ok within 10 s of sending (before S2) -> warning, part does not wait at S2");
  mqttProduct("P3"); run(20); CHECK(!camSwitchRequest);
  sensors(1, 0, 0); run(700); CHECK(state == ST_RUN_TO_CAMERA); CHECK(camSwitchRequest); camSwitchRequest = false;
  run(10100); CHECK(state == ST_RUN_TO_CAMERA);   // slow belt: still on the way
  CHECK(switchState == SW_NO_ACK); CHECK(warnings & W_SWITCH_NO_ACK);
  m = drainMqtt();
  CHECK(has(m, MQ_WARNING, "\"warning\":\"SWITCH_NO_ACK\"") && has(m, MQ_WARNING, "\"product\":\"P3\""));
  for (auto& e : m) printf("MQTT[%d] %s\n", e.first, e.second.c_str());
  sensors(0, 1, 0); run(500);
  CHECK(state == ST_WAIT_RESULT);
  camera("ok"); run(50); CHECK(state == ST_WAIT_RESULT);   // late ack: not a result
  camera("1;2;3;NG;"); run(20); CHECK(state == ST_RUN_TO_EXIT);
  m = drainMqtt();
  CHECK(has(m, MQ_RESULT, "\"result\":\"NG\"") && has(m, MQ_RESULT, "\"switch_ok\":false"));
  partToExitAndAway();
  CHECK(!(warnings & W_SWITCH_NO_ACK));

  puts("\n=== 4. part at the camera first, name later: switch then, 10 s timeout from sending");
  partToCamera(); CHECK(state == ST_WAIT_PRODUCT);
  run(3000); CHECK(state == ST_WAIT_PRODUCT);
  cmd("PRODUCT P4"); run(50); CHECK(state == ST_SWITCH_PROGRAM); CHECK(camSwitchRequest);
  camSwitchRequest = false;
  run(9800); CHECK(state == ST_SWITCH_PROGRAM);
  run(400); CHECK(state == ST_WAIT_RESULT); CHECK(warnings & W_SWITCH_NO_ACK);
  cmd("SIM 1;2;3;OK;"); run(20); CHECK(state == ST_RUN_TO_EXIT);
  partToExitAndAway(); drainMqtt();

  puts("\n=== 5. next part's name during this part's inspection: switch waits for the next S1");
  mqttProduct("P5a"); run(20);
  sensors(1, 0, 0); run(700); ackSwitch();
  sensors(0, 1, 0); run(500); CHECK(state == ST_WAIT_RESULT);
  mqttProduct("P5b"); run(50);
  CHECK(!camSwitchRequest);       // camera busy with P5a
  CHECK(strcmp(pendingProduct, "P5b") == 0);
  camera("1;2;3;OK;"); run(20); CHECK(state == ST_RUN_TO_EXIT);
  CHECK(!camSwitchRequest);       // P5b's part is not on the belt yet
  m = drainMqtt(); CHECK(has(m, MQ_RESULT, "\"product\":\"P5a\",\"switch_ok\":true"));
  partToExitAndAway();
  CHECK(!camSwitchRequest && strcmp(pendingProduct, "P5b") == 0);   // kept for the next part
  sensors(1, 0, 0); run(700); CHECK(camSwitchRequest && strcmp(camSwitchProduct, "P5b") == 0);
  ackSwitch();
  sensors(0, 1, 0); run(500); CHECK(state == ST_WAIT_RESULT);
  cmd("SIM 1;2;3;OK;"); run(20);
  m = drainMqtt(); CHECK(has(m, MQ_RESULT, "\"product\":\"P5b\",\"switch_ok\":true"));
  partToExitAndAway();

  puts("\n=== 6. no product name -> FAULT NO_PRODUCT; reset with the part at the camera");
  partToCamera();
  run(30100);
  CHECK(state == ST_FAULT && fault == F_NO_PRODUCT);
  m = drainMqtt(); CHECK(has(m, MQ_WARNING, "NO_PRODUCT"));
  cmd("RESET"); run(50);
  mqttProduct("ABCD"); run(500); CHECK(state == ST_SWITCH_PROGRAM);
  cmd("SIM ok"); run(20); CHECK(state == ST_WAIT_RESULT);
  cmd("SIM 1;2;3;OK;"); run(20); CHECK(state == ST_RUN_TO_EXIT);
  partToExitAndAway(); drainMqtt();

  puts("\n=== 7. bad product names are rejected");
  camSwitchRequest = false;
  mqttProduct("{\"product\": \"AB\\\"CD;rm\"}"); run(20); CHECK(pendingProduct[0] == 0);
  mqttProduct("{\"product\": null}"); run(20); CHECK(pendingProduct[0] == 0);
  mqttProduct("   "); run(20); CHECK(pendingProduct[0] == 0);
  CHECK(!camSwitchRequest);
  m = drainMqtt(); CHECK(has(m, MQ_WARNING, "PRODUCT_REJECTED"));

  puts("\n=== 8. name of a cancelled job (no part) goes stale, never switched; STOP keeps the name");
  mqttProduct("OLD1"); run(20);
  run(121000);
  CHECK(pendingProduct[0] == 0);
  m = drainMqtt(); CHECK(has(m, MQ_WARNING, "PRODUCT_STALE"));
  CHECK(!camSwitchRequest && switchState != SW_WAITING);
  partToCamera();
  CHECK(state == ST_WAIT_PRODUCT); CHECK(!camSwitchRequest);
  mqttProduct("NEW1"); run(50); CHECK(state == ST_SWITCH_PROGRAM);
  ackSwitch(); CHECK(state == ST_WAIT_RESULT);
  cmd("STOP"); run(20); CHECK(state == ST_STOPPED);
  cmd("START"); run(1000);
  CHECK(state == ST_WAIT_RESULT); CHECK(strcmp(cycleProduct, "NEW1") == 0); CHECK(!camSwitchRequest);
  cmd("SIM 1;2;3;OK;"); run(20);
  m = drainMqtt(); CHECK(has(m, MQ_RESULT, "\"product\":\"NEW1\",\"switch_ok\":true"));
  partToExitAndAway();

  puts("\n=== 9. camera offline when the name arrives: sent once it connects");
  camConnected = false;
  mqttProduct("P9"); run(20); CHECK(!camSwitchRequest);
  partToCamera(); CHECK(state == ST_SWITCH_PROGRAM);
  camConnected = true; run(20); CHECK(camSwitchRequest && strcmp(camSwitchProduct, "P9") == 0);
  ackSwitch(); CHECK(state == ST_WAIT_RESULT);
  cmd("SIM 1;2;3;OK;"); run(20); partToExitAndAway(); drainMqtt();
  puts("--- offline the whole time: warning after 10 s, cycle continues");
  camConnected = false;
  mqttProduct("P10"); run(20);
  partToCamera(); CHECK(state == ST_SWITCH_PROGRAM);
  run(10100); CHECK(state == ST_WAIT_RESULT);
  m = drainMqtt(); CHECK(has(m, MQ_WARNING, "not sent: camera not connected"));
  cmd("STATUS"); run(10);

  printf("\n%s (%d failed checks)\n", fails ? "FAILED" : "ALL PASSED", fails);
  return fails ? 1 : 0;
}
