/*
  ============================================================================
   OPTA TCP CLIENT TEST  -  Arduino Opta WiFi (Ethernet port)
   Pair: python/tcp_test_server.py  (Opta = client, PC = server)
  ============================================================================
   The Opta connects as a TCP CLIENT to a TCP server, exactly like it will
   connect to the Hikrobot camera in the conveyor program.

   Two ways to use it
   1) Link test with the PC
        PC Ethernet adapter: 169.254.163.5 / 255.255.0.0 (the camera's IP)
        PC runs:  python tcp_test_server.py
        The script runs PING / ECHO / PARSE / burst / reconnect tests.
   2) Sniff the real camera
        Plug in the SC3000, set SNIFF_ONLY = true and upload. The Opta never
        transmits; it prints every byte the camera sends (text + HEX) and the
        parsed x;y;theta;result values on the Serial Monitor (115200 baud).

   Protocol (server -> Opta; a message ends with CR/LF or 50 ms of silence)
     PING              -> PONG
     ECHO <text>       -> ECHO:<text>
     PARSE <text>      -> XYZR x=.. y=.. theta=.. result=..  or  XYZR ERR
     INFO              -> INFO ip=... server=... uptime=... rx=... tx=...
     TIME              -> TIME <millis>
     LED <1-4> <0|1>   -> OK   (switches STATUS LED 1..4 - visible check)
     anything else     -> no reply; printed on Serial and parsed as x;y;theta;result
   After connecting the Opta sends "HELLO OPTA-TCP-CLIENT-TEST ..."
   (not in SNIFF_ONLY mode). Relays are not used - safe with the drive wired.

   LEDs: STATUS1 = Ethernet link, STATUS2 = connected to server,
         STATUS3 = flashes on receive, blue USER LED = flashes on transmit
  ============================================================================
*/

#include <Ethernet.h>
#include "mbed.h"
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

// ------------------------------ Configuration --------------------------------
IPAddress SERVER_IP(169, 254, 163, 5);     // PC running tcp_test_server.py, or the camera
const uint16_t SERVER_PORT = 8000;
IPAddress OPTA_IP(169, 254, 163, 10);
IPAddress OPTA_SUBNET(255, 255, 0, 0);
IPAddress OPTA_GATEWAY(169, 254, 163, 1);  // not used
IPAddress OPTA_DNS(169, 254, 163, 1);      // not used

const bool SNIFF_ONLY = false;             // true = never transmit (use with the real camera)
const bool HEX_DUMP   = true;
const unsigned long RECONNECT_DELAY_MS = 1000;
const unsigned long RX_FRAME_GAP_MS    = 50;
const int  POLL_MS = 10;
const size_t LINE_MAX = 600;
// ------------------------------------------------------------------------------

TCPSocket* sock = nullptr;
char line[LINE_MAX];
size_t lineLen = 0;
unsigned long lastByteAt = 0, lastAttempt = 0, rxLedUntil = 0, txLedUntil = 0;
unsigned long connectedAt = 0;
uint32_t rxBytes = 0, txBytes = 0, connects = 0;
bool linkUp = false;
bool ledOverride[4] = { false, false, false, false };
bool ledState[4]    = { false, false, false, false };
const uint8_t STATUS_LEDS[4] = { LED_D0, LED_D1, LED_D2, LED_D3 };

void closeSocket(const char* why);
void sendLine(const char* text);
void handleLine(char* msg);
bool parseCam(const char* msg, float* v, char* result, size_t resultSize);
void fmtFixed(char* out, size_t size, float v);
void dumpBytes(const uint8_t* data, int n);
void tryConnect();

// Parses "x;y;theta;result;" (',' also accepted, trailing separator optional).
bool parseCam(const char* msg, float* v, char* result, size_t resultSize) {
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
  while (nt > 0 && tok[nt - 1][0] == '\0') nt--;
  if (nt < 4 || tok[3][0] == '\0') return false;
  for (uint8_t i = 0; i < 3; i++) {
    char* end = nullptr;
    v[i] = strtof(tok[i], &end);
    if (end == tok[i] || *end != '\0') return false;
  }
  size_t k = 0;
  for (; tok[3][k] && k < resultSize - 1; k++) result[k] = (char)toupper((unsigned char)tok[3][k]);
  result[k] = '\0';
  return true;
}

void fmtFixed(char* out, size_t size, float v) {
  long scaled = (long)(v * 1000.0f + (v >= 0 ? 0.5f : -0.5f));
  const char* sign = (scaled < 0) ? "-" : "";
  if (scaled < 0) scaled = -scaled;
  snprintf(out, size, "%s%ld.%03ld", sign, scaled / 1000L, scaled % 1000L);
}

void dumpBytes(const uint8_t* data, int n) {
  if (!HEX_DUMP) return;
  Serial.print("RX "); Serial.print(n); Serial.print(" bytes  \"");
  for (int i = 0; i < n; i++) {
    uint8_t c = data[i];
    if (c == '\r') Serial.print("\\r");
    else if (c == '\n') Serial.print("\\n");
    else if (c >= 0x20 && c <= 0x7E) Serial.write(c);
    else Serial.print('.');
  }
  Serial.print("\"  HEX:");
  for (int i = 0; i < n; i++) {
    Serial.print(' ');
    if (data[i] < 0x10) Serial.print('0');
    Serial.print(data[i], HEX);
  }
  Serial.println();
}

void closeSocket(const char* why) {
  if (!sock) return;
  sock->close();
  delete sock;
  sock = nullptr;
  lineLen = 0;
  lastAttempt = millis();
  Serial.print("Disconnected ("); Serial.print(why); Serial.print(")  rx="); Serial.print(rxBytes);
  Serial.print(" tx="); Serial.println(txBytes);
}

void sendLine(const char* text) {
  if (!sock || SNIFF_ONLY) return;
  char buf[LINE_MAX + 16];
  int n = snprintf(buf, sizeof(buf), "%s\r\n", text);
  if (n <= 0) return;
  if ((size_t)n >= sizeof(buf)) n = sizeof(buf) - 1;
  sock->set_timeout(1000);
  nsapi_size_or_error_t r = sock->send(buf, (nsapi_size_t)n);
  sock->set_timeout(POLL_MS);
  if (r != n) { closeSocket("send failed"); return; }
  txBytes += n;
  txLedUntil = millis() + 30;
}

void handleLine(char* msg) {
  Serial.print("MSG \""); Serial.print(msg); Serial.println('"');

  char cmd[16];
  size_t k = 0;
  while (msg[k] && msg[k] != ' ' && k < sizeof(cmd) - 1) { cmd[k] = (char)toupper((unsigned char)msg[k]); k++; }
  cmd[k] = '\0';
  const char* arg = msg + k;
  while (*arg == ' ') arg++;

  char reply[LINE_MAX + 16];
  float v[3];
  if (strcmp(cmd, "PING") == 0) {
    sendLine("PONG");
  } else if (strcmp(cmd, "ECHO") == 0) {
    snprintf(reply, sizeof(reply), "ECHO:%s", arg);
    sendLine(reply);
  } else if (strcmp(cmd, "PARSE") == 0) {
    char res[16];
    if (parseCam(arg, v, res, sizeof(res))) {
      char xs[16], ys[16], ts[16];
      fmtFixed(xs, sizeof(xs), v[0]); fmtFixed(ys, sizeof(ys), v[1]); fmtFixed(ts, sizeof(ts), v[2]);
      snprintf(reply, sizeof(reply), "XYZR x=%s y=%s theta=%s result=%s", xs, ys, ts, res);
    } else {
      snprintf(reply, sizeof(reply), "XYZR ERR");
    }
    sendLine(reply);
  } else if (strcmp(cmd, "TIME") == 0) {
    snprintf(reply, sizeof(reply), "TIME %lu", millis());
    sendLine(reply);
  } else if (strcmp(cmd, "INFO") == 0) {
    snprintf(reply, sizeof(reply), "INFO ip=%s server=%s:%u link=%d uptime=%lu connects=%lu rx=%lu tx=%lu",
             OPTA_IP.toString().c_str(), SERVER_IP.toString().c_str(), SERVER_PORT, linkUp,
             millis() / 1000UL, (unsigned long)connects, (unsigned long)rxBytes, (unsigned long)txBytes);
    sendLine(reply);
  } else if (strcmp(cmd, "LED") == 0) {
    int n = -1, val = -1;
    if (sscanf(arg, "%d %d", &n, &val) == 2 && n >= 1 && n <= 4 && (val == 0 || val == 1)) {
      ledOverride[n - 1] = true;
      ledState[n - 1] = (val == 1);
      sendLine("OK");
    } else {
      sendLine("ERR usage: LED <1-4> <0|1>");
    }
  } else {
    // Camera data: no reply, just show what it would mean to the conveyor program.
    char res[16];
    if (parseCam(msg, v, res, sizeof(res))) {
      char xs[16], ys[16], ts[16];
      fmtFixed(xs, sizeof(xs), v[0]); fmtFixed(ys, sizeof(ys), v[1]); fmtFixed(ts, sizeof(ts), v[2]);
      Serial.print("    camera result: x="); Serial.print(xs); Serial.print(" y="); Serial.print(ys);
      Serial.print(" theta="); Serial.print(ts); Serial.print(" result="); Serial.println(res);
    } else {
      Serial.println("    not x;y;theta;result - the deploy program would treat this as NG");
    }
  }
}

void tryConnect() {
  Serial.print("Connecting to "); Serial.print(SERVER_IP); Serial.print(':'); Serial.print(SERVER_PORT);
  Serial.println(" ...");
  sock = new TCPSocket();
  if (sock->open(Ethernet.getNetwork()) != NSAPI_ERROR_OK) {
    Serial.println("  socket open failed");
    delete sock; sock = nullptr; lastAttempt = millis();
    return;
  }
  uint8_t ip[4] = { SERVER_IP[0], SERVER_IP[1], SERVER_IP[2], SERVER_IP[3] };
  SocketAddress addr(ip, NSAPI_IPv4, SERVER_PORT);
  unsigned long t0 = millis();
  nsapi_error_t err = sock->connect(addr);                  // blocks until connected or failed
  if (err != NSAPI_ERROR_OK && err != NSAPI_ERROR_IS_CONNECTED) {
    unsigned long took = millis() - t0;
    Serial.print("  connect failed, error "); Serial.print((int)err);
    Serial.print(" after "); Serial.print(took); Serial.println(" ms");
    if (took < 1000) Serial.println("  -> fast refusal: the host answers but no server is listening on that port");
    else             Serial.println("  -> no answer: check server IP, subnet mask, cable, and that the server is on");
    sock->close(); delete sock; sock = nullptr; lastAttempt = millis();
    return;
  }
  sock->set_timeout(POLL_MS);
  connects++;
  connectedAt = millis();
  lineLen = 0;
  Serial.print("  CONNECTED in "); Serial.print(millis() - t0); Serial.println(" ms");
  if (!SNIFF_ONLY) {
    char hello[80];
    snprintf(hello, sizeof(hello), "HELLO OPTA-TCP-CLIENT-TEST ip=%s connects=%lu",
             OPTA_IP.toString().c_str(), (unsigned long)connects);
    sendLine(hello);
  }
}

void setup() {
  for (uint8_t i = 0; i < 4; i++) { pinMode(STATUS_LEDS[i], OUTPUT); digitalWrite(STATUS_LEDS[i], LOW); }
  pinMode(LED_USER, OUTPUT);
  digitalWrite(LED_USER, LOW);

  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) {}          // wait max 3 s for the Serial Monitor

  Serial.println();
  Serial.println("=== OPTA TCP CLIENT TEST ===");
  Serial.print("Opta IP "); Serial.print(OPTA_IP); Serial.print("  server "); Serial.print(SERVER_IP);
  Serial.print(':'); Serial.print(SERVER_PORT); Serial.println(SNIFF_ONLY ? "  (SNIFF ONLY)" : "");
  Serial.println("Waiting for Ethernet link (max 10 s)...");
  Ethernet.begin(nullptr, OPTA_IP, OPTA_DNS, OPTA_GATEWAY, OPTA_SUBNET, 10000, 4000);
  linkUp = (Ethernet.linkStatus() == LinkON);
  Serial.print("Ethernet link "); Serial.println(linkUp ? "UP" : "DOWN (check cable)");
}

void loop() {
  unsigned long now = millis();
  bool up = (Ethernet.linkStatus() == LinkON);
  if (up != linkUp) { linkUp = up; Serial.print("Ethernet link "); Serial.println(up ? "UP" : "DOWN"); }
  if (!linkUp && sock) closeSocket("link down");

  if (!sock) {
    if (linkUp && now - lastAttempt >= RECONNECT_DELAY_MS) tryConnect();
    else delay(10);
  } else {
    uint8_t buf[256];
    nsapi_size_or_error_t n = sock->recv(buf, sizeof(buf));   // waits max POLL_MS
    now = millis();
    if (n > 0) {
      rxBytes += n;
      rxLedUntil = now + 30;
      lastByteAt = now;
      dumpBytes(buf, n);
      for (int i = 0; i < n && sock; i++) {
        char c = (char)buf[i];
        if (c == '\r' || c == '\n' || c == 0) {
          if (lineLen > 0) { line[lineLen] = '\0'; lineLen = 0; handleLine(line); }
        } else if (lineLen < LINE_MAX - 1) {
          line[lineLen++] = c;
        } else {
          line[lineLen] = '\0'; lineLen = 0; handleLine(line);
        }
      }
    } else if (n == 0) {
      closeSocket("server closed the connection");
    } else if (n != NSAPI_ERROR_WOULD_BLOCK) {
      char why[32];
      snprintf(why, sizeof(why), "socket error %d", (int)n);
      closeSocket(why);
    }
    if (sock && lineLen > 0 && now - lastByteAt >= RX_FRAME_GAP_MS) {
      line[lineLen] = '\0';
      lineLen = 0;
      Serial.println("    (message had no CR/LF ending)");
      handleLine(line);
    }
  }

  bool autoState[4] = { linkUp, sock != nullptr, (long)(rxLedUntil - millis()) > 0, false };
  for (uint8_t i = 0; i < 4; i++) digitalWrite(STATUS_LEDS[i], (ledOverride[i] ? ledState[i] : autoState[i]) ? HIGH : LOW);
  digitalWrite(LED_USER, (long)(txLedUntil - millis()) > 0 ? HIGH : LOW);
}
