# PinScope · Field Instrument 005

[![CI](https://github.com/mbparks/pinscope/actions/workflows/ci.yml/badge.svg)](https://github.com/mbparks/pinscope/actions/workflows/ci.yml)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/License-GPL--3.0--or--later-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Single-file](https://img.shields.io/badge/runtime-single--file%20HTML-d4a017)](pinscope.html)

Single-file HTML console for the Arduino Uno Q (and any classic Uno-compatible
board) over USB serial, WiFi, MQTT, or BLE. Drop the page into a browser,
connect to a running firmware, and you get:

- a live map of every digital and analog pin
- click-to-toggle outputs, sliders for PWM, an interrupt-counting FREQ mode for
  pulse and tachometer signals
- a strip chart with per-trace stats, calibration in real units, threshold
  alerts, paused/CSV/PNG export
- an FFT view and a waterfall spectrogram for any analog or virtual channel
- an XY scatter (Lissajous-style) with optional connecting lines
- an I2C / Qwiic scanner and four poll slots that feed virtual channels
- a cross-pin math engine: derive a virtual channel as an expression of any
  other channels (e.g. `v0 = sqrt(a0*a0 + a1*a1)`)
- an RGB LED panel that drives any three PWM pins from a color picker
- a per-device session that auto-saves to localStorage, plus JSON
  export and import

No build step. No framework. No external assets at runtime once loaded.

The visual language is the same amber-on-dark "field instrument" treatment
used in the rest of the M.B. Parks tool series.

## Quick start

1. Flash `pinscope.ino` onto your Uno Q (or any Arduino with a Wire-capable
   core). The sketch uses only `Arduino.h` and `Wire.h`, both included with
   the core. Baud rate is 115200.
2. Open `pinscope.html` in Chrome, Edge, or any Chromium browser. Firefox and
   Safari work too if you only use the WiFi transport; the serial transport
   needs the Web Serial API (Chromium-based).
3. Click SERIAL and pick the board's COM port, or click WIFI and point it at
   a WebSocket bridge (any small Python or Node service that pipes between a
   WebSocket and the serial port works).
4. The pin grid lights up. Click a pin's mode badge to cycle through OFF, IN,
   IN-PU, OUT, PWM (when applicable), and FREQ (on interrupt-capable pins).

## Wire protocol

Both directions exchange line-delimited JSON at 115200 baud. One object per
line, terminated by `\n` or `\r\n`.

### Host to board

| Command | Fields        | Notes                                            |
| ------- | ------------- | ------------------------------------------------ |
| `hello` | none          | Board responds with hello and a state packet.    |
| `poll`  | none          | Board responds with one state packet.            |
| `mode`  | `pin`, `mode` | Modes: `off`, `in`, `inp`, `out`, `pwm`, `freq`. |
| `set`   | `pin`, `val`  | Drives a digital output 0 or 1.                  |
| `pwm`   | `pin`, `val`  | 0..255 duty on a PWM-capable pin in PWM mode.    |
| `hz`    | `val`         | State-push rate in Hz, 1..50. Default 10.        |
| `i2c`   | `op`, ...     | See I2C section below.                           |

### Board to host

```
{"t":"hello","id":"FI005-XXXX","name":"Arduino Uno Q","hz":10}
{"t":"state","d":[14 ints],"a":[6 ints],"m":[14 strings],"v":[4 ints or null],"f":[14 ints]}
{"t":"ack","cmd":"..."}
{"t":"err","msg":"..."}
{"t":"i2c","op":"scan","addrs":[...]}
{"t":"i2c","op":"read","addr":N,"reg":N,"data":[...]}
```

The `f` array carries the measured frequency in Hz for pins in FREQ mode, or
`-1` for pins not in FREQ mode. The firmware computes new Hz values every
250 ms by counting RISING edges via `attachInterrupt`.

### I2C ops

| Op         | Fields                                         | Notes                                          |
| ---------- | ---------------------------------------------- | ---------------------------------------------- |
| `scan`     | none                                           | Board scans 0x01..0x77 and reports responders. |
| `read`     | `addr`, `reg`, `count`                         | Reads `count` bytes (1..8).                    |
| `write`    | `addr`, `reg`, `data`                          | Writes a byte array.                           |
| `poll`     | `slot`, `addr`, `reg`, `count`, `hz`, `signed` | Starts a periodic read into virtual slot 0..3. |
| `stoppoll` | `slot`                                         | Cancels the poll on that slot.                 |

Each active poll assembles its bytes big-endian into an int32 (signed if
requested) and reports the value in `v[slot]` of every state packet.

## Pin model

- D0 and D1 are reserved for the USB serial UART.
- D2..D13 are user-controlled.
- PWM-capable pins on the Uno family: 3, 5, 6, 9, 10, 11.
- FREQ mode requires hardware interrupt support. On a classic Uno R3, that
  means D2 and D3 only. On the Uno Q most pins have EXTI lines available; the
  firmware checks `digitalPinToInterrupt` and rejects with an error if a pin
  cannot interrupt. The PinScope UI is conservative and only offers FREQ on
  D2 and D3 by default. To enable other pins in the UI, edit
  `INTERRUPT_PINS` near the top of the script in `pinscope.html`.
- A0..A5 are 10-bit by default (1023) and switchable to 12-bit (4095) per
  device from the analog row. The toggle is cosmetic on classic Uno; on Uno Q
  and other 12-bit cores the firmware already returns the full range and the
  UI scales accordingly.

## Virtual channels

The four virtual channels V0..V3 carry derived data. A slot can be configured
two ways, mutually exclusive:

- **I2C poll.** Pick an address in the scanner grid, choose a register, set
  the byte count and poll rate, and start it. The board reads at that rate
  and the value lands in `v[slot]`.
- **Cross-pin math.** Type an expression in the math panel and assign it to a
  slot. Each incoming state packet is fed through the expression and the
  result lands in `v[slot]`.

Math expressions can reference any pin by short name:

- `a0`..`a5` for analog channels
- `d0`..`d13` for digital pins (0 or 1)
- `v0`..`v3` for the four virtual channels (a slot cannot reference itself
  in a useful way; self-reference reads the prior value if any)
- `f0`..`f13` for the frequency in Hz on any FREQ-mode pin

The supported math functions are `abs`, `sqrt`, `min`, `max`, `pow`, `sin`,
`cos`, `tan`, `log`, `log10`, `exp`, `floor`, `ceil`, `round`, `atan2`, and
`sign`. Arithmetic uses the usual `+ - * / %` and parentheses. Anything else
is rejected at compile time.

Useful examples:

```
a0 - a1                             // differential
(a0 + a1) / 2                       // common-mode
sqrt(a0*a0 + a1*a1)                 // magnitude (a0 cos, a1 sin)
a0 * 0.0048828                      // calibrated voltage at 5V/1023
abs(a0 - a1) > 100                  // expressions return numbers; 0 or 1 here
f2                                  // plot D2's measured frequency on V0
```

Virtual channels are first-class everywhere: strip chart, FFT, spectrogram,
scatter, alerts, statistics, and CSV export. Pin labels are clickable to
attach a linear calibration (`y = m*raw + b`) with a label, unit, and decimal
count. Calibrated values then propagate through every view and the CSV.

## Sample rate

Use the Hz selector at the top of each device card to change the firmware's
state-push rate. Options are 5, 10, 20, 30, and 50 Hz. This is the rate the
firmware sends `state` packets, so it sets the time resolution available to
the FFT, the spectrogram, and the strip chart. Faster rates use more USB
serial bandwidth and CPU; 10 Hz is plenty for most maker use, and 50 Hz is
enough for slow control loops or 25 Hz signals at Nyquist.

The selected rate is saved in the session and re-applied on reconnect.

## RGB LED panel

The Board LEDs section drives any three PWM-capable pins from an HTML color
picker. It is useful both for the Uno Q's built-in indicator LEDs (wire the
firmware's `LEDR/LEDG/LEDB` pins, or whichever your board exposes) and for
any common-anode or common-cathode discrete RGB LED on the breadboard.

Picking a color in the swatch:

- sets each chosen pin to PWM mode if it is not already
- writes the matching 0..255 duty cycles for R, G, B
- persists the pin assignment and the most recent color in the session

The OFF button writes zero duty to all three.

## Spectrogram

The Spectrogram tab is a rolling waterfall: it computes a windowed FFT every
250 ms over a 1 s slice of the chosen channel and pushes a vertical column
into the canvas. The X axis is time-into-the-past (right = now), the Y axis
is frequency from DC up to the channel's Nyquist, and intensity is mapped to
the amber gradient. The history window is 30 s, 1 min, 2 min, or 5 min.

The same Hann window toggle used in the FFT view applies here. The PNG
button saves the current waterfall with a caption strip.

If you are looking for a frequency that the FFT view picks out clearly but
the spectrogram does not, raise the firmware sample rate so the analysis
window contains more cycles, or widen the slice (edit `sliceMs` in the
script if you want longer slices at the cost of time resolution).

## MQTT

PinScope speaks MQTT 3.1.1 directly to a broker over WebSocket. There is no
external library; the protocol is implemented inline as a small native codec
covering the packets we need (CONNECT, CONNACK, PUBLISH, SUBSCRIBE, SUBACK,
PINGREQ, PINGRESP, DISCONNECT) at QoS 0.

Topic convention:

    <base>/<deviceId>/out    board -> host  (state, hello, ack, err, i2c)
    <base>/<deviceId>/in     host  -> board (commands)

The base defaults to `pinscope`. Click MQTT, fill in the broker's WebSocket
URL, optionally a base and a specific device-id filter, and connect. The
browser subscribes to `<base>/+/out` and locks onto the first device id it
sees. From then on, all commands route to `<base>/<thatDeviceId>/in`.

Public brokers good for testing: `ws://broker.hivemq.com:8000/mqtt`,
`ws://test.mosquitto.org:8080`. These are world-readable; do not send
anything sensitive over them.

The board itself does not speak MQTT in the reference firmware. To actually
drive a board over MQTT, run `pinscope_mqtt_bridge.py` next to it. The
bridge opens the board's USB serial port, connects to the same broker over
plain TCP MQTT (port 1883 by default), reads the device id from the first
hello packet, then forwards traffic both ways. A future firmware variant
with native MQTT (over WiFi or Ethernet) would skip the bridge entirely.

Example:

    pip install paho-mqtt pyserial
    python3 pinscope_mqtt_bridge.py \
        --port /dev/ttyACM0 --baud 115200 \
        --broker test.mosquitto.org --broker-port 1883 \
        --topic-base pinscope

Then in the browser: MQTT, broker URL `ws://test.mosquitto.org:8080`, base
`pinscope`, leave the filter blank, CONNECT. Within a second or two the
device card lights up.

## BLE

PinScope speaks Bluetooth Low Energy directly to a board running
`pinscope_ble.ino`. The same firmware works on three target boards:

- **Arduino Uno R4 WiFi**, RA4M1 host MCU with an ESP32-S3 BLE coprocessor.
- **Arduino Nano 33 IoT**, SAMD21 host MCU with a NINA-W102 BLE coprocessor.
- **Arduino Uno Q**, STM32U585 MCU with native BLE (experimental: the Uno Q
  runs sketches under Zephyr OS via `arduino:zephyr:unoq`, so ArduinoBLE
  compatibility depends on the core version; verify on your specific build
  before committing).

All three use the ArduinoBLE library, so a single sketch source builds for
any of them. The sketch detects which board it was built for at compile time
and announces itself accordingly in the hello packet.

The committed BLE schema (must match `pinscope.html` and `pinscope_ble.ino`):

    Service     7e2bf001-9d27-4e96-9c9f-1f4b8a0c5e6d   PinScope I/O console
    Notify char 7e2bf002-9d27-4e96-9c9f-1f4b8a0c5e6d   board -> host
    Write char  7e2bf003-9d27-4e96-9c9f-1f4b8a0c5e6d   host  -> board

Both characteristics carry line-delimited JSON. The wire protocol is
identical to Serial, WiFi, and MQTT. Either side chunks at 200 bytes per
ATT write to stay safely under any default MTU on any of the three target
boards. The receiver buffers chunks until it sees `\n` and dispatches one
complete object per line.

Click BLE in the rail (or press `B`) to open the system Bluetooth device
chooser. Picking the entry advertised as `PinScope` opens a GATT
connection, subscribes to the Notify char, and the device card lights up
within a second.

Web Bluetooth is Chromium-only and needs a user gesture (click) to open
the picker the first time. On HTTPS pages, Chrome remembers the device
after the first consent; on `file://` URLs it asks again each session.

To build and flash:

1. Install the ArduinoBLE library (Tools > Manage Libraries > "ArduinoBLE").
2. Select your board: Uno R4 WiFi, Nano 33 IoT, or Uno Q.
3. Open `pinscope_ble.ino`, compile, upload.
4. On power-up the serial monitor prints
   "PinScope BLE advertising as 'PinScope'". The board is now discoverable.

The serial monitor stays available for debugging; the BLE firmware does not
disable USB serial. State packets only stream while a central is connected.

## Multi-board layout

Connect a second board (or load a replay) and a card appears underneath the
first. Click STACK in the top rail to flip to GRID; cards then lay out
two-per-row on screens wider than 1400 px and stack again below that. The
keyboard shortcut `G` toggles the layout from anywhere.

## Replay

Use the CAPTURE button on the strip chart toolbar to download the current
recorder buffer as a `pinscope_capture_<id>_<stamp>.json` file. The file
holds the raw wire-shape samples plus the device id, name, sample rate, and
last-known pin modes; it is not a CSV and is not meant for analysis in
external tools, just for replay.

Click REPLAY in the top rail (or press `R`) and pick a capture file. PinScope
spawns a virtual device that pumps the captured samples through the same
pipeline as a live board, looping when it reaches the end. The pill in the
device card head is purple and labeled `replay` so it never gets confused
with a live unit. Commands sent to a replay device are silently dropped; the
hardware is not there to honor them.

## Raw command sender

Each device card has a single-line JSON input at the bottom of the Wire Log
section. Type any valid command object and hit SEND (or Enter). Useful for
exercising new firmware features without UI controls, or sending things the
UI does not expose yet (custom I2C ops, debug commands you add to your fork
of the firmware, and so on). Arrow up and arrow down walk through the
recent history (last 50).

## Keyboard shortcuts

Press `?` for a full list. Quick reference:

| Key      | Action                                  |
| -------- | --------------------------------------- |
| `?`      | open this help                          |
| `Space`  | pause / resume the active strip chart   |
| `C`      | clear the wire log on the active device |
| `G`      | toggle stack / grid layout              |
| `S`      | open SERIAL connect (Web Serial only)   |
| `W`      | open WIFI connect modal                 |
| `M`      | open MQTT connect modal                 |
| `B`      | open BLE device picker (Web Bluetooth)  |
| `R`      | open a capture file for REPLAY          |
| `1`..`4` | switch tabs on the active device        |
| `Esc`    | close any open modal                    |

The "active device" is whichever card is closest to the top of the viewport.
Shortcuts are suppressed while typing in an input or text area.

## Sessions

Every device card auto-saves its configuration to localStorage 400 ms after
any change. The saved fields are:

- alias, ADC resolution, sample rate
- per-pin calibrations
- active strip chart traces
- threshold alerts
- I2C poll slot configurations
- math expressions
- RGB LED pin assignment and most recent color

The EXPORT button writes the same snapshot to a JSON file. IMPORT reads one
back. The session file format is versioned (current version 1.1, additive
over 1.0); older 1.0 files load fine with the new fields defaulted.

## Frequency measurement notes

The firmware uses RISING-edge interrupts with `attachInterrupt`. There is one
ISR per pin (small wrappers around a per-pin volatile counter). Every 250 ms
the main loop reads and resets the counters with interrupts briefly off, and
converts counts to Hz.

Practical bounds:

- Minimum measurable frequency is roughly 4 Hz (one edge per 250 ms window).
  Below that the readout flickers between 0 and 4 Hz; sample for several
  cycles instead.
- Maximum measurable frequency depends on the core. On a classic 16 MHz Uno
  the ISR overhead is small enough to handle tens of kHz without dropping
  edges, but the JSON serialization will bottleneck the host display long
  before the ISR does. In practice, treat 1 kHz as a comfortable upper
  bound and use a frequency divider for anything faster.
- The reported Hz is averaged over the 250 ms calculation window, so a real
  100 Hz signal reads as a steady 100, but a pulsed signal at one-pulse-per-
  second reads as 4 Hz.

If you need pulse width or duty cycle in addition to frequency, the easiest
path is a math channel that combines two FREQ pins (rising and falling edge
on adjacent pins via a small inverter or a level-trigger circuit). A direct
duty-cycle mode is a candidate for tier 3.

## Browser support

- Chrome, Edge, Opera, Brave, Arc: full feature set including Web Serial
  and Web Bluetooth (BLE).
- Firefox, Safari: WiFi (WebSocket) and MQTT (also WebSocket) transports
  work. Web Serial and Web Bluetooth are not available, so the SERIAL and
  BLE buttons are disabled. Replay works in any browser since it reads a
  local file.
- The canvases use the standard 2D API, devicePixelRatio for crispness on
  Retina, and `requestAnimationFrame` for the draw loops. No WebGL, no
  WebAssembly.

## MQTT firmware variant

If your board has WiFi, you can skip the host-side bridge entirely. The
`pinscope_mqtt.ino` sketch connects to a broker directly using WiFiNINA and
ArduinoMqttClient, advertising itself on `<base>/<deviceId>/out` and
listening for commands on `<base>/<deviceId>/in`. The device id is derived
from the WiFi MAC at boot, so two boards on the same broker never collide.

Tested on:

- **Arduino Uno R4 WiFi**, ESP32-S3 coprocessor exposed via the WiFiNINA
  API shim.
- **Arduino Nano 33 IoT**, NINA-W102 coprocessor.

Edit `WIFI_SSID`, `WIFI_PASS`, `MQTT_HOST`, `MQTT_PORT`, and `TOPIC_BASE`
near the top of the sketch before flashing. The broker is plain TCP MQTT
(default port 1883), not the WebSocket port. The PinScope browser console
still uses the broker's WebSocket port (typically 8080 or 8000) to talk to
the same broker; the broker routes between the two.

## Continuous integration

The repo ships with a GitHub Actions workflow at `.github/workflows/ci.yml`
that runs on every push and pull request, plus on demand via
`workflow_dispatch`. It has four jobs:

| Job              | What it does                                                                                                                                                                                                       |
| ---------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `lint`           | Extracts the JS from `pinscope.html`, runs JSHint with `.jshintrc`, then checks formatting with Prettier and HTML validity with html-validate. Also fails the build if any em dash slips into the tracked sources. |
| `compile-serial` | arduino-cli compile matrix for `pinscope.ino` on Uno R3, Nano 33 IoT, and Uno R4 WiFi. The serial firmware compiles on every classic Arduino core, so this is the broadest sanity check.                           |
| `compile-ble`    | Compile matrix for `pinscope_ble.ino` on Nano 33 IoT and Uno R4 WiFi (the two boards with first-class ArduinoBLE support). Uno Q is omitted from CI pending Zephyr-core verification.                              |
| `compile-mqtt`   | Compile matrix for `pinscope_mqtt.ino` on the two NINA boards, with `WiFiNINA` and `ArduinoMqttClient` pulled in by the workflow.                                                                                  |
| `bridge-syntax`  | Compiles the Python bridge with `py_compile` and runs `ruff` for style.                                                                                                                                            |

The workflow uses the official `arduino/compile-sketches@v1` action and the
matrix-job pattern documented in Arduino's "GitHub Actions for Arduino"
guide; staging each sketch into a folder of the same name keeps arduino-cli
happy with the flat repo layout.

If you fork this and want to add a board, drop another entry in the
relevant matrix and (if needed) install its core via the `platforms` key.

## Files

- `pinscope.html` is the entire console: HTML, CSS, JavaScript, all in one
  file. Around 165 KB and roughly 4200 lines. Open it directly from disk
  or serve it; both work.
- `pinscope.ino` is the reference firmware for USB serial. Around 550 lines.
- `pinscope_ble.ino` is the BLE-capable firmware variant. One source for
  Uno R4 WiFi, Nano 33 IoT, and (experimentally) Uno Q. Around 620 lines.
- `pinscope_mqtt.ino` is the MQTT-capable firmware variant for Uno R4 WiFi
  and Nano 33 IoT. Around 640 lines.
- `pinscope_mqtt_bridge.py` is a small serial-to-MQTT bridge for boards
  that lack WiFi. Needs `paho-mqtt` and `pyserial`.
- `.github/workflows/ci.yml` is the CI workflow (compile matrix + lint).
- `.jshintrc`, `.prettierrc`, `.prettierignore` are the lint configs.
- `README.md` is this file.

## License

PinScope (Field Instrument 005) is free software: you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation, either version 3 of the License,
or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see https://www.gnu.org/licenses/gpl-3.0.html

SPDX-License-Identifier: GPL-3.0-or-later

Copyright (C) 2025-2026 Michael B. Parks · Green Shoe Garage

## Repo layout

```
pinscope/
├── pinscope.html              # the entire console
├── pinscope.ino               # serial firmware (any classic Arduino)
├── pinscope_ble.ino           # BLE firmware (Nano 33 IoT, Uno R4 WiFi, Uno Q experimental)
├── pinscope_mqtt.ino          # MQTT firmware (Uno R4 WiFi, Nano 33 IoT)
├── pinscope_mqtt_bridge.py    # serial-to-MQTT bridge for boards without WiFi
├── scripts/
│   ├── lint.sh                # run the same checks CI runs
│   └── compile.sh             # compile a sketch against a chosen FQBN
├── .github/workflows/ci.yml   # GitHub Actions: compile matrix + lint
├── .jshintrc                  # lint config for the embedded JS
├── .prettierrc                # formatter config
├── .prettierignore            # paths Prettier skips
├── .gitignore
├── LICENSE                    # GPL-3.0-or-later
├── CONTRIBUTING.md
└── README.md
```

## First-time setup (fresh clone)

```sh
git clone https://github.com/mbparks/pinscope.git
cd pinscope

# Open the console directly in a browser
open pinscope.html       # macOS
xdg-open pinscope.html   # Linux
start pinscope.html      # Windows
```

There is no install step for the browser side. The Arduino firmwares need
arduino-cli or the Arduino IDE 2.x, with the matching board cores and the
ArduinoBLE, WiFiNINA, and ArduinoMqttClient libraries installed for the
variants you want to use. See the per-section instructions below.

## Publishing this repo

If you forked or are reading this in a fresh checkout, here is the
exact sequence to take it from local files to a live GitHub repo:

```sh
cd pinscope
git init
git add .
git commit -m "initial commit: PinScope (Field Instrument 005)"
git branch -M main
git remote add origin https://github.com/mbparks/pinscope.git
git push -u origin main
```

The CI workflow runs on the first push and on every subsequent push and
pull request. The badge at the top of this README will turn green within
a few minutes of the first successful run.
