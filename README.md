# Conveyor Agent – Arduino Opta + Hikrobot SC3000 + MQTT

Conveyor control with a vision check. The Opta runs the belt with 3 proximity sensors and an ATV12 drive. It reads the camera result `x;y;theta;result` over TCP and publishes each result to MQTT.

Several products are supported: every part comes with its product name over MQTT, and the Opta switches the camera to that product's program (`switch <product>`) before the inspection. See [Multiple products](#multiple-products-camera-program-switch).

## Folder contents

```
conveyor_agent/
├── README.md
├── arduino/
│   ├── opta_tcp_server_test/          TCP test – Opta is SERVER   (pair: python/tcp_test_client.py)
│   ├── opta_tcp_client_test/          TCP test – Opta is CLIENT   (pair: python/tcp_test_server.py)
│   ├── opta_deploy_tcp_client_mqtt/   DEPLOY  – Opta is TCP CLIENT, camera is TCP SERVER, + MQTT
│   └── opta_deploy_tcp_server_mqtt/   DEPLOY  – Opta is TCP SERVER, camera is TCP CLIENT, + MQTT
└── python/
    ├── tcp_test_client.py    tests opta_tcp_server_test
    ├── tcp_test_server.py    tests opta_tcp_client_test
    ├── fake_camera.py        fake SC3000 for the deploy sketches (--mode server | client)
    ├── mqtt_monitor.py       shows the results arriving at the MQTT broker
    └── requirements.txt      paho-mqtt (only mqtt_monitor.py needs it)
test/host_sim/                PC build of the deploy sketches with stubbed Arduino/mbed APIs:
                              sh test/host_sim/run.sh   (product switch sequence tests, needs g++)
```

The two deploy sketches are the same program. The only difference is `OPTA_IS_TCP_SERVER` (`false` / `true`). Use the one that matches the camera's SCMVS communication setting.

| Camera setting in SCMVS | Upload | Test the deploy with |
|---|---|---|
| **TCP Server** (camera listens on port 8000) | `opta_deploy_tcp_client_mqtt` | `python fake_camera.py --mode server` |
| **TCP Client** (camera connects to the Opta) | `opta_deploy_tcp_server_mqtt` | `python fake_camera.py --mode client` |

## Arduino setup

1. Arduino IDE → Boards Manager → install **Arduino Mbed OS Opta Boards**.
2. Select board **Arduino Opta**, Target core **Main Core** (M7).
3. Library Manager → install **ArduinoMqttClient** (by Arduino). Only the deploy sketches need it.
4. Open the sketch folder and upload. Use the Serial Monitor at **115200** baud with any line ending.

## Network

| Device | IP / mask | Note |
|---|---|---|
| Opta (Ethernet) | 169.254.163.10 / 255.255.0.0 | static, set in every sketch |
| Camera SC3000 | 169.254.163.5 / 255.255.0.0 | port 8000 |
| PC for tests | 169.254.163.5 (when the PC replaces the camera) or 169.254.163.50 / 255.255.0.0 | |
| MQTT broker | 172.25.200.57 : 1883 | user `raghulrajg`. Same broker as `MQTT_BROKER_HOST` in the Task-Scheduler-Agent `.env` and the mobile app |

- The Opta has only one Ethernet port. Use a small switch if the camera and the PC must be connected at the same time.
- **MQTT goes over the Opta's WiFi by default.** The Ethernet port is on the camera's 169.254.x.x link and cannot reach the broker. Set `WIFI_SSID` and `WIFI_PASSWORD` in the deploy sketch.
  - The WiFi network must be able to reach the broker (172.25.200.57).
  - If the Opta, camera and broker are on one wired network instead, set `MQTT_USE_ETHERNET = true` and change `OPTA_IP` / `OPTA_SUBNET` to that network.
- Allow Python through the PC firewall for TCP port 8000.

## Camera data format

One message per inspection:

```
x;y;theta;result;          e.g.   12.5;30.2;45.0;OK;
```

- `,` is accepted instead of `;`. The trailing separator and CR/LF are optional.
- A message ends at CR/LF, at the 4th `;` or `,`, or after 50 ms of silence. Two results sent back to back are still split correctly.
- `result`:
  - **OK**: OK, PASS, PASSED, GOOD, TRUE, 1
  - **NG**: NG, NOK, FAIL, FAILED, BAD, FALSE, 0, NOREAD, ERROR
- Any other value, or a message that is not `x;y;theta;result`, counts as **NG** (`UNKNOWN_RESULT_IS_NG = true`).
- Only data that starts arriving **after the part has stopped and settled** at S2 is used. Results sent earlier, while the belt is moving or from a camera in continuous mode, are ignored.

## MQTT

| Topic | Direction | Payload |
|---|---|---|
| `autonomous/product/Transformation` | Opta → | one result per part (below) |
| `autonomous/product/Warning` | Opta → | `{"warning", "product", "cycle", "state", "detail"}` |
| `qc/qc_01/request` | → Opta | product name of the next part: `{"product":"ABCD", ...}` (sent by the scheduler's QC agent) or plain text `ABCD` |

Result (published for every part):

```json
{"x":12.500,"y":30.200,"theta":45.000,"result":"OK","valid":true,"cycle":5,"raw":"12.5;30.2;45.0;OK;","product":"ABCD","switch_ok":true}
{"x":10.000,"y":20.000,"theta":5.000,"result":"NG","valid":true,"cycle":6,"raw":"10;20;5;NG;","product":"ABCD","switch_ok":false}
{"x":null,"y":null,"theta":null,"result":"NG","valid":false,"cycle":7,"raw":"garbage","product":"ABCD","switch_ok":true}
```

`product` and `switch_ok` are `null` when the cycle had no product name (only possible with `REQUIRE_PRODUCT = false`).

Warnings:

| `warning` | When | Effect |
|---|---|---|
| `SWITCH_NO_ACK` | no `ok` within 10 s after `switch <product>` | OUTPUT 2 blinks until the part leaves; the cycle continues; result has `"switch_ok":false` |
| `NO_PRODUCT` | part at the camera, no product name within 30 s | FAULT `NO_PRODUCT` (needs RESET) |
| `PRODUCT_REJECTED` | product name empty, not a string, too long or with other characters than `A-Z a-z 0-9 space _ - .` | name ignored |
| `PRODUCT_STALE` | the product name waited more than 120 s for a part | name discarded |

MQTT runs in its own thread with a 16-message queue. A WiFi or broker outage never stops the conveyor. Results are kept and sent when the connection is back.

## Multiple products (camera program switch)

Each product has its own program (solution) on the camera. The Opta switches the program for every part:

1. The scheduler sends the product name of the part on `qc/qc_01/request` (it comes from the operator's message in the mobile app, e.g. "take ABCD from manufacturing_1 ..."). By hand: `python python/mqtt_monitor.py --product ABCD`, or `PRODUCT ABCD` in the Serial Monitor.
2. When the part has stopped at the camera (S2), the Opta sends `switch ABCD` to the camera over the camera TCP link.
3. It waits up to 10 s for `ok` (any case). Then it triggers the camera (if `TRIGGER_TCP`) and waits for the result as before.
4. No `ok` within 10 s: warning `SWITCH_NO_ACK` on the Serial Monitor and on `autonomous/product/Warning`, OUTPUT 2 blinks, and the cycle continues with `"switch_ok":false` in the result.

**One product name = one cycle.** Every part needs its own product name. It may arrive before the part or while the part waits at the camera. A part at the camera waits up to 30 s for its name, then the conveyor faults (`NO_PRODUCT`). After a STOP the part on the belt keeps its name; after a FAULT RESET only a part still at the camera keeps it.

In SCMVS, set up the communication so that the string `switch <program>` loads that program and answers `ok`, and name each program exactly like the product (the name the operator types in the app). `SWITCH_APPEND_CRLF = true` if the camera expects CR/LF after the command.

## Conveyor sequence (deploy sketches)

| State | Condition | Motor |
|---|---|---|
| IDLE | S1 detects the object **and** S3 is clear | ON |
| RUN_TO_CAMERA | S2 detects the object | OFF |
| SETTLE → WAIT_PRODUCT | wait for this part's product name (MQTT) | OFF |
| SWITCH_PROGRAM | send `switch <product>`, wait max 10 s for `ok` (no `ok` → warning, continue) | OFF |
| WAIT_RESULT | wait for the camera result (optional trigger) | OFF |
| result received | result is published to MQTT. If S3 is clear the motor starts; if S3 is occupied the conveyor waits | ON |
| RUN_TO_EXIT | S3 detects the object | OFF |
| AT_EXIT | object removed from S3 → back to IDLE | OFF |

**Interrupt indication**

| Condition | Indication | Clears with |
|---|---|---|
| Fault (S2 or S3 travel timeout, no camera result within 10 s, no product name within 30 s, S3 before S2, optional E-stop or drive fault) | OUTPUT 2 and red LED **steady** | RESET |
| Stop, camera offline, exit blocked, Ethernet link down, no `ok` to the program switch | OUTPUT 2 and red LED **blinking** | — |
| Everything normal | OUTPUT 4 green lamp and green LED **on** (off during any fault, stop or warning) | — |

RESET can come from the **USER button**, typing `RESET` in the Serial Monitor, or an optional push button on I6. The blue USER LED is on while the camera link is up.

## Wiring

| Opta | Connection |
|---|---|
| I1 (A0) | S1 entry sensor (24 V PNP, NO) |
| I2 (A1) | S2 camera-position sensor |
| I3 (A2) | S3 exit sensor |
| I4 (A3) | optional E-stop monitor (24 V = OK) → `USE_ESTOP_INPUT` |
| I5 (A4) | optional ATV12 fault relay: +24 V → R1C, R1A → I5 → `USE_DRIVE_OK_INPUT` |
| I6 (A5) | optional external RESET button → `USE_EXT_RESET` |
| OUTPUT 1 (D0) | ATV12 **+24V → LI1** (run forward) |
| OUTPUT 2 (D1) | alarm beacon / buzzer |
| OUTPUT 3 (D2) | NG lamp |
| OUTPUT 4 (D3) | green lamp **SYSTEM OK** (on = running normally; off = fault, stop or warning) |

- **ATV12 speed:** potentiometer between +10V / AI1 / COM.
- **Sensor 0 V:** must be common with the Opta's 0 V.
- **E-stop:** must be **hard-wired**. It has to break LI1 or the drive supply; the I4 input only reports it to the program.

**ATV12H037M2 parameters:** `tCC=2C`, `tCt=trn`, `Fr1=AI1`, `rrS=nO`, `bFr=50`, `UnS=230`, `nCr=1.9`.

- **Recommended:** set `SEt- dEC` to 0.2–0.5 s. The factory value of 3.0 s lets the part coast past the camera. Keep `SETTLE_MS` longer than `dEC`.
- **`tCt=trn`:** the drive needs a 0→1 edge on LI1 to start. The program always opens the relay for at least 300 ms before restarting, so every start is a clean edge.

## Camera trigger (`CAMERA_TRIGGER_MODE`)

| Mode | Use when |
|---|---|
| `TRIGGER_NONE` (default) | The camera triggers itself (continuous mode or its own sensor). |
| `TRIGGER_TCP` | SCMVS Trigger Source = Communication. The Opta sends `"start"` (= Communication String) when the part has settled. **Recommended if the camera sends only one result per part.** |

## Serial Monitor commands (deploy sketches)

| Command | Action |
|---|---|
| `STATUS` | state, sensors, motor, camera/MQTT link, last x/y/theta/result, counters |
| `RESET` / `START` | clear a fault / restart after STOP |
| `STOP` | stop the conveyor |
| `SIM 12.5;30.2;45.0;OK;` | inject a camera result (works only while waiting for a result) |
| `SIM ok` | inject the camera's answer to `switch <product>` (in SWITCH_PROGRAM) |
| `PRODUCT ABCD` | product name for the next part, as if received over MQTT |
| `TRIG` | send the camera trigger now |
| `RECONNECT` | drop the camera connection (client mode reconnects; server mode waits for the camera) |
| `CLEAR`, `VERSION`, `PING`, `HELP` | |

## Test procedure

### 1. TCP link tests

**A – Opta = server** (PC at 169.254.163.50)

```
upload arduino/opta_tcp_server_test
python python/tcp_test_client.py --host 169.254.163.10
```

**B – Opta = client** (PC at 169.254.163.5, the camera's IP)

```
upload arduino/opta_tcp_client_test
python python/tcp_test_server.py
```

Both run connect, ping, echo, `x;y;theta;result` parse, burst, LED, reconnect and info tests. Add `--tests soak --duration 600` for a 10-minute stability run.

**See exactly what the camera sends:** set `SNIFF_ONLY = true` in `opta_tcp_client_test`, connect the real camera (as TCP server) and watch the Serial Monitor. Every byte is shown as text and HEX, plus the parsed x/y/theta/result.

### 2. Deploy program without the camera

```
# camera = TCP server  ->  upload opta_deploy_tcp_client_mqtt, PC at 169.254.163.5
python python/fake_camera.py --mode server

# camera = TCP client  ->  upload opta_deploy_tcp_server_mqtt
python python/fake_camera.py --mode client --host 169.254.163.10
```

Trigger the sensors by hand. Give the part a product name (`PRODUCT ABCD` in the Serial Monitor, or `python python/mqtt_monitor.py --product ABCD`). The Opta sends `switch ABCD`, the fake camera answers `ok`. When the Serial Monitor shows `WAIT_RESULT`, press **Enter** in `fake_camera.py` to send an OK result.

| fake_camera.py | Action |
|---|---|
| Enter / `s` | send a random OK result |
| `ng` | send a random NG result |
| `xyz 12.5 30.2 45 OK` | send these values |
| `raw <text>` | send any text |
| `--interval 1` | continuous camera, one result per second |
| `--auto` | answer the TCP trigger |
| `switch <product>` from the Opta | answered with `ok` (`--no-switch-ack` or `switchack off` to test the warning) |
| `kick` | drop the connection to test reconnect |

### 3. MQTT

On a PC in the broker's network:

```
pip install -r python/requirements.txt
python python/mqtt_monitor.py          # prints every result and warning the Opta publishes
python python/mqtt_monitor.py --product ABCD   # product name for the next part
python python/mqtt_monitor.py --test   # also sends one fake result (checks broker + login)
```

## Main settings (top of the deploy sketch)

| Setting | Default | Meaning |
|---|---|---|
| `OPTA_IS_TCP_SERVER` | false / true | camera link mode |
| `CAMERA_IP`, `CAMERA_PORT` | 169.254.163.5, 8000 | camera address (port is the listen port in server mode) |
| `WIFI_SSID`, `WIFI_PASSWORD` | — | **must be filled in** for MQTT over WiFi |
| `MQTT_BROKER_IP`, `MQTT_BROKER_PORT` | 172.25.200.57, 1883 | broker (= `MQTT_BROKER_HOST` in the scheduler `.env`) |
| `MQTT_USERNAME`, `MQTT_PASSWORD` | raghulrajg / … | login |
| `MQTT_TOPIC` | autonomous/product/Transformation | results |
| `MQTT_PRODUCT_TOPIC` | qc/qc_01/request | product name of the next part (`qc/<station>/request` of the scheduler's QC agent) |
| `MQTT_WARNING_TOPIC` | autonomous/product/Warning | warnings |
| `REQUIRE_PRODUCT` | true | every cycle needs a product name |
| `SWITCH_COMMAND_PREFIX` | `"switch "` | camera command = prefix + product |
| `SWITCH_ACK`, `SWITCH_ACK_TIMEOUT_MS` | `ok`, 10000 | camera answer, max wait (then warning) |
| `SWITCH_APPEND_CRLF` | false | CR/LF after the switch command |
| `PRODUCT_WAIT_TIMEOUT_MS` | 30000 | part at the camera without a product name → FAULT |
| `PRODUCT_MAX_AGE_MS` | 120000 | older product names are discarded |
| `UNKNOWN_RESULT_IS_NG` | true | treat bad or unknown results as NG |
| `START_DELAY_MS` | 500 | S1 must be stable this long before the motor starts |
| `SETTLE_MS` | 400 | wait after stopping at S2 |
| `TRAVEL_TO_CAMERA_TIMEOUT_MS` | 15000 | fault if S2 is not reached |
| `RESULT_TIMEOUT_MS` | 10000 | fault if no camera result |
| `TRAVEL_TO_EXIT_TIMEOUT_MS` | 15000 | fault if S3 is not reached |
| `USE_WATCHDOG` | false | hardware watchdog; enable after commissioning |

## Troubleshooting

| Symptom | Check |
|---|---|
| `connect ... failed` repeating (client mode) | camera IP/port, SCMVS = TCP Server, cable, Opta mask 255.255.0.0 |
| No `connected from` (server mode) | SCMVS TCP Client target = 169.254.163.10 : 8000 |
| `FAULT NO_PRODUCT` | no product name for the part: is the scheduler's QC agent (`qc_01_conveyor`) running, and does the STATUS line show `mqtt=1`? Send one by hand with `PRODUCT <name>` |
| `WARN SWITCH_NO_ACK` | the camera did not answer `ok` to `switch <product>`: check the SCMVS communication setup and that a program with exactly that name exists. `SNIFF_ONLY` shows what the camera sends |
| `FAULT RESULT_TIMEOUT` | camera not sending, or its result arrived before the part settled → use `TRIGGER_TCP`. RESET also reconnects the camera link. |
| `RESULT NG - not x;y;theta;result` | check the real format with `SNIFF_ONLY` (`opta_tcp_client_test`) |
| `MQTT WiFi connect failed` | SSID/password; Opta WiFi firmware (run the *WiFiFirmwareUpdater* example once if needed) |
| `MQTT connect ... failed (error -2)` | broker not reachable from WiFi (IP route / firewall) |
| `MQTT connect ... failed (error 4 or 5)` | username / password / broker ACL |
| Part stops past the camera | lower ATV12 `dEC`, move S2 earlier |
| Camera accepts one client only | your PC script and the Opta cannot read the camera at the same time. Use the MQTT data from the Opta instead. |

## Verification status

- All 4 sketches compile against the Arduino Mbed OS Opta core (M7). **The 3.1 changes (product name, program switch) were compiled and tested on a PC with stubbed Arduino/mbed APIs only, not yet with the Opta core: compile once in the Arduino IDE before uploading.**
- **Program switch (3.1), PC simulation, both modes:** product over MQTT (JSON and plain text), `ok` in any case, data before the `ok` ignored, no `ok` → warning and the cycle continues, late `ok` not taken as a result, one product per cycle, no product → `NO_PRODUCT` fault, rejected and stale names, STOP/RESET keeping the name, camera offline at switch time. `fake_camera.py` answers `switch <product>` with `ok`.
- On a PC simulation:
  - **Deploy sketches, both modes:** full sequence including OK, NG and invalid results, back-to-back messages, comma format, reconnect, `SIM`, and the MQTT JSON payloads. 115 checks passed. MQTT was tested against a stub, not a real broker.
  - **TCP test pairs:** 8/8 tests passed for each.
- **Not yet tested on real hardware.** Check the wiring, drive and camera settings on site.
