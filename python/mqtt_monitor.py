#!/usr/bin/env python3
"""
MQTT monitor: prints every result the Opta publishes, and can publish a test
message to check the broker without the Opta.

  pip install paho-mqtt

  python mqtt_monitor.py                 # subscribe and print results
  python mqtt_monitor.py --test          # also publish one fake result
"""

import argparse
import json
import time

import paho.mqtt.client as mqtt

MQTT_BROKER_HOST = "172.16.13.62"
MQTT_BROKER_PORT = 1883
MQTT_USERNAME = "raghulrajg"
MQTT_PASSWORD = "Gr2_nemam"
MQTT_TOPIC = "autonomous/product/Transformation"


def make_client():
    try:                                   # paho-mqtt 2.x
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION1, client_id="opta-monitor")
    except AttributeError:                 # paho-mqtt 1.x
        return mqtt.Client(client_id="opta-monitor")


def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print(f"Connected to {MQTT_BROKER_HOST}:{MQTT_BROKER_PORT}, subscribed to {MQTT_TOPIC}")
        client.subscribe(MQTT_TOPIC)
    else:
        print(f"Connect failed, rc={rc} (4 = bad username/password, 5 = not authorised)")


def on_message(client, userdata, msg):
    stamp = time.strftime("%H:%M:%S")
    try:
        d = json.loads(msg.payload.decode())
        print(f"{stamp}  result={d.get('result', '?'):<3} x={d.get('x')} y={d.get('y')} theta={d.get('theta')} "
              f"valid={d.get('valid')} cycle={d.get('cycle')} raw={d.get('raw')!r}")
    except (ValueError, AttributeError):
        print(f"{stamp}  {msg.topic}: {msg.payload!r}")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--test", action="store_true", help="publish one fake result after connecting")
    args = p.parse_args()

    client = make_client()
    client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)
    client.on_connect = on_connect
    client.on_message = on_message
    client.on_disconnect = lambda c, u, rc: print(f"Disconnected (rc={rc}) - reconnecting...")
    client.connect(MQTT_BROKER_HOST, MQTT_BROKER_PORT, keepalive=30)
    client.loop_start()
    if args.test:
        time.sleep(1.0)
        fake = {"x": 1.0, "y": 2.0, "theta": 3.0, "result": "OK", "valid": True, "cycle": 0, "raw": "1;2;3;OK;"}
        client.publish(MQTT_TOPIC, json.dumps(fake))
        print("Published test message:", fake)
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    client.loop_stop()
    client.disconnect()


if __name__ == "__main__":
    main()
