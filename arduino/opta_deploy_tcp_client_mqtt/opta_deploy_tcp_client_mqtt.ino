/*
  ============================================================================
   CONVEYOR INSPECTION CONTROLLER + MQTT  -  Arduino Opta WiFi (AFX00002, M7)
   DEPLOY: Opta = TCP CLIENT (camera = TCP server)  +  MQTT client
  ============================================================================
   Hardware
     - Arduino Opta WiFi
     - Hikrobot SC3000 vision sensor on Ethernet (TCP)
     - 3 x industrial proximity sensors, 24 V PNP, normally-open
     - Schneider ATV12H037M2 drive, 2-wire control, speed from AI1

   Camera data (one message per inspection)
       x;y;theta;result;        e.g.  12.5;30.2;45.0;OK;
     - ',' is accepted instead of ';', the trailing separator and CR/LF are
       optional. result = OK / NG (also PASS / FAIL / 1 / 0, see OK_WORDS /
       NG_WORDS). Anything that is not x;y;theta;result counts as NG.

   Sequence (one product at a time)
     IDLE            S1 detects object AND S3 is clear .......... motor ON
     RUN_TO_CAMERA   S2 detects object .......................... motor OFF
     SETTLE          wait for the belt to stop, optionally trigger camera
     WAIT_RESULT     wait for the camera result (x;y;theta;result)
     -> result       published to MQTT; S3 clear ................ motor ON
                     S3 occupied -> WAIT_EXIT_CLEAR (waits, warning shown)
     RUN_TO_EXIT     S3 detects object .......................... motor OFF
     AT_EXIT         object removed from S3 ..................... back to IDLE
     Any timeout / sequence error / STOP -> motor OFF + interrupt indication

   Interrupt indication
     FAULT (latched, needs RESET) : OUTPUT 2 steady ON,  red RESET LED steady
     STOPPED / warning            : OUTPUT 2 blinking,   red RESET LED blinking
       warnings: camera not connected, exit blocked, Ethernet link down
     Blue USER LED = TCP connection to the camera is up.
     Every event is printed on the USB Serial Monitor (115200 baud).
     RESET: Opta USER button, "RESET" typed in the Serial Monitor, or the
            optional external push button on I6.

   Camera link  (OPTA_IS_TCP_SERVER selects the mode)
     false = Opta is TCP CLIENT: connects to the camera's TCP server at
             CAMERA_IP:CAMERA_PORT, reconnects every RECONNECT_INTERVAL_MS.
     true  = Opta is TCP SERVER: listens on CAMERA_PORT; the camera (SCMVS
             "TCP Client") connects to OPTA_IP:CAMERA_PORT. A new connection
             replaces the old one.
     - Runs in its own thread, so network delays never delay the motor stop.
     - TCP keep-alive detects a camera that lost power (~12 s).
     - Message end: CR, LF, the 4th ';' or ',' or RX_FRAME_GAP_MS of silence.
     - Only data that starts arriving after the part has stopped and settled
       is used as the result; earlier data is ignored.

  ----------------------------------------------------------------------------
   WIRING (defaults - change pin constants below if needed)
  ----------------------------------------------------------------------------
   Opta supply 24 VDC. Sensor 0 V must be common with Opta 0 V.
     I1 (A0) <- S1 entry sensor   (PNP: brown +24 V, blue 0 V, black -> I1)
     I2 (A1) <- S2 camera-position sensor
     I3 (A2) <- S3 exit sensor
     I4 (A3) <- [optional] E-stop monitor contact (24 V = healthy)
     I5 (A4) <- [optional] ATV12 fault relay: +24 V -> R1C, R1A -> I5
                (R1 energised = drive healthy, so 24 V = OK)
     I6 (A5) <- [optional] external RESET push button (NO, 24 V)

     OUTPUT 1 (D0)  ATV12 "+24V" terminal -> relay -> "LI1"  (RUN forward)
     OUTPUT 2 (D1)  alarm beacon / buzzer   (interrupt indication)
     OUTPUT 3 (D2)  NG lamp (on while a rejected product is at the exit)
     OUTPUT 4 (D3)  green lamp SYSTEM OK (on = running normally: no fault,
                    no stop, no warning; off = something needs attention)
     RJ45           Hikrobot SC3000 (direct cable, or a switch if a PC must
                    also be connected)

     ATV12 AI1: potentiometer 2.2..10 k between "+10V" and "COM", wiper -> "AI1"
     The E-stop MUST be hard-wired (break LI1 and/or the drive supply); the
     I4 input only reports it to the program.

  ----------------------------------------------------------------------------
   ATV12H037M2 parameters (as configured)
  ----------------------------------------------------------------------------
     I_O-  tCC = 2C    2-wire control (LI1 = run forward)
           tCt = trn   start needs a 0 -> 1 transition on LI1. The program
                       always opens the relay for >= MIN_MOTOR_OFF_MS before
                       closing it again, so every start is a clean edge.
     CtL-  Fr1 = AI1   speed reference from AI1 (potentiometer)
     FUn-  rrS = nO    reverse disabled
     drC-  bFr = 50, UnS = 230, nCr = 1.9
   RECOMMENDED: SEt- dEC = 0.2 .. 0.5 s (factory 3.0 s lets the product coast
   past the camera) and ACC = 0.5 .. 1.0 s. Keep SETTLE_MS > dEC.

  ----------------------------------------------------------------------------
   Network
  ----------------------------------------------------------------------------
     Opta    169.254.163.10 / 255.255.0.0 (static)
     Camera  169.254.163.5  / 255.255.0.0
       Opta = client : camera SCMVS "TCP Server", port 8000
       Opta = server : camera SCMVS "TCP Client" -> 169.254.163.10 port 8000

   USB Serial commands (Arduino IDE Serial Monitor, 115200, any line ending)
     HELP, PING, STATUS, RESET, START, STOP, SIM <text>, TRIG, RECONNECT,
     CLEAR, VERSION     e.g.  SIM 12.5;30.2;45.0;OK;   (test without camera)

   MQTT (result publishing)
     Every camera result is published as JSON to MQTT_TOPIC, e.g.
       {"x":12.500,"y":30.200,"theta":45.000,"result":"OK","valid":true,"cycle":5,"raw":"12.5;30.2;45.0;OK;"}
       {"x":10.000,"y":20.000,"theta":5.000,"result":"NG","valid":true,"cycle":6,"raw":"10;20;5;NG;"}
       {"x":null,"y":null,"theta":null,"result":"NG","valid":false,"cycle":7,"raw":"garbage"}
     Default network for MQTT = Opta WiFi (Ethernet is on the camera's
     169.254.x.x link and cannot reach the broker). Fill in WIFI_SSID and
     WIFI_PASSWORD. If the Opta, camera and broker share one wired network,
     set MQTT_USE_ETHERNET = true and give the Opta an IP in that network.
     MQTT runs in its own thread with a send queue: a broker or WiFi outage
     never stops the conveyor, and up to MQTT_QUEUE_SIZE results are kept
     and sent when the connection is back.
     Library: install "ArduinoMqttClient" (Arduino) from the Library Manager.

   Board package: "Arduino Mbed OS Opta Boards", board "Arduino Opta",
   target core "Main Core" (M7).
  ============================================================================
*/

#include <Ethernet.h>
#include <WiFi.h>
#include <ArduinoMqttClient.h>     // Library Manager: "ArduinoMqttClient"
#include "mbed.h"
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

#define FW_VERSION "OPTA-CONVEYOR 3.0"

// ============================ CONFIGURATION =================================
// ---- Network ----
// false = Opta is TCP CLIENT (camera = TCP server)
// true  = Opta is TCP SERVER (camera = TCP client)
const bool OPTA_IS_TCP_SERVER = false;
IPAddress CAMERA_IP(169, 254, 163, 5);       // camera address (used in client mode)
const uint16_t CAMERA_PORT = 8000;           // client mode: camera port, server mode: listen port
IPAddress OPTA_IP(169, 254, 163, 10);
IPAddress OPTA_SUBNET(255, 255, 0, 0);       // 169.254.x.x (link-local) uses /16
IPAddress OPTA_GATEWAY(0, 0, 0, 0);    // not used (camera is on the same subnet)
IPAddress OPTA_DNS(0, 0, 0, 0);        // not used

const unsigned long RECONNECT_INTERVAL_MS = 2000;  // retry period while the camera is unreachable
const int KEEPALIVE_IDLE_MS     = 3000;      // TCP keep-alive: dead camera detected after
const int KEEPALIVE_INTERVAL_MS = 1000;      //   idle + 9 x interval (~12 s)
const int SOCKET_SEND_TIMEOUT_MS = 500;
const int SOCKET_POLL_MS         = 20;       // camera thread receive poll period

// ---- MQTT ----
const bool USE_MQTT          = true;
const bool MQTT_USE_ETHERNET = false;        // false = WiFi (default), true = Ethernet
const char* const WIFI_SSID     = "DESKTOP-HKOM52L 2139"; 
const char* const WIFI_PASSWORD = "123456789";
IPAddress MQTT_BROKER_IP(172, 16, 13, 62);   // MQTT_BROKER_HOST
const uint16_t MQTT_BROKER_PORT = 1883;
const char* const MQTT_USERNAME  = "raghulrajg";
const char* const MQTT_PASSWORD  = "Gr2_nemam";
const char* const MQTT_CLIENT_ID = "opta-conveyor";
const char* const MQTT_TOPIC     = "autonomous/product/Transformation";
const bool MQTT_RETAIN = false;
const uint8_t MQTT_QOS = 0;
const unsigned long MQTT_RETRY_MS = 5000;     // WiFi / broker reconnect period
const unsigned long MQTT_KEEPALIVE_MS = 15000;
const uint8_t MQTT_QUEUE_SIZE = 16;
const size_t MQTT_PAYLOAD_MAX = 256;

// ---- Inputs ----
const uint8_t PIN_S1_ENTRY  = A0;     // I1
const uint8_t PIN_S2_CAMERA = A1;     // I2
const uint8_t PIN_S3_EXIT   = A2;     // I3
const uint8_t PIN_ESTOP     = A3;     // I4 (optional)
const uint8_t PIN_DRIVE_OK  = A4;     // I5 (optional)
const uint8_t PIN_EXT_RESET = A5;     // I6 (optional)

const bool SENSOR_ACTIVE_HIGH = true;  // PNP-NO: 24 V = object present. false for NC sensors
const bool USE_ESTOP_INPUT    = true; // true only if I4 is wired
const bool USE_DRIVE_OK_INPUT = false; // true only if ATV12 R1 is wired to I5
const bool USE_EXT_RESET      = true; // true only if a reset button is wired to I6

// ---- Outputs ----
const uint8_t RELAY_MOTOR   = D0;     // OUTPUT 1 -> ATV12 LI1
const uint8_t RELAY_ALARM   = D1;     // OUTPUT 2 -> beacon / buzzer
const uint8_t RELAY_NG      = D2;     // OUTPUT 3 -> NG lamp
const uint8_t RELAY_SYSTEM_OK = D3;   // OUTPUT 4 -> green lamp: system OK

// ---- Camera trigger ----
//   TRIGGER_NONE  : camera triggers itself (continuous / own sensor). Default.
//   TRIGGER_TCP   : Opta sends CAMERA_TRIGGER_STRING to the camera server when
//                   the part has stopped (SCMVS Trigger Source = Communication)
enum TriggerMode : uint8_t { TRIGGER_NONE, TRIGGER_TCP };
const TriggerMode CAMERA_TRIGGER_MODE = TRIGGER_NONE;
const char* const CAMERA_TRIGGER_STRING = "start"; // = SCMVS "Communication String"
const bool TRIGGER_APPEND_CRLF = false;            // true if the camera expects CR/LF

// ---- Camera result ----
// Message format: x;y;theta;result;   (',' also accepted)
// A camera message is complete after this many ';' or ',' (x;y;theta;result; = 4). 0 = off.
const uint8_t FRAME_END_SEPARATORS = 4;
// result field values (case-insensitive)
const char* const OK_WORDS[] = { "OK", "PASS", "PASSED", "GOOD", "TRUE", "1" };
const char* const NG_WORDS[] = { "NG", "NOK", "FAIL", "FAILED", "BAD", "FALSE", "0", "NOREAD", "ERROR" };
// result field not in either list, or message not in x;y;theta;result format:
// true = NG (NG lamp on, published as NG), false = OK
const bool UNKNOWN_RESULT_IS_NG = true;

// ---- Timing (ms) ----
const unsigned long SENSOR_DEBOUNCE_MS          = 20;
const unsigned long START_DELAY_MS              = 500;   // S1 stable before motor starts
const unsigned long SETTLE_MS                   = 400;   // after stop at S2, before trigger
const unsigned long TRAVEL_TO_CAMERA_TIMEOUT_MS = 15000; // S1 -> S2 max travel time
const unsigned long RESULT_TIMEOUT_MS           = 10000; // max wait for camera result
const unsigned long TRAVEL_TO_EXIT_TIMEOUT_MS   = 15000; // S2 -> S3 max travel time
const unsigned long EXIT_BLOCKED_WARN_MS        = 2000;  // warn if exit stays occupied
const unsigned long EXIT_CLEAR_DELAY_MS         = 500;   // S3 must be clear this long before restart
const unsigned long MIN_MOTOR_OFF_MS            = 300;   // clean 0->1 edge for tCt = trn
const unsigned long DRIVE_FAULT_FILTER_MS       = 500;
const unsigned long DRIVE_BOOT_GRACE_MS         = 3000;  // ATV12 needs time to become ready
const unsigned long CAMERA_BOOT_GRACE_MS        = 45000; // no "camera offline" warning right after power-up
const unsigned long IGNORED_LOG_INTERVAL_MS     = 2000;  // rate-limit "ignored camera data" events
const unsigned long BUTTON_DEBOUNCE_MS          = 50;
const unsigned long RX_FRAME_GAP_MS             = 50;    // un-terminated message ends after this silence
const unsigned long BLINK_MS                    = 500;
const unsigned long LINK_CHECK_MS               = 500;
const unsigned long ETH_BOOT_WAIT_MS            = 5000;  // max wait for cable at power-up
// Hardware watchdog: resets the Opta (all relays drop) if the program hangs.
// Enable after commissioning. Once started it cannot be stopped until reset;
// if a later upload fails, double-press the Opta RESET button and upload again.
const bool          USE_WATCHDOG                = false;
const uint32_t      WATCHDOG_MS                 = 8000;
// ============================================================================

// ------------------------------ Types ---------------------------------------
enum State : uint8_t {
  ST_IDLE, ST_RUN_TO_CAMERA, ST_SETTLE, ST_WAIT_RESULT, ST_WAIT_EXIT_CLEAR,
  ST_RUN_TO_EXIT, ST_AT_EXIT, ST_STOPPED, ST_FAULT
};
const char* const STATE_NAMES[] = {
  "IDLE", "RUN_TO_CAMERA", "SETTLE", "WAIT_RESULT", "WAIT_EXIT_CLEAR",
  "RUN_TO_EXIT", "AT_EXIT", "STOPPED", "FAULT"
};

enum Fault : uint8_t {
  F_NONE, F_S2_TIMEOUT, F_RESULT_TIMEOUT, F_S3_TIMEOUT, F_S3_UNEXPECTED,
  F_ESTOP, F_DRIVE
};
const char* const FAULT_NAMES[] = {
  "NONE", "S2_TIMEOUT", "RESULT_TIMEOUT", "S3_TIMEOUT", "S3_UNEXPECTED",
  "ESTOP", "DRIVE_FAULT"
};

enum ResultClass : uint8_t { RES_NONE, RES_OK, RES_NG, RES_OTHER };
const char* const RESULT_NAMES[] = { "NONE", "OK", "NG", "OTHER" };

// Warning bits (non-latching interrupt indication)
const uint8_t W_EXIT_BLOCKED   = 0x01;
const uint8_t W_CAMERA_OFFLINE = 0x02;
const uint8_t W_ETH_LINK_DOWN  = 0x04;

const size_t LINE_MAX = 128;

struct DebouncedInput {
  uint8_t pin;
  bool activeHigh;
  unsigned long debounceMs;
  bool state;          // debounced, true = active
  bool lastRaw;
  unsigned long rawChangedAt;
  unsigned long stateSince;
  bool rose;           // became active this scan
  bool fell;           // became inactive this scan
};

struct LineRx {
  char buf[LINE_MAX];
  size_t len;
  unsigned long lastByteAt;
  unsigned long startedAt;      // time of the first byte of the message being built
  unsigned long doneStartedAt;  // first-byte time of the message just completed
  uint8_t separators;           // ';' or ',' seen in the current message
  uint8_t endAfterSeparators;   // complete the message after this many separators (0 = off)
};

// Messages from the camera thread to loop()
enum NetMsgType : uint8_t { NM_DATA, NM_INFO, NM_MQTT };
const size_t NETMSG_MAX = 340;
struct NetMsg {
  NetMsgType type;
  unsigned long t;              // millis() when the first byte arrived
  char text[NETMSG_MAX];
};
const uint8_t NETQ_SIZE = 8;

// ----------------------------- Prototypes -----------------------------------
void initInput(DebouncedInput& in, uint8_t pin, bool activeHigh, unsigned long debounceMs, unsigned long now);
void updateInput(DebouncedInput& in, unsigned long now);
bool sensorStableFor(const DebouncedInput& in, unsigned long ms, unsigned long now);
bool sensorClearFor(const DebouncedInput& in, unsigned long ms, unsigned long now);
void enterState(State s, const char* reason);
void raiseFault(Fault f, const char* detail);
void requestReset(const char* source, Print& out);
void requestStop(const char* source, Print& out);
void runStateMachine(unsigned long now);
void checkSafetyInputs(unsigned long now);
void updateMotor(unsigned long now);
void updateWarnings(unsigned long now);
void updateIndication(unsigned long now);
void fireCameraTrigger(const char* source);
void acceptResult(const char* msg, const char* source, unsigned long receivedAt);
struct CamResult {
  bool valid;          // x, y, theta are numbers and a result field is present
  float x, y, theta;
  char result[16];     // result field as received (upper case)
};
bool parseCameraMessage(const char* msg, CamResult& r);
ResultClass classifyResult(const char* msg, const CamResult& r);
bool wordInList(const char* w, const char* const* list, size_t n);
bool containsWord(const char* msg, const char* word);
void fmtFixed(char* out, size_t size, float v);
void resetRx(LineRx& rx);
bool feedByte(LineRx& rx, int ch, char* out, size_t outSize, unsigned long now);
bool frameTimeout(LineRx& rx, char* out, size_t outSize, unsigned long now);
void serviceSerial(unsigned long now);
void serviceCamera(unsigned long now);
void checkEthernetLink(unsigned long now);
void logEvent(const char* kind, const char* fmt, ...);
void buildStatus(char* out, size_t size);
void processCommand(char* line, Print& out, const char* source);
bool cameraConnected();
void warningText(uint8_t w, char* out, size_t size);
void printHelp(Print& out);
// camera thread
void cameraTask();
void netPost(NetMsgType type, unsigned long t, const char* fmt, ...);
// MQTT thread
void mqttTask();
void mqttEnqueue(const char* payload);
void publishResult(const CamResult& r, bool isNg, const char* raw);
bool netPop(NetMsg& out);

// ------------------------------ Globals -------------------------------------
LineRx serialRx;
DebouncedInput s1, s2, s3, estopIn, driveOkIn, extResetIn, userBtn;

State state = ST_IDLE;
unsigned long stateSince = 0;
Fault fault = F_NONE;
uint8_t warnings = 0;
bool motorOn = false;
unsigned long motorOffSince = 0;
bool ngLamp = false;
bool alarmOut = false;
bool linkUp = false;
unsigned long lastLinkCheck = 0;

ResultClass lastResultClass = RES_NONE;
float lastX = 0, lastY = 0, lastTheta = 0;
char lastCamResult[16] = "-";
char lastResultText[48] = "-";
uint32_t cntCycles = 0, cntOk = 0, cntNg = 0, cntFaults = 0;

// ---- shared between loop() and the camera thread ----
rtos::Thread cameraThread(osPriorityNormal, 8192, nullptr, "camera");
rtos::Mutex netMutex;                       // protects netQ
NetMsg netQ[NETQ_SIZE];
uint8_t netQHead = 0, netQCount = 0;
volatile bool camConnected = false;
volatile bool camTriggerRequest = false;
volatile bool camReconnectRequest = false;

// ---- shared between loop() and the MQTT thread ----
rtos::Thread mqttThread(osPriorityNormal, 8192, nullptr, "mqtt");
rtos::Mutex mqttMutex;                      // protects mqttQ
char mqttQ[MQTT_QUEUE_SIZE][MQTT_PAYLOAD_MAX];
uint8_t mqttQHead = 0, mqttQCount = 0;
uint32_t mqttPublished = 0, mqttDropped = 0;
volatile bool mqttConnected = false;
WiFiClient mqttWifiClient;
EthernetClient mqttEthClient;
MqttClient mqttClient(MQTT_USE_ETHERNET ? (Client&)mqttEthClient : (Client&)mqttWifiClient);

// ============================== Inputs ======================================
void initInput(DebouncedInput& in, uint8_t pin, bool activeHigh, unsigned long debounceMs, unsigned long now) {
  in.pin = pin;
  in.activeHigh = activeHigh;
  in.debounceMs = debounceMs;
  pinMode(pin, INPUT);
  bool raw = ((digitalRead(pin) == HIGH) == activeHigh);
  in.state = raw;
  in.lastRaw = raw;
  in.rawChangedAt = now;
  in.stateSince = now;
  in.rose = in.fell = false;
}

void updateInput(DebouncedInput& in, unsigned long now) {
  bool raw = ((digitalRead(in.pin) == HIGH) == in.activeHigh);
  in.rose = in.fell = false;
  if (raw != in.lastRaw) {
    in.lastRaw = raw;
    in.rawChangedAt = now;
  }
  if (raw != in.state && (now - in.rawChangedAt) >= in.debounceMs) {
    in.state = raw;
    in.stateSince = now;
    if (raw) in.rose = true; else in.fell = true;
  }
}

bool sensorStableFor(const DebouncedInput& in, unsigned long ms, unsigned long now) {
  return in.state && (now - in.stateSince) >= ms;
}

bool sensorClearFor(const DebouncedInput& in, unsigned long ms, unsigned long now) {
  return !in.state && (now - in.stateSince) >= ms;
}

// =========================== State machine ==================================
void enterState(State s, const char* reason) {
  State old = state;
  state = s;
  stateSince = millis();
  logEvent("STATE", "%s->%s %s", STATE_NAMES[old], STATE_NAMES[s], reason);

  switch (s) {
    case ST_RUN_TO_CAMERA:
      ngLamp = false;                       // new product, clear old NG lamp
      break;
    case ST_WAIT_RESULT: {
      // Only camera data that starts arriving from now on is accepted (see acceptResult).
      if (CAMERA_TRIGGER_MODE != TRIGGER_NONE) fireCameraTrigger("auto");
      if (!cameraConnected()) logEvent("WARN", "camera not connected - waiting for result anyway");
      break;
    }
    case ST_AT_EXIT:
      cntCycles++;
      break;
    default:
      break;
  }
  updateMotor(stateSince);                  // apply motor change immediately
}

void raiseFault(Fault f, const char* detail) {
  if (state == ST_FAULT) return;            // keep the first fault
  fault = f;
  cntFaults++;
  logEvent("FAULT", "%s %s", FAULT_NAMES[f], detail);
  enterState(ST_FAULT, FAULT_NAMES[f]);
}

void requestReset(const char* source, Print& out) {
  if (state == ST_FAULT) {
    if (USE_ESTOP_INPUT && !estopIn.state) { out.println("ERR reset refused: E-stop still active"); return; }
    if (USE_DRIVE_OK_INPUT && !driveOkIn.state) { out.println("ERR reset refused: drive still in fault"); return; }
    logEvent("RESET", "fault %s cleared by %s", FAULT_NAMES[fault], source);
    if (fault == F_RESULT_TIMEOUT) camReconnectRequest = true;   // start with a fresh camera link
    fault = F_NONE;
    enterState(ST_IDLE, "reset");
    out.println("OK reset");
  } else if (state == ST_STOPPED) {
    logEvent("RESET", "restart by %s", source);
    enterState(ST_IDLE, "restart");
    out.println("OK restarted");
  } else {
    out.println("OK nothing to reset");
  }
}

void requestStop(const char* source, Print& out) {
  if (state == ST_FAULT || state == ST_STOPPED) { out.println("OK already stopped"); return; }
  char reason[40];
  snprintf(reason, sizeof(reason), "stop by %s", source);
  enterState(ST_STOPPED, reason);
  out.println("OK stopped");
}

void runStateMachine(unsigned long now) {
  unsigned long inState = now - stateSince;

  switch (state) {
    case ST_IDLE:
      if (!sensorClearFor(s3, EXIT_CLEAR_DELAY_MS, now)) break;  // exit occupied: wait
      if (s2.state) { enterState(ST_SETTLE, "object already at camera"); break; }
      if (sensorStableFor(s1, START_DELAY_MS, now)) enterState(ST_RUN_TO_CAMERA, "S1 detected, S3 clear");
      break;

    case ST_RUN_TO_CAMERA:
      if (s2.state) { enterState(ST_SETTLE, "S2 detected"); break; }
      if (s3.state) { raiseFault(F_S3_UNEXPECTED, "S3 active before S2 - check S2 sensor"); break; }
      if (inState > TRAVEL_TO_CAMERA_TIMEOUT_MS) raiseFault(F_S2_TIMEOUT, "object did not reach S2 (jam / sensor / drive)");
      break;

    case ST_SETTLE:
      if (inState >= SETTLE_MS) enterState(ST_WAIT_RESULT, "settled");
      break;

    case ST_WAIT_RESULT:
      // The result arrives through acceptResult() (camera thread or SIM command).
      if (inState > RESULT_TIMEOUT_MS) {
        raiseFault(F_RESULT_TIMEOUT, cameraConnected() ? "camera connected but sent no result" : "camera not connected");
      }
      break;

    case ST_WAIT_EXIT_CLEAR:
      if (sensorClearFor(s3, EXIT_CLEAR_DELAY_MS, now)) enterState(ST_RUN_TO_EXIT, "S3 cleared");
      break;

    case ST_RUN_TO_EXIT:
      if (s3.state) { enterState(ST_AT_EXIT, "S3 detected"); break; }
      if (inState > TRAVEL_TO_EXIT_TIMEOUT_MS) raiseFault(F_S3_TIMEOUT, "object did not reach S3 (jam / sensor / drive)");
      break;

    case ST_AT_EXIT:
      if (!s3.state) {
        ngLamp = false;
        enterState(ST_IDLE, "product removed");
      }
      break;

    case ST_STOPPED:
    case ST_FAULT:
      break;
  }
}

void checkSafetyInputs(unsigned long now) {
  if (USE_ESTOP_INPUT && !estopIn.state) raiseFault(F_ESTOP, "E-stop input open (I4)");
  if (USE_DRIVE_OK_INPUT && now > DRIVE_BOOT_GRACE_MS && !driveOkIn.state) raiseFault(F_DRIVE, "ATV12 fault relay open (I5) - check drive display");
}

// =============================== Outputs ====================================
void updateMotor(unsigned long now) {
  bool want = (state == ST_RUN_TO_CAMERA || state == ST_RUN_TO_EXIT);
  if (!want) {
    if (motorOn) {
      digitalWrite(RELAY_MOTOR, LOW);
      motorOn = false;
      motorOffSince = now;
    }
  } else if (!motorOn && (now - motorOffSince) >= MIN_MOTOR_OFF_MS) {
    digitalWrite(RELAY_MOTOR, HIGH);
    motorOn = true;
  }
  digitalWrite(LED_D0, motorOn ? HIGH : LOW);
}

void updateWarnings(unsigned long now) {
  uint8_t w = 0;
  if (state == ST_WAIT_EXIT_CLEAR && (now - stateSince) >= EXIT_BLOCKED_WARN_MS) w |= W_EXIT_BLOCKED;
  if (!cameraConnected() && now > CAMERA_BOOT_GRACE_MS) w |= W_CAMERA_OFFLINE;
  if (!linkUp) w |= W_ETH_LINK_DOWN;

  if (w != warnings) {
    char txt[64];
    warningText(w, txt, sizeof(txt));
    logEvent("WARN", "%s", txt);
    warnings = w;
  }
}

void updateIndication(unsigned long now) {
  bool blink = ((now / BLINK_MS) % 2) == 0;
  bool interrupted = (state == ST_STOPPED) || (warnings != 0);

  if (state == ST_FAULT)  alarmOut = true;
  else if (interrupted)   alarmOut = blink;
  else                    alarmOut = false;

  digitalWrite(RELAY_ALARM, alarmOut ? HIGH : LOW);
  digitalWrite(LED_D1, alarmOut ? HIGH : LOW);
  digitalWrite(RELAY_NG, ngLamp ? HIGH : LOW);
  digitalWrite(LED_D2, ngLamp ? HIGH : LOW);
  bool systemOk = (state != ST_FAULT) && !interrupted;
  digitalWrite(RELAY_SYSTEM_OK, systemOk ? HIGH : LOW);                      // OUTPUT 4 green lamp
  digitalWrite(LED_D3, systemOk ? HIGH : LOW);
  digitalWrite(LEDR, alarmOut ? HIGH : LOW);                                 // red   = interrupt
  digitalWrite(LEDG, systemOk ? HIGH : LOW);                                 // green = healthy
  digitalWrite(LED_USER, cameraConnected() ? HIGH : LOW);                    // blue  = camera link
}

void fireCameraTrigger(const char* source) {
  if (!cameraConnected()) {
    logEvent("TRIGGER", "FAILED camera not connected (%s)", source);
    return;
  }
  camTriggerRequest = true;                 // the camera thread sends it
  logEvent("TRIGGER", "tcp \"%s\" requested (%s)", CAMERA_TRIGGER_STRING, source);
}

// ============================ Camera result =================================
bool containsWord(const char* msg, const char* word) {
  size_t wl = strlen(word);
  const char* p = msg;
  while (*p) {
    while (*p && !isalnum((unsigned char)*p)) p++;          // skip separators
    const char* start = p;
    while (*p && isalnum((unsigned char)*p)) p++;           // one token
    size_t tl = (size_t)(p - start);
    if (tl == wl && tl > 0) {
      bool same = true;
      for (size_t i = 0; i < tl; i++) {
        if (toupper((unsigned char)start[i]) != toupper((unsigned char)word[i])) { same = false; break; }
      }
      if (same) return true;
    }
  }
  return false;
}

bool wordInList(const char* w, const char* const* list, size_t n) {
  for (size_t i = 0; i < n; i++) {
    const char* a = w; const char* b = list[i];
    while (*a && *b && toupper((unsigned char)*a) == toupper((unsigned char)*b)) { a++; b++; }
    if (*a == '\0' && *b == '\0') return true;
  }
  return false;
}

// Parses "x;y;theta;result;" (',' also accepted, trailing separator optional).
bool parseCameraMessage(const char* msg, CamResult& r) {
  r.valid = false;
  r.x = r.y = r.theta = 0;
  r.result[0] = '\0';
  char tok[6][32];
  uint8_t nt = 0;
  const char* p = msg;
  for (;;) {
    const char* a = p;
    while (*p && *p != ';' && *p != ',') p++;
    const char* b = p;
    while (a < b && isspace((unsigned char)*a)) a++;
    while (b > a && isspace((unsigned char)b[-1])) b--;
    if (nt < 6) {
      size_t len = (size_t)(b - a);
      if (len > 31) len = 31;
      memcpy(tok[nt], a, len);
      tok[nt][len] = '\0';
      nt++;
    }
    if (*p == '\0') break;
    p++;
  }
  while (nt > 0 && tok[nt - 1][0] == '\0') nt--;          // trailing ';'
  if (nt < 4) return false;
  float v[3];
  for (uint8_t i = 0; i < 3; i++) {
    char* end = nullptr;
    v[i] = strtof(tok[i], &end);
    if (end == tok[i] || *end != '\0') return false;      // not a number
  }
  if (tok[3][0] == '\0') return false;
  r.x = v[0]; r.y = v[1]; r.theta = v[2];
  size_t k = 0;
  for (; tok[3][k] && k < sizeof(r.result) - 1; k++) r.result[k] = (char)toupper((unsigned char)tok[3][k]);
  r.result[k] = '\0';
  r.valid = true;
  return true;
}

ResultClass classifyResult(const char* msg, const CamResult& r) {
  const size_t nOk = sizeof(OK_WORDS) / sizeof(OK_WORDS[0]);
  const size_t nNg = sizeof(NG_WORDS) / sizeof(NG_WORDS[0]);
  if (r.valid) {
    if (wordInList(r.result, OK_WORDS, nOk)) return RES_OK;
    if (wordInList(r.result, NG_WORDS, nNg)) return RES_NG;
    return RES_OTHER;
  }
  for (size_t i = 0; i < nNg; i++) if (containsWord(msg, NG_WORDS[i])) return RES_NG;   // e.g. "NG" alone
  return RES_OTHER;
}

// Formats a float with 3 decimals without relying on printf float support.
void fmtFixed(char* out, size_t size, float v) {
  long scaled = (long)(v * 1000.0f + (v >= 0 ? 0.5f : -0.5f));
  const char* sign = (scaled < 0) ? "-" : "";
  if (scaled < 0) scaled = -scaled;
  snprintf(out, size, "%s%ld.%03ld", sign, scaled / 1000L, scaled % 1000L);
}

void acceptResult(const char* msg, const char* source, unsigned long receivedAt) {
  // Data that started arriving before the part had settled is stale (image taken
  // while the belt was still moving) and is ignored like data outside WAIT_RESULT.
  bool stale = (state == ST_WAIT_RESULT) && ((long)(receivedAt - stateSince) < 0);
  if (state != ST_WAIT_RESULT || stale) {
    // Not waiting for a result (camera in continuous mode, or part not yet stopped).
    static unsigned long lastIgnoredLog = 0;
    static uint32_t ignoredCount = 0;
    ignoredCount++;
    if (millis() - lastIgnoredLog >= IGNORED_LOG_INTERVAL_MS) {
      logEvent("IGNORED", "%lu msg(s) from %s in state %s%s, last: %s",
               (unsigned long)ignoredCount, source, STATE_NAMES[state],
               stale ? " (sent before the part settled)" : "", msg);
      lastIgnoredLog = millis();
      ignoredCount = 0;
    }
    return;
  }
  CamResult r;
  parseCameraMessage(msg, r);
  ResultClass rc = classifyResult(msg, r);
  bool isNg = (rc == RES_NG) || (rc == RES_OTHER && UNKNOWN_RESULT_IS_NG);
  lastResultClass = isNg ? RES_NG : RES_OK;
  strncpy(lastResultText, msg, sizeof(lastResultText) - 1);
  lastResultText[sizeof(lastResultText) - 1] = '\0';
  if (isNg) cntNg++; else cntOk++;
  ngLamp = isNg;

  if (r.valid) {
    lastX = r.x; lastY = r.y; lastTheta = r.theta;
    strncpy(lastCamResult, r.result, sizeof(lastCamResult) - 1);
    lastCamResult[sizeof(lastCamResult) - 1] = '\0';
    char xs[16], ys[16], ts[16];
    fmtFixed(xs, sizeof(xs), r.x); fmtFixed(ys, sizeof(ys), r.y); fmtFixed(ts, sizeof(ts), r.theta);
    logEvent("RESULT", "%s x=%s y=%s theta=%s result=%s from %s in %lu ms%s",
             isNg ? "NG" : "OK", xs, ys, ts, r.result, source, millis() - stateSince,
             isNg ? " -> NG lamp" : "");
  } else {
    strcpy(lastCamResult, "INVALID");
    logEvent("RESULT", "%s - not x;y;theta;result (raw \"%s\") from %s in %lu ms%s",
             isNg ? "NG" : "OK", msg, source, millis() - stateSince, isNg ? " -> NG lamp" : "");
  }

  publishResult(r, isNg, msg);

  if (s3.state) enterState(ST_WAIT_EXIT_CLEAR, "result received, S3 occupied");
  else          enterState(ST_RUN_TO_EXIT, "result received, S3 clear");
}

// ============================ Line framing ==================================
void resetRx(LineRx& rx) {
  rx.len = 0;
  rx.separators = 0;
  rx.buf[0] = '\0';
}

static void takeLine(LineRx& rx, char* out, size_t outSize) {
  rx.doneStartedAt = rx.startedAt;
  rx.buf[rx.len] = '\0';
  strncpy(out, rx.buf, outSize - 1);
  out[outSize - 1] = '\0';
  resetRx(rx);
}

// Feeds one byte. True when a complete message is in 'out'.
// A message ends at CR, LF, NUL or ETX; STX and other control bytes are ignored.
bool feedByte(LineRx& rx, int ch, char* out, size_t outSize, unsigned long now) {
  rx.lastByteAt = now;
  if (ch == '\r' || ch == '\n' || ch == 0 || ch == 0x03) {
    if (rx.len == 0) return false;
    takeLine(rx, out, outSize);
    return true;
  }
  if (ch < 0x20 || ch > 0x7E) return false;
  if (rx.len == 0) rx.startedAt = now;
  rx.buf[rx.len++] = (char)ch;
  if ((ch == ';' || ch == ',') && rx.endAfterSeparators > 0 && ++rx.separators >= rx.endAfterSeparators) {
    takeLine(rx, out, outSize);
    return true;
  }
  if (rx.len >= LINE_MAX - 1) { takeLine(rx, out, outSize); return true; }   // overflow
  return false;
}

// A message without terminator is complete after RX_FRAME_GAP_MS of silence.
bool frameTimeout(LineRx& rx, char* out, size_t outSize, unsigned long now) {
  if (rx.len > 0 && (now - rx.lastByteAt) >= RX_FRAME_GAP_MS) {
    takeLine(rx, out, outSize);
    return true;
  }
  return false;
}

// ============================ Camera thread =================================
// Only this thread touches the camera socket. It talks to loop() through netQ
// (messages, each stamped with the time its first byte arrived) and the cam* flags.
void netPost(NetMsgType type, unsigned long t, const char* fmt, ...) {
  char text[NETMSG_MAX];
  va_list args;
  va_start(args, fmt);
  vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);

  netMutex.lock();
  if (netQCount == NETQ_SIZE) { netQHead = (netQHead + 1) % NETQ_SIZE; netQCount--; }  // drop oldest
  uint8_t idx = (netQHead + netQCount) % NETQ_SIZE;
  netQ[idx].type = type;
  netQ[idx].t = t;
  strncpy(netQ[idx].text, text, NETMSG_MAX - 1);
  netQ[idx].text[NETMSG_MAX - 1] = '\0';
  netQCount++;
  netMutex.unlock();
}

bool netPop(NetMsg& out) {
  netMutex.lock();
  if (netQCount == 0) { netMutex.unlock(); return false; }
  out = netQ[netQHead];
  netQHead = (netQHead + 1) % NETQ_SIZE;
  netQCount--;
  netMutex.unlock();
  return true;
}

static bool sockIsAccepted = false;         // sockets from accept() are freed by close()

static void closeCameraSocket(TCPSocket*& sock, const char* why) {
  if (!sock) return;
  sock->close();
  if (!sockIsAccepted) delete sock;
  sock = nullptr;
  camConnected = false;
  netPost(NM_INFO, millis(), "disconnected (%s)", why);
}

static void configureCameraSocket(TCPSocket* sock) {
  int on = 1, idle = KEEPALIVE_IDLE_MS, intvl = KEEPALIVE_INTERVAL_MS;
  sock->setsockopt(NSAPI_SOCKET, NSAPI_KEEPALIVE, &on, sizeof(on));
  sock->setsockopt(NSAPI_SOCKET, NSAPI_KEEPIDLE, &idle, sizeof(idle));
  sock->setsockopt(NSAPI_SOCKET, NSAPI_KEEPINTVL, &intvl, sizeof(intvl));
  sock->set_timeout(SOCKET_POLL_MS);
}

void cameraTask() {
  TCPSocket* sock = nullptr;                // connection to the camera
  TCPSocket* listener = nullptr;            // server mode only
  LineRx rx;
  rx.endAfterSeparators = FRAME_END_SEPARATORS;
  resetRx(rx);
  uint8_t buf[128];
  char msg[LINE_MAX];
  char ipText[20];
  snprintf(ipText, sizeof(ipText), "%u.%u.%u.%u", CAMERA_IP[0], CAMERA_IP[1], CAMERA_IP[2], CAMERA_IP[3]);
  unsigned long lastAttempt = 0;
  bool firstAttempt = true;
  uint32_t failures = 0;

  for (;;) {
    bool link = (Ethernet.linkStatus() == LinkON);

    if (sock && !link) closeCameraSocket(sock, "Ethernet link down");
    if (camReconnectRequest) {
      camReconnectRequest = false;
      if (sock) closeCameraSocket(sock, "reconnect requested");
      firstAttempt = true;                                 // retry immediately
    }

    if (OPTA_IS_TCP_SERVER) {
      // ================= SERVER MODE: the camera connects to the Opta =================
      if (!listener) {
        if (!link) { delay(50); continue; }
        listener = new TCPSocket();
        int on = 1;
        bool ok = listener->open(Ethernet.getNetwork()) == NSAPI_ERROR_OK;
        if (ok) listener->setsockopt(NSAPI_SOCKET, NSAPI_REUSEADDR, &on, sizeof(on));
        ok = ok && listener->bind(CAMERA_PORT) == NSAPI_ERROR_OK;
        ok = ok && listener->listen(2) == NSAPI_ERROR_OK;
        if (!ok) {
          netPost(NM_INFO, millis(), "cannot listen on port %u - retry in 2 s", CAMERA_PORT);
          listener->close(); delete listener; listener = nullptr;
          delay(2000);
          continue;
        }
        listener->set_blocking(false);
        netPost(NM_INFO, millis(), "listening on port %u - waiting for the camera (SCMVS TCP Client)", CAMERA_PORT);
      }
      nsapi_error_t aerr = NSAPI_ERROR_OK;
      TCPSocket* incoming = listener->accept(&aerr);       // non-blocking
      if (incoming) {
        if (sock) closeCameraSocket(sock, "replaced by a new camera connection");
        sock = incoming;
        sockIsAccepted = true;
        configureCameraSocket(sock);
        resetRx(rx);
        camConnected = true;
        SocketAddress peer;
        sock->getpeername(&peer);
        netPost(NM_INFO, millis(), "connected from %s:%u (camera is the TCP client)",
                peer.get_ip_address() ? peer.get_ip_address() : "?", peer.get_port());
      }
      if (!sock) { camConnected = false; delay(20); continue; }
    } else {
      // ================= CLIENT MODE: the Opta connects to the camera =================
      if (!sock) {
        camConnected = false;
        if (!link || (!firstAttempt && millis() - lastAttempt < RECONNECT_INTERVAL_MS)) { delay(50); continue; }
        firstAttempt = false;
        lastAttempt = millis();

        sock = new TCPSocket();
        sockIsAccepted = false;
        if (sock->open(Ethernet.getNetwork()) != NSAPI_ERROR_OK) {
          delete sock; sock = nullptr;
          netPost(NM_INFO, millis(), "socket open failed");
          continue;
        }
        uint8_t ipBytes[4] = { CAMERA_IP[0], CAMERA_IP[1], CAMERA_IP[2], CAMERA_IP[3] };
        SocketAddress addr(ipBytes, NSAPI_IPv4, CAMERA_PORT);
        nsapi_error_t err = sock->connect(addr);            // may block for seconds: only this thread waits
        if (err != NSAPI_ERROR_OK && err != NSAPI_ERROR_IS_CONNECTED) {
          failures++;
          if (failures == 1 || failures % 15 == 0) {
            netPost(NM_INFO, millis(), "connect to %s:%u failed (err %d, attempt %lu) - retrying",
                    ipText, CAMERA_PORT, (int)err, (unsigned long)failures);
          }
          sock->close(); delete sock; sock = nullptr;
          continue;
        }
        configureCameraSocket(sock);
        resetRx(rx);
        failures = 0;
        camConnected = true;
        netPost(NM_INFO, millis(), "connected to %s:%u", ipText, CAMERA_PORT);
      }
    }

    // ---------------- send trigger ----------------
    if (camTriggerRequest) {
      camTriggerRequest = false;
      char t[48];
      int n = snprintf(t, sizeof(t), "%s%s", CAMERA_TRIGGER_STRING, TRIGGER_APPEND_CRLF ? "\r\n" : "");
      sock->set_timeout(SOCKET_SEND_TIMEOUT_MS);
      nsapi_size_or_error_t r = sock->send(t, (nsapi_size_t)n);
      sock->set_timeout(SOCKET_POLL_MS);
      if (r == n) netPost(NM_INFO, millis(), "trigger \"%s\" sent", CAMERA_TRIGGER_STRING);
      else { closeCameraSocket(sock, "trigger send failed"); continue; }
    }

    // ---------------- receive ----------------
    nsapi_size_or_error_t n = sock->recv(buf, sizeof(buf));  // waits max SOCKET_POLL_MS
    unsigned long now = millis();
    if (n > 0) {
      for (int i = 0; i < n; i++) {
        if (feedByte(rx, buf[i], msg, sizeof(msg), now)) netPost(NM_DATA, rx.doneStartedAt, "%s", msg);
      }
    } else if (n == 0) {
      closeCameraSocket(sock, "camera closed the connection");
      continue;
    } else if (n != NSAPI_ERROR_WOULD_BLOCK) {
      char why[40];
      snprintf(why, sizeof(why), "socket error %d", (int)n);
      closeCameraSocket(sock, why);
      continue;
    }
    if (frameTimeout(rx, msg, sizeof(msg), now)) netPost(NM_DATA, rx.doneStartedAt, "%s", msg);
  }
}

bool cameraConnected() {
  return camConnected;
}

void serviceCamera(unsigned long now) {
  NetMsg m;
  for (int i = 0; i < NETQ_SIZE && netPop(m); i++) {
    if (m.type == NM_DATA) acceptResult(m.text, "camera", m.t);
    else logEvent(m.type == NM_MQTT ? "MQTT" : "CAMERA", "%s", m.text);
  }
}

// ============================== MQTT thread =================================
// loop() only puts JSON payloads into mqttQ. This thread owns WiFi and the MQTT
// client, so a slow WiFi join or an unreachable broker never delays the conveyor.
void mqttEnqueue(const char* payload) {
  if (!USE_MQTT) return;
  mqttMutex.lock();
  if (mqttQCount == MQTT_QUEUE_SIZE) {                     // full: drop the oldest
    mqttQHead = (mqttQHead + 1) % MQTT_QUEUE_SIZE;
    mqttQCount--;
    mqttDropped++;
  }
  uint8_t idx = (mqttQHead + mqttQCount) % MQTT_QUEUE_SIZE;
  strncpy(mqttQ[idx], payload, MQTT_PAYLOAD_MAX - 1);
  mqttQ[idx][MQTT_PAYLOAD_MAX - 1] = '\0';
  mqttQCount++;
  mqttMutex.unlock();
}

// Copies the oldest payload without removing it (removed only after a good publish).
static bool mqttPeek(char* out) {
  mqttMutex.lock();
  bool any = mqttQCount > 0;
  if (any) strcpy(out, mqttQ[mqttQHead]);
  mqttMutex.unlock();
  return any;
}

static void mqttPopFront() {
  mqttMutex.lock();
  if (mqttQCount > 0) { mqttQHead = (mqttQHead + 1) % MQTT_QUEUE_SIZE; mqttQCount--; }
  mqttMutex.unlock();
}

void publishResult(const CamResult& r, bool isNg, const char* raw) {
  char safe[64];                                           // raw text without JSON-breaking chars
  size_t k = 0;
  for (size_t i = 0; raw[i] && k < sizeof(safe) - 1; i++) safe[k++] = (raw[i] == '"' || raw[i] == '\\') ? '\'' : raw[i];
  safe[k] = '\0';
  char payload[MQTT_PAYLOAD_MAX];
  if (r.valid) {
    char xs[16], ys[16], ts[16];
    fmtFixed(xs, sizeof(xs), r.x); fmtFixed(ys, sizeof(ys), r.y); fmtFixed(ts, sizeof(ts), r.theta);
    snprintf(payload, sizeof(payload),
             "{\"x\":%s,\"y\":%s,\"theta\":%s,\"result\":\"%s\",\"valid\":true,\"cycle\":%lu,\"raw\":\"%s\"}",
             xs, ys, ts, isNg ? "NG" : "OK", (unsigned long)(cntCycles + 1), safe);
  } else {
    snprintf(payload, sizeof(payload),
             "{\"x\":null,\"y\":null,\"theta\":null,\"result\":\"%s\",\"valid\":false,\"cycle\":%lu,\"raw\":\"%s\"}",
             isNg ? "NG" : "OK", (unsigned long)(cntCycles + 1), safe);
  }
  mqttEnqueue(payload);
  if (USE_MQTT && !mqttConnected) logEvent("MQTT", "offline - result queued (%u waiting)", (unsigned)mqttQCount);
}

void mqttTask() {
  unsigned long lastWifiAttempt = 0, lastMqttAttempt = 0;
  bool firstWifi = true, firstMqtt = true, wifiWasUp = false;
  char brokerText[20];
  snprintf(brokerText, sizeof(brokerText), "%u.%u.%u.%u",
           MQTT_BROKER_IP[0], MQTT_BROKER_IP[1], MQTT_BROKER_IP[2], MQTT_BROKER_IP[3]);
  char payload[MQTT_PAYLOAD_MAX];

  mqttClient.setId(MQTT_CLIENT_ID);
  mqttClient.setUsernamePassword(MQTT_USERNAME, MQTT_PASSWORD);
  mqttClient.setKeepAliveInterval(MQTT_KEEPALIVE_MS);
  mqttClient.setConnectionTimeout(5000);

  for (;;) {
    // ---- 1. network for MQTT ----
    if (!MQTT_USE_ETHERNET) {
      if (WiFi.status() != WL_CONNECTED) {
        if (wifiWasUp) { netPost(NM_MQTT, millis(), "WiFi lost"); wifiWasUp = false; }
        mqttConnected = false;
        if (!firstWifi && millis() - lastWifiAttempt < MQTT_RETRY_MS) { delay(100); continue; }
        firstWifi = false;
        lastWifiAttempt = millis();
        netPost(NM_MQTT, millis(), "WiFi connecting to \"%s\" ...", WIFI_SSID);
        int st = WiFi.begin(WIFI_SSID, WIFI_PASSWORD);      // blocks a few seconds (this thread only)
        if (st != WL_CONNECTED) {
          netPost(NM_MQTT, millis(), "WiFi connect failed (status %d) - retry in %lu s", st, MQTT_RETRY_MS / 1000UL);
          continue;
        }
        IPAddress ip = WiFi.localIP();
        netPost(NM_MQTT, millis(), "WiFi connected, IP %u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
        wifiWasUp = true;
        firstMqtt = true;                                    // connect to the broker right away
      }
    } else if (Ethernet.linkStatus() != LinkON) {
      mqttConnected = false;
      delay(200);
      continue;
    }

    // ---- 2. broker connection ----
    if (!mqttClient.connected()) {
      if (mqttConnected) { netPost(NM_MQTT, millis(), "broker connection lost"); mqttConnected = false; }
      if (!firstMqtt && millis() - lastMqttAttempt < MQTT_RETRY_MS) { delay(100); continue; }
      firstMqtt = false;
      lastMqttAttempt = millis();
      if (!mqttClient.connect(MQTT_BROKER_IP, MQTT_BROKER_PORT)) {
        // -2 = TCP connect failed (broker IP/port/route), 4 = bad user/password, 5 = not authorised
        netPost(NM_MQTT, millis(), "connect to %s:%u failed (error %d) - retry in %lu s",
                brokerText, MQTT_BROKER_PORT, mqttClient.connectError(), MQTT_RETRY_MS / 1000UL);
        mqttClient.stop();
        continue;
      }
      mqttConnected = true;
      netPost(NM_MQTT, millis(), "connected to broker %s:%u as \"%s\"", brokerText, MQTT_BROKER_PORT, MQTT_CLIENT_ID);
    }

    // ---- 3. publish queued results ----
    while (mqttPeek(payload)) {
      bool ok = mqttClient.beginMessage(MQTT_TOPIC, strlen(payload), MQTT_RETAIN, MQTT_QOS) &&
                mqttClient.print(payload) == strlen(payload) &&
                mqttClient.endMessage();
      if (!ok) {
        netPost(NM_MQTT, millis(), "publish failed - will retry after reconnect");
        mqttClient.stop();
        mqttConnected = false;
        break;
      }
      mqttPopFront();
      mqttPublished++;
      netPost(NM_MQTT, millis(), "published to %s: %s", MQTT_TOPIC, payload);
    }

    mqttClient.poll();                                       // keep-alive (PINGREQ)
    delay(20);
  }
}

// ============================== Serial / log ================================
void logEvent(const char* kind, const char* fmt, ...) {
  char body[380];
  va_list args;
  va_start(args, fmt);
  vsnprintf(body, sizeof(body), fmt, args);
  va_end(args);
  unsigned long t = millis();
  char line[420];
  snprintf(line, sizeof(line), "EVT %lu.%03lu %s %s", t / 1000UL, t % 1000UL, kind, body);
  Serial.println(line);
}

void serviceSerial(unsigned long now) {
  char line[LINE_MAX];
  for (int k = 0; k < 64 && Serial.available() > 0; k++) {
    if (feedByte(serialRx, Serial.read(), line, sizeof(line), now)) processCommand(line, Serial, "usb");
  }
  if (frameTimeout(serialRx, line, sizeof(line), now)) processCommand(line, Serial, "usb");
}

void checkEthernetLink(unsigned long now) {
  if (now - lastLinkCheck < LINK_CHECK_MS) return;
  lastLinkCheck = now;
  bool up = (Ethernet.linkStatus() == LinkON);
  if (up != linkUp) {
    linkUp = up;
    logEvent("NET", "ethernet link %s", up ? "UP" : "DOWN");
  }
}

void warningText(uint8_t w, char* out, size_t size) {
  if (w == 0) { snprintf(out, size, "NONE"); return; }
  snprintf(out, size, "%s%s%s",
           (w & W_EXIT_BLOCKED)   ? "EXIT_BLOCKED|"   : "",
           (w & W_CAMERA_OFFLINE) ? "CAMERA_OFFLINE|" : "",
           (w & W_ETH_LINK_DOWN)  ? "ETH_LINK_DOWN|"  : "");
  size_t n = strlen(out);
  if (n > 0 && out[n - 1] == '|') out[n - 1] = '\0';
}

void buildStatus(char* out, size_t size) {
  char w[64], xs[16], ys[16], ts[16];
  warningText(warnings, w, sizeof(w));
  fmtFixed(xs, sizeof(xs), lastX); fmtFixed(ys, sizeof(ys), lastY); fmtFixed(ts, sizeof(ts), lastTheta);
  snprintf(out, size,
           "STATUS state=%s fault=%s warn=%s S1=%d S2=%d S3=%d motor=%d alarm=%d ng=%d "
           "camera=%d link=%d mqtt=%d mqtt_queue=%u last=%s result=%s x=%s y=%s theta=%s cycles=%lu ok=%lu ngcount=%lu faults=%lu uptime=%lu",
           STATE_NAMES[state], FAULT_NAMES[fault], w,
           s1.state, s2.state, s3.state, motorOn, alarmOut, ngLamp,
           cameraConnected(), linkUp, (int)mqttConnected, (unsigned)mqttQCount, RESULT_NAMES[lastResultClass], lastCamResult, xs, ys, ts,
           (unsigned long)cntCycles, (unsigned long)cntOk, (unsigned long)cntNg,
           (unsigned long)cntFaults, millis() / 1000UL);
}

void printHelp(Print& out) {
  out.println("Commands: HELP | PING | STATUS | RESET | START | STOP | SIM <text> | TRIG | RECONNECT | CLEAR | VERSION");
  out.println("  SIM <text> : inject a camera result (only used in WAIT_RESULT), e.g. SIM 12.5;30.2;45.0;OK;");
  out.println("  TRIG       : send the camera trigger now (tests the camera link)");
  out.println("  RECONNECT  : drop the camera connection (client mode reconnects, server mode waits for the camera)");
}

void processCommand(char* line, Print& out, const char* source) {
  while (*line == ' ') line++;
  char* arg = strchr(line, ' ');
  if (arg) { *arg++ = '\0'; while (*arg == ' ') arg++; }
  for (char* p = line; *p; p++) *p = (char)toupper((unsigned char)*p);

  if (strcmp(line, "PING") == 0) {
    out.println("PONG");
  } else if (strcmp(line, "STATUS") == 0) {
    char st[380];
    buildStatus(st, sizeof(st));
    out.println(st);
  } else if (strcmp(line, "RESET") == 0 || strcmp(line, "START") == 0) {
    requestReset(source, out);
  } else if (strcmp(line, "STOP") == 0) {
    requestStop(source, out);
  } else if (strcmp(line, "SIM") == 0) {
    if (!arg || !*arg) { out.println("ERR usage: SIM <text>"); return; }
    if (state != ST_WAIT_RESULT) {
      out.print("ERR not waiting for a result, state="); out.println(STATE_NAMES[state]);
      return;
    }
    out.println("OK result injected");
    acceptResult(arg, "sim", millis());
  } else if (strcmp(line, "TRIG") == 0) {
    fireCameraTrigger(source);
  } else if (strcmp(line, "RECONNECT") == 0) {
    camReconnectRequest = true;
    out.println("OK reconnecting to camera");
  } else if (strcmp(line, "CLEAR") == 0) {
    cntCycles = cntOk = cntNg = cntFaults = 0;
    out.println("OK counters cleared");
  } else if (strcmp(line, "VERSION") == 0) {
    out.println(FW_VERSION);
  } else if (strcmp(line, "HELP") == 0 || strcmp(line, "?") == 0) {
    printHelp(out);
  } else {
    out.print("ERR unknown command: "); out.println(line);
  }
}

// ================================ Setup =====================================
void setup() {
  // Outputs first, all OFF (relay open = motor stopped).
  const uint8_t outs[] = { RELAY_MOTOR, RELAY_ALARM, RELAY_NG, RELAY_SYSTEM_OK,
                           LED_D0, LED_D1, LED_D2, LED_D3, LED_USER, LEDR, LEDG };
  for (uint8_t i = 0; i < sizeof(outs); i++) { pinMode(outs[i], OUTPUT); digitalWrite(outs[i], LOW); }

  Serial.begin(115200);
  delay(500);                                               // no while(!Serial): runs standalone

  unsigned long now = millis();
  initInput(s1, PIN_S1_ENTRY, SENSOR_ACTIVE_HIGH, SENSOR_DEBOUNCE_MS, now);
  initInput(s2, PIN_S2_CAMERA, SENSOR_ACTIVE_HIGH, SENSOR_DEBOUNCE_MS, now);
  initInput(s3, PIN_S3_EXIT, SENSOR_ACTIVE_HIGH, SENSOR_DEBOUNCE_MS, now);
  initInput(estopIn, PIN_ESTOP, true, SENSOR_DEBOUNCE_MS, now);
  initInput(driveOkIn, PIN_DRIVE_OK, true, DRIVE_FAULT_FILTER_MS, now);
  initInput(extResetIn, PIN_EXT_RESET, true, BUTTON_DEBOUNCE_MS, now);
  initInput(userBtn, BTN_USER, false, BUTTON_DEBOUNCE_MS, now);   // pressed = LOW
  serialRx.endAfterSeparators = 0;
  resetRx(serialRx);

  Serial.println();
  Serial.println(FW_VERSION);
  Serial.print("Opta IP "); Serial.print(OPTA_IP);
  if (OPTA_IS_TCP_SERVER) { Serial.print("  TCP SERVER, camera connects to port "); Serial.println(CAMERA_PORT); }
  else { Serial.print("  TCP CLIENT -> camera "); Serial.print(CAMERA_IP); Serial.print(':'); Serial.println(CAMERA_PORT); }
  // Waits at most ETH_BOOT_WAIT_MS for the cable; the link comes up later by itself.
  Ethernet.begin(nullptr, OPTA_IP, OPTA_DNS, OPTA_GATEWAY, OPTA_SUBNET, ETH_BOOT_WAIT_MS, 4000);
  linkUp = (Ethernet.linkStatus() == LinkON);

  stateSince = millis();
  motorOffSince = stateSince;
  logEvent("BOOT", "%s mode=%s link=%s trigger_mode=%d mqtt=%s", FW_VERSION,
           OPTA_IS_TCP_SERVER ? "TCP_SERVER" : "TCP_CLIENT", linkUp ? "UP" : "DOWN",
           (int)CAMERA_TRIGGER_MODE, USE_MQTT ? (MQTT_USE_ETHERNET ? "ETHERNET" : "WIFI") : "OFF");
  printHelp(Serial);

  cameraThread.start(cameraTask);
  if (USE_MQTT) mqttThread.start(mqttTask);
  if (USE_WATCHDOG) mbed::Watchdog::get_instance().start(WATCHDOG_MS);
}

// ================================= Loop =====================================
void loop() {
  unsigned long now = millis();

  // 1. Inputs
  updateInput(s1, now);
  updateInput(s2, now);
  updateInput(s3, now);
  updateInput(estopIn, now);
  updateInput(driveOkIn, now);
  updateInput(extResetIn, now);
  updateInput(userBtn, now);

  // 2. Safety + sequence
  checkSafetyInputs(now);
  runStateMachine(now);
  updateMotor(now);

  // 3. Local operator: USER button = STOP while running, RESET when stopped/faulted
  if (userBtn.rose) {
    if (state == ST_FAULT || state == ST_STOPPED) requestReset("button", Serial);
    else requestStop("button", Serial);
  }
  if (USE_EXT_RESET && extResetIn.rose) requestReset("I6", Serial);

  // 4. Camera messages + USB commands
  checkEthernetLink(now);
  serviceCamera(now);
  serviceSerial(now);

  // 5. Indication
  updateWarnings(now);
  updateIndication(now);

  if (USE_WATCHDOG) mbed::Watchdog::get_instance().kick();
  delay(1);                                 // lets the camera and network threads run
}
