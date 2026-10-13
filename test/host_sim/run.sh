#!/bin/sh
# Builds a deploy sketch on the PC with stubbed Arduino / mbed APIs and runs the
# product-switch sequence tests (no Opta needed).   sh test/host_sim/run.sh
set -e
cd "$(dirname "$0")"
for mode in client server; do
  sketch="../../arduino/opta_deploy_tcp_${mode}_mqtt/opta_deploy_tcp_${mode}_mqtt.ino"
  # the tests check the MQTT payloads: build with MQTT on whatever the sketch says
  sed 's/^const bool USE_MQTT *= *false;/const bool USE_MQTT = true;/' "$sketch" > "/tmp/sketch_${mode}.ino"
  g++ -std=c++17 -I. -x c++ -DSKETCH="\"/tmp/sketch_${mode}.ino\"" product_switch_test.cpp -o "/tmp/product_switch_${mode}"
  echo "== $mode"; "/tmp/product_switch_${mode}" | tail -1
done
