#!/usr/bin/env python3
"""
MQTT monitor: prints every result and warning the Opta publishes, and can
publish a test message to check the broker without the Opta.

  pip install paho-mqtt

  python mqtt_monitor.py                   # subscribe and print results + warnings
  python mqtt_monitor.py --test            # also publish one fake result
  python mqtt_monitor.py --product ABCD    # send the product name for the next part
                                           # (what the scheduler does for every part)
"""

import argparse
import json
import time

import paho.mqtt.client as mqtt

MQTT_BROKER_HOST = "172.25.200.57"     # same broker as the Task-Scheduler-Agent .env
MQTT_BROKER_PORT = 1883
MQTT_USERNAME = "raghulrajg"
MQTT_PASSWORD = "Gr2_nemam"
MQTT_TOPIC = "autonomous/product/Transformation"
MQTT_WARNING_TOPIC = "autonomous/product/Warning"
MQTT_PRODUCT_TOPIC = "qc/qc_01/request"


def make_client():
    try:                                   # paho-mqtt 2.x
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION1, client_id="opta-monitor")
    except AttributeError:                 # paho-mqtt 1.x
        return mqtt.Client(client_id="opta-monitor")


def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print(f"Connected to {MQTT_BROKER_HOST}:{MQTT_BROKER_PORT}, subscribed to {MQTT_TOPIC}, {MQTT_WARNING_TOPIC}")
        client.subscribe([(MQTT_TOPIC, 0), (MQTT_WARNING_TOPIC, 0)])
    else:
        print(f"Connect failed, rc={rc} (4 = bad username/password, 5 = not authorised)")


def on_message(client, userdata, msg):
    stamp = time.strftime("%H:%M:%S")
    try:
        d = json.loads(msg.payload.decode())
        if msg.topic == MQTT_WARNING_TOPIC:
            print(f"{stamp}  WARNING {d.get('warning')} product={d.get('product')} cycle={d.get('cycle')}: "
                  f"{d.get('detail')}")
            return
        print(f"{stamp}  result={d.get('result', '?'):<3} x={d.get('x')} y={d.get('y')} theta={d.get('theta')} "
              f"valid={d.get('valid')} cycle={d.get('cycle')} product={d.get('product')} "
              f"switch_ok={d.get('switch_ok')} raw={d.get('raw')!r}")
    except (ValueError, AttributeError):
        print(f"{stamp}  {msg.topic}: {msg.payload!r}")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--test", action="store_true", help="publish one fake result after connecting")
    p.add_argument("--product", help=f"publish this product name on {MQTT_PRODUCT_TOPIC} for the next part")
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
    if args.product:
        time.sleep(1.0)
        request = {"request_id": f"manual-{int(time.time())}", "product": args.product, "ts": time.time()}
        client.publish(MQTT_PRODUCT_TOPIC, json.dumps(request), qos=1)
        print(f"Published product for the next part on {MQTT_PRODUCT_TOPIC}:", request)
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    client.loop_stop()
    client.disconnect()


if __name__ == "__main__":
    main()
