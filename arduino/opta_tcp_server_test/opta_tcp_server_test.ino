/*
  ============================================================================
   OPTA TCP SERVER TEST  -  Arduino Opta WiFi (Ethernet port)
   Pair: python/tcp_test_client.py  (Opta = server, PC = client)
  ============================================================================
   - Static IP 169.254.163.10 / 255.255.0.0, TCP server on port 8000,
     up to 4 clients at the same time.
   - Every byte received is printed on the USB Serial Monitor (115200) as
     text + HEX.
   - Every received line is forwarded to the other clients as
     "FROM <ip>:<port> <line>".
   - Relays are NOT used: safe to run with the motor drive wired.

   PC Ethernet adapter: 169.254.163.50 / 255.255.0.0
   PC runs:  python tcp_test_client.py --host 169.254.163.10

   Line protocol (text, CR/LF or LF terminated, case-insensitive commands)
     PING              -> PONG
     INFO              -> INFO ip=... clients=... uptime=... rx=... tx=...
     TIME              -> TIME <millis>
     LED <1-4> <0|1>   -> OK   (switches STATUS LED 1..4, visible check)
     PARSE <x;y;theta;result;> -> XYZR x=.. y=.. theta=.. result=..  or  XYZR ERR
     anything else     -> ECHO:<same text>

   LEDs: STATUS1 = Ethernet link up, STATUS2 = at least one client,
         STATUS3 = flashes on receive, blue USER LED = flashes on transmit
  ============================================================================
*/

#include <Ethernet.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------ Configuration --------------------------------
IPAddress OPTA_IP(169, 254, 163, 10);
IPAddress OPTA_SUBNET(255, 255, 0, 0);
IPAddress OPTA_GATEWAY(169, 254, 163, 1);   // not used
IPAddress OPTA_DNS(169, 254, 163, 1);       // not used
const uint16_t TEST_PORT = 8000;

const uint8_t  MAX_CLIENTS        = 4;
const bool     ECHO_ENABLED       = true;
const bool     FORWARD_TO_OTHERS  = true;
const bool     HEX_DUMP           = true;
const size_t   LINE_MAX           = 600;
const unsigned long RX_FRAME_GAP_MS = 50;   // un-terminated message ends after this silence
const unsigned long WRITE_TIMEOUT_MS = 200;
// ------------------------------------------------------------------------------

struct Slot {
  EthernetClient client;
  bool used;
  char ip[20];
  uint16_t port;
  char line[LINE_MAX];
  size_t len;
  unsigned long lastByteAt;
  uint32_t rxBytes, txBytes;
};

void handleLine(uint8_t idx, char* line);
void sendTo(uint8_t idx, const char* text);
void dumpBytes(uint8_t idx, const uint8_t* data, size_t n);
void serviceClients(unsigned long now);
void acceptClients(unsigned long now);
void closeSlot(uint8_t idx, const char* why);
uint8_t clientCount();
bool parseCam(const char* msg, float* v, char* result, size_t resultSize);
void fmtFixed(char* out, size_t size, float v);

EthernetServer server(TEST_PORT);
Slot slots[MAX_CLIENTS];
bool linkUp = false;
unsigned long lastLinkCheck = 0, rxLedUntil = 0, txLedUntil = 0;
uint32_t totalRx = 0, totalTx = 0;

const uint8_t STATUS_LEDS[4] = { LED_D0, LED_D1, LED_D2, LED_D3 };
bool userLedOverride[4] = { false, false, false, false };
bool userLedState[4]    = { false, false, false, false };

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

uint8_t clientCount() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) if (slots[i].used) n++;
  return n;
}

void sendTo(uint8_t idx, const char* text) {
  if (!slots[idx].used) return;
  size_t n = strlen(text);
  size_t w = slots[idx].client.write((const uint8_t*)text, n);
  w += slots[idx].client.write((const uint8_t*)"\r\n", 2);
  if (w != n + 2) { closeSlot(idx, "write failed"); return; }
  slots[idx].txBytes += w;
  totalTx += w;
  txLedUntil = millis() + 30;
}

void dumpBytes(uint8_t idx, const uint8_t* data, size_t n) {
  if (!HEX_DUMP) return;
  Serial.print("RX ["); Serial.print(slots[idx].ip); Serial.print("] ");
  Serial.print(n); Serial.print(" bytes  \"");
  for (size_t i = 0; i < n; i++) {
    uint8_t c = data[i];
    if (c == '\r') Serial.print("\\r");
    else if (c == '\n') Serial.print("\\n");
    else if (c >= 0x20 && c <= 0x7E) Serial.write(c);
    else Serial.print('.');
  }
  Serial.print("\"  HEX:");
  for (size_t i = 0; i < n; i++) {
    Serial.print(' ');
    if (data[i] < 0x10) Serial.print('0');
    Serial.print(data[i], HEX);
  }
  Serial.println();
}

void closeSlot(uint8_t idx, const char* why) {
  if (!slots[idx].used) return;
  slots[idx].client.stop();
  slots[idx].used = false;
  Serial.print("Client "); Serial.print(slots[idx].ip); Serial.print(':'); Serial.print(slots[idx].port);
  Serial.print(" closed ("); Serial.print(why); Serial.print(")  rx="); Serial.print(slots[idx].rxBytes);
  Serial.print(" tx="); Serial.println(slots[idx].txBytes);
}

void handleLine(uint8_t idx, char* line) {
  Serial.print("LINE ["); Serial.print(slots[idx].ip); Serial.print("] "); Serial.println(line);

  if (FORWARD_TO_OTHERS) {
    char fwd[LINE_MAX + 40];
    snprintf(fwd, sizeof(fwd), "FROM %s:%u %s", slots[idx].ip, slots[idx].port, line);
    for (uint8_t j = 0; j < MAX_CLIENTS; j++) if (j != idx && slots[j].used) sendTo(j, fwd);
  }

  char cmd[16];
  size_t k = 0;
  while (line[k] && line[k] != ' ' && k < sizeof(cmd) - 1) { cmd[k] = (char)toupper((unsigned char)line[k]); k++; }
  cmd[k] = '\0';
  const char* arg = line + k;
  while (*arg == ' ') arg++;

  char reply[LINE_MAX + 16];
  float v[3];
  if (strcmp(cmd, "PING") == 0 && line[4] == '\0') {
    sendTo(idx, "PONG");
  } else if (strcmp(cmd, "TIME") == 0) {
    snprintf(reply, sizeof(reply), "TIME %lu", millis());
    sendTo(idx, reply);
  } else if (strcmp(cmd, "INFO") == 0) {
    snprintf(reply, sizeof(reply), "INFO ip=%s port=%u link=%d clients=%u uptime=%lu rx=%lu tx=%lu",
             OPTA_IP.toString().c_str(), TEST_PORT, linkUp, clientCount(), millis() / 1000UL,
             (unsigned long)totalRx, (unsigned long)totalTx);
    sendTo(idx, reply);
  } else if (strcmp(cmd, "PARSE") == 0) {
    char res[16];
    if (parseCam(arg, v, res, sizeof(res))) {
      char xs[16], ys[16], ts[16];
      fmtFixed(xs, sizeof(xs), v[0]); fmtFixed(ys, sizeof(ys), v[1]); fmtFixed(ts, sizeof(ts), v[2]);
      snprintf(reply, sizeof(reply), "XYZR x=%s y=%s theta=%s result=%s", xs, ys, ts, res);
    } else {
      snprintf(reply, sizeof(reply), "XYZR ERR");
    }
    sendTo(idx, reply);
  } else if (strcmp(cmd, "LED") == 0) {
    int n = -1, val = -1;
    if (sscanf(arg, "%d %d", &n, &val) == 2 && n >= 1 && n <= 4 && (val == 0 || val == 1)) {
      userLedOverride[n - 1] = true;
      userLedState[n - 1] = (val == 1);
      sendTo(idx, "OK");
    } else {
      sendTo(idx, "ERR usage: LED <1-4> <0|1>");
    }
  } else if (ECHO_ENABLED) {
    snprintf(reply, sizeof(reply), "ECHO:%s", line);
    sendTo(idx, reply);
  }
}

void acceptClients(unsigned long now) {
  EthernetClient incoming = server.accept();
  if (!incoming) return;
  int idx = -1;
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) if (!slots[i].used) { idx = i; break; }
  if (idx < 0) {
    // All slots used. The mbed socket layer does not always notice a client
    // that closed politely, so free the slot that has been quiet the longest.
    idx = 0;
    for (uint8_t i = 1; i < MAX_CLIENTS; i++) if (slots[i].lastByteAt < slots[idx].lastByteAt) idx = i;
    closeSlot((uint8_t)idx, "evicted for new client");
  }
  Slot& s = slots[idx];
  s.client = incoming;
  s.client.setSocketTimeout(WRITE_TIMEOUT_MS);
  s.used = true;
  s.len = 0;
  s.lastByteAt = now;
  s.rxBytes = s.txBytes = 0;
  snprintf(s.ip, sizeof(s.ip), "%s", incoming.remoteIP().toString().c_str());
  s.port = incoming.remotePort();
  Serial.print("Client "); Serial.print(s.ip); Serial.print(':'); Serial.print(s.port);
  Serial.print(" connected (slot "); Serial.print(idx); Serial.println(')');
  char hello[64];
  snprintf(hello, sizeof(hello), "HELLO OPTA-TCP-SERVER-TEST slot=%d", idx);
  sendTo(idx, hello);
}

void serviceClients(unsigned long now) {
  uint8_t chunk[256];
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    Slot& s = slots[i];
    if (!s.used) continue;
    if (!s.client.connected()) { closeSlot(i, "peer closed"); continue; }

    int avail = s.client.available();
    if (avail > 0) {
      int n = s.client.read(chunk, (size_t)min(avail, (int)sizeof(chunk)));
      if (n > 0) {
        s.rxBytes += n;
        totalRx += n;
        s.lastByteAt = now;
        rxLedUntil = now + 30;
        dumpBytes(i, chunk, (size_t)n);
        for (int b = 0; b < n && s.used; b++) {
          char c = (char)chunk[b];
          if (c == '\r' || c == '\n' || c == 0) {
            if (s.len > 0) { s.line[s.len] = '\0'; s.len = 0; handleLine(i, s.line); }
          } else if (s.len < LINE_MAX - 1) {
            s.line[s.len++] = c;
          } else {
            s.line[s.len] = '\0'; s.len = 0; handleLine(i, s.line);
          }
        }
      }
    } else if (s.len > 0 && (now - s.lastByteAt) >= RX_FRAME_GAP_MS) {
      s.line[s.len] = '\0';
      s.len = 0;
      Serial.println("(message had no CR/LF terminator)");
      handleLine(i, s.line);
    }
  }
}

void setup() {
  for (uint8_t i = 0; i < 4; i++) { pinMode(STATUS_LEDS[i], OUTPUT); digitalWrite(STATUS_LEDS[i], LOW); }
  pinMode(LED_USER, OUTPUT);
  digitalWrite(LED_USER, LOW);
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) slots[i].used = false;

  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) {}

  Serial.println();
  Serial.println("=== OPTA TCP SERVER TEST ===");
  Serial.print("Static IP "); Serial.print(OPTA_IP); Serial.print("  port "); Serial.println(TEST_PORT);
  Serial.println("Waiting for Ethernet link (max 10 s)...");
  Ethernet.begin(nullptr, OPTA_IP, OPTA_DNS, OPTA_GATEWAY, OPTA_SUBNET, 10000, 4000);
  linkUp = (Ethernet.linkStatus() == LinkON);
  Serial.print("Ethernet link "); Serial.println(linkUp ? "UP" : "DOWN (check cable)");
  server.begin();
  Serial.println("Server listening. From the PC run:  python tcp_test_client.py --host 169.254.163.10");
}

void loop() {
  unsigned long now = millis();

  if (now - lastLinkCheck >= 500) {
    lastLinkCheck = now;
    bool up = (Ethernet.linkStatus() == LinkON);
    if (up != linkUp) { linkUp = up; Serial.print("Ethernet link "); Serial.println(up ? "UP" : "DOWN"); }
  }

  acceptClients(now);
  serviceClients(now);

  bool autoState[4] = { linkUp, clientCount() > 0, (long)(rxLedUntil - now) > 0, false };
  for (uint8_t i = 0; i < 4; i++) {
    bool on = userLedOverride[i] ? userLedState[i] : autoState[i];
    digitalWrite(STATUS_LEDS[i], on ? HIGH : LOW);
  }
  digitalWrite(LED_USER, (long)(txLedUntil - now) > 0 ? HIGH : LOW);
}
