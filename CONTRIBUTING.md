# Contributing to PinScope

Thanks for your interest. This is a personal project run by M.B. Parks under
Green Shoe Garage, but bug reports, pull requests, and ideas are welcome.

## What CI checks on every push

The workflow at `.github/workflows/ci.yml` runs five jobs:

1. **Lint.** JSHint against the embedded JS from `pinscope.html`, Prettier
   on text files, html-validate, and a hard fail on em dashes anywhere in
   the tracked sources.
2. **Compile serial firmware.** `pinscope.ino` compiled for Arduino Uno R3,
   Nano 33 IoT, and Uno R4 WiFi via arduino-cli.
3. **Compile BLE firmware.** `pinscope_ble.ino` for Nano 33 IoT and Uno R4
   WiFi, with the ArduinoBLE library.
4. **Compile MQTT firmware.** `pinscope_mqtt.ino` for the same two boards,
   with WiFiNINA and ArduinoMqttClient.
5. **Bridge syntax.** `pinscope_mqtt_bridge.py` checked with `py_compile`
   and `ruff`.

All five must pass before a PR can land.

## Running the checks locally

You don't need GitHub Actions to validate a change. Two helper scripts in
`scripts/` reproduce the most useful parts:

```sh
# Lint the embedded JS
scripts/lint.sh

# Compile a sketch against a board's core (requires arduino-cli on PATH)
scripts/compile.sh pinscope.ino arduino:renesas_uno:unor4wifi
```

`scripts/lint.sh` extracts the JS from `pinscope.html`, runs JSHint with
the repo's `.jshintrc`, and reports findings. `scripts/compile.sh` stages
the sketch in a temp folder of the same name and calls
`arduino-cli compile` against the FQBN you pass.

## Style conventions for PRs

- **No em dashes anywhere.** Replace with commas, periods, parentheses, or
  sentence restructuring. The CI grep will fail your build.
- **No external runtime dependencies in `pinscope.html`.** The console is a
  single self-contained file. Anything that needs a CDN script tag, a
  package install, or a build step does not belong in the artifact.
- **Document the wire protocol** if you add a command or a packet type.
  All three of the firmware sketches and the README's protocol table must
  stay in sync.
- **Update the SKILL of changes.** If you change a feature that the README
  describes, update the README in the same PR.
- **Match existing formatting.** Prettier handles the text files,
  JSHint handles the JS, and the Arduino sketches use 2-space indents with
  no tabs. The CI enforces this.

## Reporting bugs

Open an issue at https://github.com/mbparks/pinscope/issues with:

- A short description of what happened
- What you expected
- Browser + OS, board model and core version if firmware-related
- A capture file (`CAPTURE` button on the strip chart) if the bug is
  triggered by a specific data pattern

## License

By contributing, you agree your contributions are licensed under the same
GPL-3.0-or-later that the project uses.
