# PinScope · Field Instrument 005

[![CI](https://github.com/mbparks/pinscope/actions/workflows/ci.yml/badge.svg)](https://github.com/mbparks/pinscope/actions/workflows/ci.yml)
[![Docs](https://github.com/mbparks/pinscope/actions/workflows/pages.yml/badge.svg)](https://mbparks.github.io/pinscope/)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/License-GPL--3.0--or--later-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Single-file](https://img.shields.io/badge/runtime-single--file%20HTML-d4a017)](pinscope.html)

**Docs site:** [mbparks.github.io/pinscope](https://mbparks.github.io/pinscope/)

Single-file HTML console for the Arduino Uno Q (and any classic Uno-compatible
board) over USB serial, WiFi, MQTT, or BLE. Drop the page into a browser,
connect to a running firmware, and you get:

- a live map of every digital and analog pin, click-to-toggle outputs, sliders
  for PWM, an interrupt-counting FREQ mode for pulse and tachometer signals
- a strip chart with per-trace stats, calibration in real units, threshold
  alerts, oscilloscope-style trigger, baseline + diff view, CSV one-shot and
  streaming export, PNG snapshots
- FFT, waterfall spectrogram, and XY scatter views for any analog or virtual
  channel
- an I2C / Qwiic scanner with four poll slots that feed virtual channels, a
  companion sensor library of one-click presets for common breakouts
- a cross-pin math engine: derive a virtual channel from any other channels
  (e.g. `v0 = sqrt(a0*a0 + a1*a1)`)
- a scripted automation sandbox for repeatable test sequences
- an RGB LED panel that drives any three PWM pins from a color picker
- a sandboxed plugin system so users can ship their own panels per device
- per-device sessions that auto-save to localStorage, plus JSON export and
  import (calibrations, alerts, trigger, math, plugin state, all of it)

No build step. No framework. No external assets at runtime once loaded.

The visual language is the same amber-on-dark "field instrument" treatment
used in the rest of the M.B. Parks tool series.

## Table of contents

- [Quick start](#quick-start)
- [Hardware bring-up](#hardware-bring-up) (see `BRINGUP.md` for the full walkthrough)
- [Files in this repo](#files-in-this-repo)
- [Browser support](#browser-support)
- [Repo layout](#repo-layout)
- [Publishing this repo](#publishing-this-repo)

**Console reference:**

- [Pin model](#pin-model)
- [Sample rate](#sample-rate)
- [Keyboard shortcuts](#keyboard-shortcuts)
- [Multi-board layout](#multi-board-layout)
- [Raw command sender](#raw-command-sender)

**Channels and analysis:**

- [Calibration](#calibration)
- [Virtual channels](#virtual-channels)
- [Cross-pin math](#cross-pin-math)
- [Strip chart](#strip-chart)
- [Baseline + diff view](#baseline--diff-view)
- [Oscilloscope-style trigger](#oscilloscope-style-trigger)
- [FFT and spectrogram](#fft-and-spectrogram)
- [XY scatter](#xy-scatter)
- [Frequency measurement](#frequency-measurement)
- [Threshold alerts](#threshold-alerts)

**I/O and protocols:**

- [I2C / Qwiic](#i2c--qwiic)
- [Companion sensor library](#companion-sensor-library)
- [RGB LED panel](#rgb-led-panel)
- [Wire protocol](#wire-protocol)

**Automation:**

- [Scripted automation](#scripted-automation)
- [CSV export](#csv-export)
- [Replay](#replay)
- [Plugins](#plugins)

**Connectivity:**

- [USB serial firmware](#usb-serial-firmware)
- [WiFi firmware (raw socket)](#wifi-firmware-raw-socket)
- [MQTT](#mqtt)
- [BLE](#ble)
- [Session diff](#session-diff)

**Project:**

- [Sessions](#sessions)
- [Continuous integration](#continuous-integration)
- [License](#license)

## Quick start

```sh
git clone https://github.com/mbparks/pinscope.git
cd pinscope
open pinscope.html       # macOS
xdg-open pinscope.html   # Linux
start pinscope.html      # Windows
```

There is no install step for the browser side. The Arduino firmwares need
arduino-cli or the Arduino IDE 2.x, with the matching board cores and the
ArduinoBLE, WiFiNINA, and ArduinoMqttClient libraries installed for the
variants you want to use. See the per-section instructions below.

## Hardware bring-up

`BRINGUP.md` walks through bringing PinScope up on real hardware
phase-by-phase, with explicit pass criteria and known failure modes. It's
tailored to the Arduino Uno Q on macOS but the general flow applies to
any supported board. Start there before flashing if it's your first time.

## Files in this repo

| File                       | What it is                                                                                               |
| -------------------------- | -------------------------------------------------------------------------------------------------------- |
| `pinscope.html`            | The entire browser console. ~5950 lines, ~180 KB.                                                        |
| `pinscope.ino`             | Serial firmware for the host MCU. Works on classic Uno, Uno R4, Nano 33 IoT, and (experimentally) Uno Q. |
| `pinscope_ble.ino`         | BLE firmware for boards with ArduinoBLE support.                                                         |
| `pinscope_mqtt.ino`        | MQTT firmware for boards with WiFi (Uno R4 WiFi, Nano 33 IoT).                                           |
| `pinscope_mqtt_bridge.py`  | Serial-to-MQTT bridge for boards without WiFi.                                                           |
| `plugins/`                 | Example plugins (hello, gauge, servo-sweep, field-notes).                                                |
| `scripts/lint.sh`          | Same lint pipeline CI runs; supports `--fix`.                                                            |
| `scripts/compile.sh`       | Stage and compile a sketch against a chosen FQBN.                                                        |
| `.github/workflows/ci.yml` | GitHub Actions: lint + arduino-cli compile matrix.                                                       |
| `BRINGUP.md`               | 11-phase hardware bring-up walkthrough.                                                                  |
| `CONTRIBUTING.md`          | Style rules and PR expectations.                                                                         |

## Browser support

Web Serial (USB direct connect) needs Chromium-based browsers (Chrome, Edge,
Brave, Arc, Opera). Web Bluetooth (BLE) is also Chromium-only. WebSocket
(WiFi raw and MQTT-over-WS) works in every modern browser. CSV streaming
needs the File System Access API and is Chromium-only; in Firefox and
Safari the STREAM button is disabled with a tooltip explaining why.

The page itself is plain HTML5 and runs anywhere; the transport buttons
gray out if a browser doesn't support the underlying API.

## Repo layout

```
pinscope/
├── pinscope.html              # the entire console
├── pinscope.ino               # serial firmware
├── pinscope_ble.ino           # BLE firmware
├── pinscope_mqtt.ino          # MQTT firmware (Uno R4 WiFi, Nano 33 IoT)
├── pinscope_mqtt_bridge.py    # serial-to-MQTT bridge for boards without WiFi
├── plugins/
│   ├── hello.js               # minimal plugin (A0 + bar visualization)
│   ├── gauge.js               # SVG arc gauge with persisted pin selection
│   ├── servo-sweep.js         # triangle-wave PWM driver
│   └── field-notes.js         # notebook + timestamped moments (persist demo)
├── scripts/
│   ├── lint.sh                # run the same checks CI runs
│   └── compile.sh             # compile a sketch against a chosen FQBN
├── .github/workflows/ci.yml   # GitHub Actions
├── .jshintrc, .prettierrc, .prettierignore
├── .gitignore
├── BRINGUP.md
├── CONTRIBUTING.md
├── LICENSE                    # GPL-3.0-or-later
└── README.md
```

## Publishing this repo

```sh
cd pinscope
git init
git add .
git commit -m "initial commit: PinScope (Field Instrument 005)"
git branch -M main
git remote add origin https://github.com/mbparks/pinscope.git
git push -u origin main
```

CI runs on the first push.

---

## Pin model

Every device card maps the host MCU's pins to a uniform model:

- **D0-D13**, digital pins. Modes: `off`, `in` (high-Z input), `inp`
  (input with pull-up), `out`, `pwm` (only on PWM-capable pins, marked
  with a ~ in the UI), `freq` (only on interrupt-capable pins). Click
  the mode badge to cycle.
- **A0-A5**, analog inputs. 10-bit by default; toggle to 12-bit if the
  board's core supports `analogReadResolution(12)`.
- **V0-V3**, virtual channels. Filled in by I2C polling or cross-pin
  math expressions. Treated identically to A0-A5 by every visualization,
  alert, and CSV export.
- **F0-F13**, frequency channels. Populated by pins in FREQ mode.

D0 and D1 are reserved (USB serial uses them) and grayed out.

## Sample rate

The Hz selector on each device card sets how often the firmware sends
state packets. Range is 1 to 50 Hz. The default is 10 Hz. Higher rates
give smoother strip charts and finer FFT resolution but consume more
serial bandwidth.

Inside the firmware, the state push period is `1000 / hz` milliseconds.
Lower the rate if the wire log shows tx pressure or if you're on a slow
transport like BLE; raise it for fast transients.

## Keyboard shortcuts

| Key     | Action                                                             |
| ------- | ------------------------------------------------------------------ |
| `S`     | open the SERIAL connect dialog                                     |
| `W`     | open the WIFI connect dialog                                       |
| `M`     | open the MQTT connect dialog                                       |
| `B`     | open the BLE connect dialog                                        |
| `R`     | open a capture file (REPLAY)                                       |
| `P`     | open the plugin manager                                            |
| `G`     | toggle stacked vs grid layout when multiple devices are connected  |
| `?`     | open the help modal                                                |
| `1`-`4` | switch tabs (Strip / FFT / Spectro / Scatter) on the active device |
| `Space` | pause / resume the strip chart on the active device                |
| `C`     | clear the wire log on the active device                            |
| `Esc`   | close any open modal                                               |

Shortcuts are suppressed while typing in inputs or textareas.

## Multi-board layout

PinScope handles multiple connected boards in the same window. The STACK
button on the rail toggles between a single-column stack (one card on top
of another) and a side-by-side grid (cards laid out in columns). Three
or more connected boards still get the grid layout; STACK forces single
column if you prefer that for note-taking or screen recording. Press `G`
to toggle.

## Raw command sender

Every device card has a raw command input at the bottom for sending wire-
protocol commands directly. Type a JSON object (e.g. `{"cmd":"mode","pin":4,"mode":"out"}`),
hit Enter or SEND. The Up/Down arrow keys cycle through your command
history (50 entries deep per session). Useful for debugging firmware or
testing fields the UI doesn't expose.

---

## Calibration

Click any A0-A5 (or V0-V3) label on a device card to open the calibration
modal. Two modes:

**Manual** (the default). Type a slope `m`, offset `b`, unit, decimals,
and a label. Linear conversion `y = m·raw + b` is applied to every
reading on that pin and propagates through the strip chart, FFT,
spectrogram, scatter, stats, alerts, and CSV export.

**Wizard**, for when you have a physical reference. Apply a known value
at the low end (often a short to ground), click CAPTURE LOW. Apply a
known value at the high end, click CAPTURE HIGH. PinScope computes the
slope and offset from the two-point fit, shows a live preview using the
current raw reading, then SAVE writes the calibration to the pin.

The wizard does the algebra (`m = (hi - lo) / (rawHi - rawLo)`,
`b = lo - m·rawLo`) so you don't have to. If the two raw captures are
identical, the wizard refuses to fit and tells you so. Calibrations
persist in the session and export with the JSON.

## Virtual channels

PinScope has four virtual analog slots (V0-V3) that show up in every
visualization next to A0-A5. Two sources can feed a virtual slot:

- **I2C polling**: configure a slot to poll an I2C register at a fixed
  Hz; the latest read goes into the virtual channel. The companion
  sensor library has one-click presets for common breakouts.
- **Cross-pin math**: assign an expression like `v0 = sqrt(a0*a0 + a1*a1)`
  to a slot; the expression is recomputed every state packet against the
  latest readings.

Math overrides I2C if both target the same slot. Slot configuration
persists in the session.

## Cross-pin math

The math engine lets a virtual channel be a function of any other
channels. The expression syntax is a small JS subset: arithmetic, the
math globals (`sin`, `cos`, `sqrt`, `log`, `exp`, `abs`, `pow`, `min`,
`max`, `floor`, `ceil`, `round`), and references to channels by their
key (`a0` to `a5`, `d0` to `d13`, `v0` to `v3`, `f0` to `f13`).

Examples:

| Expression                        | What it derives                    |
| --------------------------------- | ---------------------------------- |
| `(a0 + a1) / 2`                   | average of two analog inputs       |
| `sqrt(a0*a0 + a1*a1)`             | magnitude of an XY pair            |
| `a0 - a1`                         | differential measurement           |
| `v0 * 0.0078125`                  | TMP117 raw to degrees Celsius      |
| `floor(v0 / 16) * 10 + (v0 % 16)` | BCD-unpacked seconds from a DS3231 |

Expressions are compiled via `new Function(...)` with strict mode and
no access to globals beyond the math helpers. Bad syntax surfaces in a
toast; null/undefined inputs produce null outputs (the math doesn't
crash on a momentarily-missing channel).

## Strip chart

The strip chart shows up to 14 channels at once. Click any chip in the
ANALOG / DIGITAL / VIRTUAL rows under the chart to toggle that channel
on. Click again to remove it. Each trace gets a deterministic color
keyed off its name.

The toolbar:

| Button                | Action                                                                           |
| --------------------- | -------------------------------------------------------------------------------- |
| `15s` `1m` `5m` `15m` | window length                                                                    |
| `STATS`               | toggle per-trace stats overlay (min, max, mean, std dev)                         |
| `PAUSE`               | freeze the chart and recorder                                                    |
| `CLEAR`               | drop the recorder buffer                                                         |
| `CSV`                 | one-shot dump of the buffer to a CSV file                                        |
| `STREAM`              | open a save-file picker and append every new sample to that file (Chromium only) |
| `CAPTURE`             | save the buffer as a replayable JSON file                                        |
| `BASELINE`            | load a capture file as a faded ghost trace overlay                               |
| `DIFF`                | toggle a live-minus-baseline trace on top of the chart                           |
| `TRIG`                | open the oscilloscope-style trigger modal                                        |
| `PNG`                 | export the current chart as a PNG snapshot                                       |

The recorder buffer caps at 9000 samples (about 15 minutes at 10 Hz,
3 minutes at 50 Hz). For longer runs use STREAM.

## Baseline + diff view

The strip chart can overlay a previously captured run as a faded dashed
ghost trace and render a live-minus-baseline diff trace on top. Useful
for before-vs-after comparisons: tweak a circuit, flash new firmware,
swap a sensor, see exactly how the new behavior differs from the saved
one.

1. CAPTURE a baseline run.
2. BASELINE loads a capture file as the baseline. Click again to clear.
3. DIFF toggles a difference trace, computed as `live - baseline` for
   each active channel, normalized across all active diffs, centered on
   the chart's midline.

The baseline is aligned to "now" by tail-shifting its timestamps, so a
capture from yesterday lines up with the current strip-chart window
without any clock-sync gymnastics. Diff is nearest-neighbor by time,
good enough at typical sample rates.

## Oscilloscope-style trigger

The strip chart can capture a fixed window of samples around a threshold
crossing and freeze on the event for inspection. Useful for one-shot
events that are hard to spot in the live stream: a glitch, a power
transient, a single-shot pulse from a sensor.

Click **TRIG** on the chart toolbar, pick a channel (any A, V, F, or D
pin), pick a condition (rising, falling, above, below), set a threshold
in raw units, and define pre and post sample windows in seconds. ARM
the trigger. The button starts pulsing red as long as the trigger is
armed.

When the condition fires, the chart freezes onto the captured window
with a red vertical line at the fire time and a red horizontal line at
the threshold. The recorder auto-pauses so the snapshot stays still.
RE-ARM resets the trigger and resumes recording; DISARM clears it
entirely. The trigger configuration persists in the session and
re-arms automatically on the next connect.

## FFT and spectrogram

The FFT tab shows the magnitude spectrum of the active analog or virtual
channel. The window length comes from the strip chart selector, the FFT
size is computed from the available samples. Window functions (Hann,
Hamming, Blackman, none) are selectable. Magnitude is plotted on a log
y-axis by default; toggle to linear.

The Spectrogram tab is a rolling waterfall of the same FFT over time.
Useful for non-stationary signals where the frequency content shifts.
The colormap is fixed (amber-on-dark to match the rest of the UI). One
slice is computed per state packet at the current sample rate.

## XY scatter

The Scatter tab plots two channels against each other for
Lissajous-style visualization. Pick X and Y from the dropdowns. Optional
connecting lines link adjacent samples in time, so you can see the
trajectory and not just the cloud. Useful for phase analysis, sensor
correlation, or just looking at a circuit's transfer function.

## Frequency measurement

Pins in FREQ mode count rising edges via `attachInterrupt`. The firmware
has one ISR per pin (small wrappers around a per-pin volatile counter).
Every 250 ms the main loop reads and resets the counters with
interrupts briefly off, and converts counts to Hz.

Practical bounds:

- Minimum measurable: roughly 4 Hz (one edge per 250 ms window). Below
  that the readout flickers between 0 and 4 Hz; sample for several
  seconds to average.
- Maximum measurable: depends on the host MCU. On a classic Uno R3
  (16 MHz) the ISR overhead caps out around 100-200 kHz before edges
  are missed. On an Uno R4 WiFi or Nano 33 IoT, higher.

Only pins with hardware interrupt support can be put in FREQ mode. On
the classic Uno R3 that's D2 and D3 only; on the Uno R4 WiFi and Nano
33 IoT, all digital pins. The firmware returns "no interrupt on pin" if
you try to set FREQ on a non-interrupt pin.

## Threshold alerts

Each device card has a Threshold Alerts panel for raising an alert when
a channel crosses a value. Pick a pin, pick a condition (above, below,
rising, falling, equal), a value, and optionally a sound. Alerts fire
when the condition transitions from false to true (not held continuously).

Alerts persist in the session. Fired alerts show a toast and can play
a short beep through Web Audio. Useful for hands-off monitoring while
you do something else.

---

## I2C / Qwiic

Every device card has an I2C / Qwiic section with two parts:

**Scanner**, SCAN BUS sends an I2C scan and highlights every responding
address on a 16×8 grid. Click a highlighted cell to populate the read
tool with that address.

**Read/write tool**, three buttons:

- **READ**, read N bytes from a register at the selected address.
- **WRITE**, write N bytes to a register.
- **POLL**, configure a virtual slot to poll a register at a fixed Hz
  (1-50 Hz). The latest read feeds the virtual channel.

The firmware caps reads at 8 bytes per request to keep the wire packet
small. Signed reads are supported (the firmware sign-extends per the
byte count). Polls are paused while a one-shot read or write runs, then
resume.

## Companion sensor library

The I2C panel ships with one-click presets for common breakout sensors.
Pick a sensor from the COMPANION SENSORS dropdown, confirm the address,
hit APPLY. PinScope assigns free virtual slots to that sensor's
registers and, when a unit-conversion math expression makes sense,
prefills the math input below so you can apply it to a free slot with
one click.

| Preset    | What it reads                           | Notes                                    |
| --------- | --------------------------------------- | ---------------------------------------- |
| TMP102    | 12-bit temperature, 0.0625 °C/LSB       | math: `(v0 / 256) * 0.0625`              |
| TMP117    | precision temperature, 0.0078125 °C/LSB | math: `v0 * 0.0078125`                   |
| DS3231    | RTC seconds counter, BCD                | math: `floor(v0/16)*10 + (v0%16)`        |
| MCP9808   | 13-bit signed temperature               | math masks alert bits                    |
| INA219    | shunt voltage (mV) and bus voltage (V)  | two-slot preset                          |
| APDS-9301 | ambient light channel 0                 | requires power-on write to register 0xC0 |
| PCF8591   | ADC channel 0                           | single byte, default channel             |

The preset definitions live in `SENSOR_PRESETS` near the top of the
I2CPanel code in `pinscope.html`. Adding a sensor is one record:
default address, register list, byte count, signed flag, optional unit
conversion math expression. PinScope's I2C scope is read-only polling,
so sensors that need a wake/config write are flagged with a
`requiresInit` hint rather than auto-initialized.

## RGB LED panel

Three dropdowns (R, G, B) select PWM pins, a color picker picks a
color, APPLY puts each pin in PWM mode and writes the matching duty.
OFF stops driving. Useful for common-anode/cathode RGB LEDs or any
three-channel PWM device. The pin assignment and most recent color
persist in the session.

## Wire protocol

PinScope speaks a small JSON line protocol over every transport. One
JSON object per line (newline-terminated). The transport doesn't care
about the framing beyond newline.

### Host to board

| Command                                                                              | Fields                                               | Notes                            |
| ------------------------------------------------------------------------------------ | ---------------------------------------------------- | -------------------------------- |
| `{"cmd":"hello"}`                                                                    |                                                      | request a hello packet           |
| `{"cmd":"poll"}`                                                                     |                                                      | request a state packet on demand |
| `{"cmd":"mode","pin":N,"mode":M}`                                                    | M is one of `off`, `in`, `inp`, `out`, `pwm`, `freq` |                                  |
| `{"cmd":"set","pin":N,"val":V}`                                                      | V is 0 or 1                                          | only valid in `out` mode         |
| `{"cmd":"pwm","pin":N,"val":V}`                                                      | V is 0-255                                           | only valid in `pwm` mode         |
| `{"cmd":"hz","val":H}`                                                               | H is 1-50                                            | sample rate                      |
| `{"cmd":"i2c","op":"scan"}`                                                          |                                                      | scan the bus                     |
| `{"cmd":"i2c","op":"read","addr":A,"reg":R,"count":N}`                               |                                                      | one-shot read                    |
| `{"cmd":"i2c","op":"write","addr":A,"reg":R,"data":[bytes]}`                         |                                                      | one-shot write                   |
| `{"cmd":"i2c","op":"poll","slot":S,"addr":A,"reg":R,"count":N,"hz":H,"signed":bool}` |                                                      | configure a polling slot         |
| `{"cmd":"i2c","op":"stoppoll","slot":S}`                                             |                                                      | stop a polling slot              |

### Board to host

| Packet                                                            | Fields | Notes                                                                                    |
| ----------------------------------------------------------------- | ------ | ---------------------------------------------------------------------------------------- |
| `{"t":"hello","id":"...","name":"...","hz":H}`                    |        | sent on connect and on `hello`                                                           |
| `{"t":"state","d":[...],"a":[...],"m":[...],"v":[...],"f":[...]}` |        | periodic; `d` is digital readings, `a` analog, `m` pin modes, `v` virtual, `f` frequency |
| `{"t":"i2c","op":"scan","addrs":[...]}`                           |        | scan result                                                                              |
| `{"t":"i2c","op":"read","addr":A,"reg":R,"data":[bytes]}`         |        | read result                                                                              |
| `{"t":"ack","cmd":"..."}`                                         |        | command accepted                                                                         |
| `{"t":"err","msg":"..."}`                                         |        | command rejected                                                                         |

The protocol is intentionally trivial: any embedded developer can speak
it from a fresh sketch in an hour. PinScope itself parses with
`JSON.parse` per line on the browser side. Firmware uses a tiny
hand-rolled field scanner so it doesn't pull a JSON library.

---

## Scripted automation

Every device card has a SCRIPTED AUTOMATION section with a JS sandbox
for driving the board over time. The available helpers:

| Helper                | What it does                                              |
| --------------------- | --------------------------------------------------------- |
| `setMode(pin, mode)`  | change a pin's mode (`'out'`, `'in'`, `'pwm'`, etc.)      |
| `setDigital(pin, hi)` | drive a digital output                                    |
| `setPWM(pin, val)`    | set PWM duty (0..255)                                     |
| `await wait(ms)`      | sleep                                                     |
| `read(pin)`           | latest raw value for any pin key (`'a0'`, `'d2'`, `'v1'`) |
| `log(msg)`            | print to the script's output panel                        |
| `assert(cond, msg)`   | throw if `cond` is falsy                                  |

JS loops, conditionals, variable declarations, and `await` all work.
Network and storage globals (`fetch`, `localStorage`, `eval`, `Function`,
`setTimeout`, `window`, `document`, etc.) are blocked at compile time
via a strict identifier blocklist.

The RUN button starts the script; STOP aborts it (the helpers check an
aborted flag before resuming). Script content auto-saves to the session
so a refresh keeps your work.

Example: ramp PWM duty and log analog readings.

```js
setMode(9, 'pwm');
for (let duty = 0; duty <= 255; duty += 32) {
  setPWM(9, duty);
  await wait(200);
  log('duty=' + duty + ' a0=' + read('a0'));
}
```

## CSV export

The CSV button on the strip chart toolbar does a one-shot dump of the
recorder buffer to a CSV file. Columns: timestamp_ms, ISO 8601 time,
every digital pin, every analog raw plus its calibrated value if a
calibration is set, every virtual raw plus calibrated.

For longer runs, the STREAM button opens a save-file picker (File
System Access API), then appends every new sample to that file in
batches of up to 50 rows. Click STREAM again to stop and close the
file. The streamed CSV uses the same column layout. Calibrations
applied mid-stream affect subsequent rows.

CSV streaming is Chromium-only. In Firefox and Safari the STREAM button
is disabled with a tooltip explaining why; fall back to the one-shot
CSV or to a capture file.

## Replay

The REPLAY button on the rail loads a capture JSON file as a synthetic
"device" card. The card shows up labeled `[replay]` and plays the
captured samples back in real time. Useful for sharing a problematic
recording with a colleague, or for reviewing a run without having to
recreate the hardware setup.

Capture files are produced by the CAPTURE button on the strip chart and
include the full sample buffer plus enough device metadata (id, name,
hz, modes) to fake the wire protocol. The replay loops by default;
toggle to one-shot if you want it to stop at the end.

## Plugins

PinScope supports user-loaded extensions. Each plugin gets a panel on
every device card and runs inside a sandboxed iframe with no network,
no DOM access outside its own body, no `localStorage` of its own.
Communication with the host happens through `postMessage` over a
per-instance bridge id.

Open the manager with the PLUGINS button on the rail, or press P. Paste
plugin source or load it from a file, give it a name, click LOAD.
Loaded plugins persist in `localStorage` under the key
`pinscope.plugins.v1` and re-mount automatically the next time the
page opens. The checkbox next to each loaded plugin toggles enable;
REMOVE deletes it entirely.

### Plugin API

Inside the sandbox iframe, plugins call `PinScope.register({...})` exactly
once with these fields:

- `id`, unique alphanumeric identifier (`[a-zA-Z][\w-]{0,40}`)
- `name`, human label shown in the panel head
- `version`, any string
- `onInit(api)`, called once after registration
- `onState(state, api)`, called for every state packet
- `onDestroy()`, called when the card disconnects or the plugin is disabled

The `state` argument matches the wire protocol packet:
`{ d: [14], a: [6], v: [4], f: [14], m: [14], t: timestamp }`.

The `api` object provides:

- `api.send(obj)`, send a wire command (subject to the same validation
  the rest of PinScope uses)
- `api.getState()`, Promise resolving to a state snapshot on demand
- `api.log(msg)`, prints to the host's developer console
- `api.render(html)`, replaces the plugin panel body with HTML (script
  tags stripped, defense in depth)
- `api.renderSVG(svgText)`, same but without script stripping, for
  trusted SVG content
- `api.persist(key, value)`, save plugin state; round-trips through
  session export/import
- `api.recall(key)`, Promise resolving to a previously persisted value

### Persistence round-trip

Anything a plugin writes through `api.persist` lands in
`device.pluginState[plugin.id][key]` on the host and is part of the
session snapshot. Plugin state survives:

- page reload (via localStorage autosave, 400 ms debounce)
- session EXPORT to JSON file
- session IMPORT from JSON file (plugins are force-remounted so
  `onInit` re-reads via `api.recall()`)

Plugins should always read persisted state in `onInit`, not cache it
from a previous lifecycle.

### Example plugins

The repo ships with four plugins under `plugins/`:

- `hello.js`, the simplest possible plugin. Prints A0 with a bar
  visualization. Useful as a template.
- `gauge.js`, an SVG arc gauge for any analog pin. Demonstrates
  `renderSVG`, `persist`/`recall` for the selected pin, and a button
  group rendered into the plugin's own DOM.
- `servo-sweep.js`, drives a PWM pin in a triangle wave with start and
  stop controls. Demonstrates `api.send` to issue wire-protocol
  commands and a small UI with selectable PWM pin.
- `field-notes.js`, a persistent notebook attached to the device card.
  Free-form notes textarea plus a list of timestamped "moments" that
  snapshot the current state with optional labels. Demonstrates the
  full `persist`/`recall` round-trip for both simple and complex value
  types.

### Security model

The iframe sandbox is `sandbox="allow-scripts"` only. Lack of
`allow-same-origin` is deliberate and blocks the iframe from reaching
the parent document, navigating the top frame, or making same-origin
network requests. The bridge id is randomized per instance, so a
malicious plugin can't impersonate another plugin's messages.

User-supplied plugin source is embedded into the iframe via
`JSON.stringify` and run with `eval` inside the sandbox, which prevents
template-literal breakout attacks on the host. The closing `script>`
tag in the srcdoc is split across two string concatenations so the
host HTML parser doesn't terminate the outer script block early.

What plugins cannot do, by design: see other plugins' iframes, read the
host's localStorage, access `fetch`, modify other device cards, override
builtin PinScope behavior. Wire commands route through the same
`device.send()` path as user actions, so any abuse appears in the
Wire Log and can be paused or audited.

---

## USB serial firmware

The default firmware is `pinscope.ino`. Flash it via the Arduino IDE
or `arduino-cli`. It works on:

- classic Arduino Uno R3 (and any compatible 16 MHz AVR)
- Arduino Nano 33 IoT
- Arduino Uno R4 WiFi
- Arduino Uno Q (experimental, see `BRINGUP.md`; runs under
  `arduino:zephyr:unoq` which behaves differently from classic Arduino
  in subtle ways)

Open the SERIAL connect dialog in PinScope, pick the matching
`/dev/cu.usbmodem*` (macOS) or `COMx` (Windows) or `/dev/ttyACM*`
(Linux), and the card appears.

## WiFi firmware (raw socket)

There isn't a dedicated raw-socket WiFi firmware in the repo; the WIFI
button is intended for boards running a custom firmware that exposes
the wire protocol on a TCP socket via a WebSocket server. The simplest
real-world path is to use the MQTT firmware below (broker handles all
the framing) or the serial firmware with the MQTT bridge.

## MQTT

`pinscope_mqtt.ino` connects to a broker directly over WiFi using the
WiFiNINA + ArduinoMqttClient libraries. Tested on:

- **Arduino Uno R4 WiFi**, ESP32-S3 coprocessor exposed via the
  WiFiNINA API shim
- **Arduino Nano 33 IoT**, NINA-W102 coprocessor

Edit `WIFI_SSID`, `WIFI_PASS`, `MQTT_HOST`, `MQTT_PORT`, and
`TOPIC_BASE` at the top of the sketch before flashing. The board
derives its device id from the WiFi MAC at boot, so two boards on the
same broker never collide.

Topic schema:

- `<base>/<deviceId>/out`, board to host (line-delimited JSON)
- `<base>/<deviceId>/in`, host to board (one command per PUBLISH)

The PinScope browser console connects to the broker's WebSocket port
(typically 8080 or 8083), not the raw TCP port. The broker routes
between the WS client and the TCP-only firmware.

### Serial-to-MQTT bridge

For boards without WiFi, `pinscope_mqtt_bridge.py` is a small Python
script that reads serial frames from the board and republishes them
to MQTT, and forwards MQTT commands back to serial. Requires
`paho-mqtt` and `pyserial`:

```sh
pip install paho-mqtt pyserial
python pinscope_mqtt_bridge.py --port /dev/cu.usbmodem1101 --broker test.mosquitto.org
```

The bridge sniffs the board's `hello` packet to learn its device id and
constructs the topic names automatically.

## BLE

`pinscope_ble.ino` is the BLE-capable firmware variant for boards with
ArduinoBLE support:

- Arduino Uno R4 WiFi (ESP32-S3 BLE coprocessor)
- Arduino Nano 33 IoT (NINA-W102 BLE coprocessor)
- Arduino Uno Q (experimental; runs under Zephyr OS so ArduinoBLE
  behavior depends on the core version, verify on your build)

PinScope uses the following service and characteristic UUIDs:

| Item                  | UUID                                   |
| --------------------- | -------------------------------------- |
| Service               | `7e2bf001-9d27-4e96-9c9f-1f4b8a0c5e6d` |
| Notify (board → host) | `7e2bf002-9d27-4e96-9c9f-1f4b8a0c5e6d` |
| Write (host → board)  | `7e2bf003-9d27-4e96-9c9f-1f4b8a0c5e6d` |

Click BLE in the rail, pick the device from the browser's BLE picker
(Chromium-only), grant permission. State packets stream over the
notify characteristic; commands go out via the write characteristic.

## Session diff

A device card's session can be compared against another saved session
to see exactly what changed. Open a card, click DIFF in the session
controls (next to EXPORT and IMPORT), pick a session JSON file. A modal
opens showing every field that differs:

- per-pin calibrations (added, removed, changed slope/offset/label/unit)
- threshold alerts (added, removed, condition or value changed)
- I2C poll configurations (slot reassignments, address/register changes)
- cross-pin math expressions (added, removed, expression changed)
- trigger configuration changes
- script source changes (unified diff inline)
- plugin state changes

The diff is read-only; it doesn't modify the active session. Useful
for catching calibration drift between test rigs, confirming that a
"identical" board is actually identical, or pre-flight checking a
session JSON before committing it to a repo.

---

## Sessions

Every device card auto-saves its configuration to localStorage 400 ms
after any change. The saved fields are:

- alias, ADC resolution, sample rate
- per-pin calibrations
- active strip chart traces
- threshold alerts
- I2C poll slot configurations
- math expressions
- RGB LED pin assignment and most recent color
- trigger configuration (channel, condition, value, pre/post windows)
- scripted automation source
- plugin state (everything plugins write via `api.persist`)

The EXPORT button writes the same snapshot to a JSON file. IMPORT
reads one back. DIFF compares the current session against a file (see
[Session diff](#session-diff)).

Session file format is versioned. Current version is 1.3 (additive
over 1.2, 1.1, and 1.0). Older session files load fine with the new
fields defaulted. Trigger state restores to "armed" so it can fire
again, not stuck in the captured-but-frozen state. Plugins are
force-remounted on import so each plugin's `onInit` re-reads the
restored state via `api.recall()`.

## Continuous integration

The repo ships with a GitHub Actions workflow at
`.github/workflows/ci.yml` that runs on every push and pull request,
plus on demand via `workflow_dispatch`. Five jobs:

| Job              | What it does                                                                                                                                                             |
| ---------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `lint`           | extracts the JS from `pinscope.html`, runs JSHint with `.jshintrc`, then Prettier on text files, html-validate, and a hard fail on em dashes anywhere in tracked sources |
| `compile-serial` | arduino-cli compile matrix for `pinscope.ino` on Uno R3, Nano 33 IoT, and Uno R4 WiFi                                                                                    |
| `compile-ble`    | matrix for `pinscope_ble.ino` on Nano 33 IoT and Uno R4 WiFi (the two boards with first-class ArduinoBLE support); Uno Q is omitted pending Zephyr-core verification     |
| `compile-mqtt`   | matrix for `pinscope_mqtt.ino` on the two NINA boards with WiFiNINA and ArduinoMqttClient                                                                                |
| `bridge-syntax`  | compiles the Python bridge with `py_compile` and runs `ruff` for style                                                                                                   |

The workflow uses the official `arduino/compile-sketches@v1` action.
Each sketch is staged into a folder of the same name before compile so
arduino-cli is happy with the flat repo layout.

To run the same lint checks locally:

```sh
scripts/lint.sh             # all checks
scripts/lint.sh --fix       # auto-format with Prettier
scripts/lint.sh --js-only   # just JSHint on the embedded JS
```

To compile a sketch locally against a chosen FQBN:

```sh
scripts/compile.sh pinscope.ino arduino:renesas_uno:unor4wifi
```

Requires `arduino-cli` on your PATH plus the relevant cores and
libraries installed (see `CONTRIBUTING.md`).

## License

PinScope (Field Instrument 005) is free software: you can redistribute
it and/or modify it under the terms of the GNU General Public License
as published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program. If not, see https://www.gnu.org/licenses/gpl-3.0.html

SPDX-License-Identifier: GPL-3.0-or-later

Copyright (C) 2025-2026 Michael B. Parks · Green Shoe Garage
