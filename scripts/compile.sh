#!/usr/bin/env bash
#
# scripts/compile.sh
#
# Compile a PinScope sketch against a chosen FQBN, the same way CI does.
# Stages the sketch into a temp folder named like the .ino so arduino-cli
# is happy with the flat repo layout.
#
# Usage:
#   scripts/compile.sh pinscope.ino arduino:avr:uno
#   scripts/compile.sh pinscope_ble.ino arduino:renesas_uno:unor4wifi
#   scripts/compile.sh pinscope_mqtt.ino arduino:samd:nano_33_iot
#
# Requires arduino-cli on your PATH and the relevant core+libraries already
# installed:
#   arduino-cli core install arduino:avr
#   arduino-cli core install arduino:samd
#   arduino-cli core install arduino:renesas_uno
#   arduino-cli lib install ArduinoBLE
#   arduino-cli lib install WiFiNINA
#   arduino-cli lib install ArduinoMqttClient
#
# GPL-3.0-or-later

set -euo pipefail
cd "$(dirname "$0")/.."

if [[ $# -lt 2 ]]; then
  echo "usage: $0 <sketch.ino> <fqbn>"
  echo "example: $0 pinscope.ino arduino:avr:uno"
  exit 1
fi

SKETCH="$1"
FQBN="$2"

if [[ ! -f "$SKETCH" ]]; then
  echo "error: sketch '$SKETCH' not found"
  exit 1
fi

if ! command -v arduino-cli >/dev/null 2>&1; then
  echo "error: arduino-cli not found. Install from https://arduino.github.io/arduino-cli/"
  exit 1
fi

STEM="${SKETCH%.ino}"
STAGE="$(mktemp -d -t pinscope-build-XXXXXX)"
mkdir -p "$STAGE/$STEM"
cp "$SKETCH" "$STAGE/$STEM/$SKETCH"

echo "[compile] $SKETCH -> $FQBN"
echo "[compile] staged at $STAGE/$STEM"
arduino-cli compile --fqbn "$FQBN" "$STAGE/$STEM"
echo "[compile] OK"
rm -rf "$STAGE"
