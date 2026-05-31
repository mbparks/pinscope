# PinScope Bring-Up: Arduino Uno Q on macOS

Bring up the PinScope serial firmware on an Arduino Uno Q step-by-step.
Each phase has a clear pass criterion. When something fails, capture the
exact symptom (error text, screenshot, output) and we work from there.

The Uno Q is the most uncertain target in the PinScope matrix because its
MCU runs Arduino sketches under Zephyr OS via the `arduino:zephyr:unoq`
core, not bare-metal classic Arduino. So we go slowly and verify at each
layer instead of jumping straight to BLE/MQTT.

## Phase 0: Hardware sanity

Before anything software-related.

- [ ] Uno Q powered up via USB-C, both LEDs near the USB port lit
- [ ] You see the board enumerate in `System Information > USB`
      (Apple menu > About This Mac > More Info > USB)
- [ ] In Terminal: `ls /dev/cu.usbmodem*` lists at least one device

**Expected pass:** at least one `/dev/cu.usbmodem*` device appears.

**Failure modes:**

- No device shows up: try a different USB-C cable (many USB-C cables are
  charge-only and don't carry data). The original Apple cables that come
  with iPhones are usually charge-only too.
- Device shows up but disappears after a few seconds: power-delivery
  issue. Try a different port or a powered hub.
- Two devices show up: the Uno Q exposes both the Qualcomm Linux side
  (over USB ethernet/serial) and the STM32U585 MCU side. Both are normal.

**Tell me:** the exact output of `ls /dev/cu.usbmodem*` so we know which
ports are which.

## Phase 1: Tooling install

Pick one (or do both, as you said).

### Option A: Arduino IDE 2.x

- [ ] Download Arduino IDE 2.x from arduino.cc (not the legacy 1.x)
- [ ] First launch, let it auto-install whatever it offers
- [ ] Tools > Board > Boards Manager: search "uno q"
- [ ] Install **"Arduino UNO Q Boards"** or **"arduino:zephyr"** package
      (exact name depends on the Boards Manager catalog at the time)
- [ ] Tools > Board > Arduino UNO Q (it should appear after install)
- [ ] Tools > Port: pick the `/dev/cu.usbmodem*` that matches the MCU side

### Option B: arduino-cli

```sh
# Install via Homebrew if you don't have it
brew install arduino-cli

# Update the index
arduino-cli core update-index

# Look for the Uno Q core
arduino-cli core search uno
arduino-cli core search zephyr

# Install it (the exact identifier comes from the search output above;
# expect something like "arduino:zephyr" or "arduino:renesas_zephyr")
arduino-cli core install arduino:zephyr

# Verify
arduino-cli board listall | grep -i "uno q"
```

**Expected pass:** the board shows up in the board list with FQBN
`arduino:zephyr:unoq` (or close to it; the exact name may differ).

**Failure modes:**

- "Board not found" after install: the Boards Manager may not yet expose
  Uno Q via the public catalog at the time you read this. Workaround:
  install Arduino App Lab from arduino.cc, which bundles a compatible
  toolchain. App Lab uses the same arduino-cli underneath, so you can
  still run our scripts; you just point them at App Lab's bundled CLI.
- arduino-cli is much older than the Uno Q release date: `brew upgrade
arduino-cli`. Anything older than 1.0.0 likely won't recognize the
  Zephyr core.

**Tell me:** the output of `arduino-cli board listall | grep -i uno`, and
the FQBN that appears for the Uno Q.

## Phase 2: Compile the serial firmware

This is the first real test. We're not flashing yet, just confirming the
sketch source compiles against the Zephyr-based Uno Q core.

`pinscope.ino` has board-detection macros at the top (`ARDUINO_ARCH_ZEPHYR`,
`ARDUINO_UNOR4_WIFI`, `ARDUINO_SAMD_NANO_33_IOT`, etc.) that set the right
ADC resolution and board name per target. The Uno Q branch picks up
`PINSCOPE_ADC_BITS = 12` and reports itself as "Arduino Uno Q" in the
hello packet. No manual edits should be needed.

```sh
cd pinscope

# Use the helper script with the FQBN from phase 1
scripts/compile.sh pinscope.ino arduino:zephyr:unoq
```

Or from the IDE: open `pinscope.ino`, hit the checkmark (Verify).

**Expected pass:** clean compile, no errors.

**Failure modes I'm still watching for:**

- `attachInterrupt` or `digitalPinToInterrupt` undefined: the Zephyr core
  is supposed to expose the classic Arduino interrupt API, but if it
  doesn't, FREQ mode would need to be stubbed out with another guard
  in the board-detection block. Capture the exact error if this happens.
- `Wire.h` not found: Uno Q's Zephyr core should include Wire, but if it
  lives under a different name (e.g. `ZephyrI2C.h`), we need a
  conditional include.
- `analogReadResolution(12)` undefined: would only happen if the core
  surfaces ADC config differently. The `PINSCOPE_SET_ADC_RES` macro
  guards the call so we can flip it to 0 if needed for a specific board.
- `digitalPinToInterrupt(pin)` returns `NOT_AN_INTERRUPT` everywhere on
  Uno Q at runtime: compile-time fine, runtime issue caught in Phase 5
  when FREQ mode is exercised.
- "Sketch too big" or "no upload protocol defined": library or board
  config mismatch. Paste the full message.

**Tell me:** if it compiles, the size summary line (e.g. "Sketch uses
N bytes"). If it fails, the full error block.

## Phase 3: Flash

```sh
arduino-cli upload -p /dev/cu.usbmodemXXXX --fqbn arduino:zephyr:unoq /path/to/sketch
```

From the IDE: hit the arrow (Upload).

**Expected pass:** upload completes, you see something like "avrdude
done. Thank you." (or the Renesas/Zephyr equivalent). The board
reboots; the on-board LED behavior may briefly change.

**Failure modes:**

- 1200-bps touch fails: known issue with R4 family; less common on Uno
  Q but possible. Try double-tapping the reset button on the board to
  force bootloader mode, then upload.
- "Port not found" after upload starts: the board rebooted into a
  different port. macOS sometimes reassigns the `/dev/cu.usbmodem*`
  number. Re-run `ls /dev/cu.usbmodem*` after the reboot.
- Permission denied: macOS rarely needs this, but if it does, check
  System Settings > Privacy & Security for a blocked "developer tool"
  prompt.

**Tell me:** the last 10 lines of upload output, success or fail.

## Phase 4: Serial monitor smoke test

Don't open PinScope yet. First confirm the firmware boots and emits
something sensible.

- [ ] Open the Arduino IDE Serial Monitor (Tools > Serial Monitor) at
      115200 baud, or run `screen /dev/cu.usbmodemXXXX 115200` from Terminal.
- [ ] Send the literal text `{"cmd":"hello"}` followed by Enter (newline)

**Expected pass:** the board responds with something like:

```
{"t":"hello","id":"FI005-XXXX","name":"Arduino Uno Q","hz":10,"adcMax":4095}
{"t":"state","d":[0,0,0,...],"a":[N,N,...],"m":["off","off",...],"v":[null,null,null,null],"f":[-1,-1,...]}
```

The `adcMax:4095` is the 12-bit ADC reporting. Classic AVR boards would
send `adcMax:1023` instead. The browser uses this to scale the analog
bars and strip chart percent axes automatically.

Then approximately every 100 ms, another `state` line.

**Failure modes:**

- Nothing at all: serial isn't connecting. Check baud (must be 115200),
  port, and that nothing else is holding the port open.
- Garbled text or wrong baud: the firmware came up but at a different
  rate. The Zephyr USB CDC may ignore the baud setting; verify
  `Serial.begin(115200)` is being honored by trying common alternates
  (9600, 921600).
- Only `state` lines, no `hello`: the firmware is alive but missed
  your command, or `hello` isn't being parsed. Send `{"cmd":"poll"}`
  to confirm command parsing works. If `poll` works but `hello`
  doesn't, that's a bug in our `handleLine`.
- Only `hello`, no `state` stream: the `loop()` is running but
  `sendState()` isn't being called periodically. The `millis()` clock
  on Zephyr may behave differently.
- JSON looks malformed (missing commas, truncated): buffer-size or
  print-flush issue. Capture an exact sample line.

**Tell me:** paste 5 to 10 lines of what comes out of serial after you
send `{"cmd":"hello"}`, including any error messages.

## Phase 5: PinScope console connect

Now the fun part.

- [ ] Open `pinscope.html` in Chrome or Edge on the same Mac
- [ ] Click SERIAL
- [ ] Pick the same `/dev/cu.usbmodem*` from the picker
- [ ] Allow access

**Expected pass:** within a second, a device card appears with title
"Arduino Uno Q", an id like `FI005-XXXX`, and a live tx/rx counter
ticking up. Analog channel bars for A0 to A5 wiggle (any unconnected
analog input on the Uno Q reads a small floating voltage).

**Failure modes:**

- Serial picker shows no devices: Web Serial requires HTTPS or
  `file://`. Make sure you opened the HTML file from disk, not from
  http://. Also: only Chromium-based browsers support Web Serial.
- Picker shows the device but "Connection failed": another process
  has the port open. Close the Arduino IDE Serial Monitor (it locks
  the port) and try again.
- Device appears but no state updates: tx counter goes up, rx stays
  at zero. The browser is sending but the firmware isn't responding.
  Re-check Phase 4.
- Card appears but mode badges all show "OFF" and never react when
  clicked: the `cmd:mode` packets aren't reaching the firmware or
  aren't being acked. Click a mode badge, then check the Wire Log on
  the device card (bottom of the card) and tell me what you see.

**Tell me:** screenshot of the device card if anything looks off.

## Phase 6: Pin control round-trip

The first real interactive test.

- [ ] Click D13's mode badge until it reads OUT. The card should
      show a HIGH/LOW reading.
- [ ] Click the toggle button (now visible on D13). The on-board
      user LED should light up.
- [ ] Click again, LED off.

**Expected pass:** LED responds to clicks within ~100 ms.

**Failure modes:**

- Mode cycles but no LED: D13 may not be wired to the on-board LED on
  the Uno Q (some boards moved it). Try D12 or D11.
- Click registers but Wire Log shows an error from the board: the
  firmware rejected the mode change. Common cause: pin is reserved
  in the firmware's pin map.
- Lag of more than a second: the firmware's state push rate is fine
  (10 Hz default), so this would be a USB CDC buffering issue under
  Zephyr. Bump the rate to 20 or 50 Hz in the Hz selector and see if
  it improves.

**Tell me:** does the LED respond, and what's the Wire Log entry for
the mode-change command.

## Phase 7: Analog + strip chart

- [ ] On the device card, click the A0 chip in the trace selector
      under the strip chart. A trace should start drawing.
- [ ] Touch A0 with your finger (the floating input picks up
      60 Hz hum on most boards). The trace should jump around.
- [ ] Connect A0 to ground for a moment: trace should drop to 0.
- [ ] Connect A0 to 3.3V: trace should peg.

**Expected pass:** trace responds instantly, values match the
voltage you'd expect (raw ADC values, 0 to 1023 by default).

**Failure modes:**

- Trace is a flat line at exactly 0 or exactly 4095 (or 1023 on AVR):
  ADC isn't being read, or `analogRead(A0+i)` doesn't resolve correctly
  on this board's core.
- Values are weirdly truncated to 0-255 or to a 10-bit range when the
  board should be 12-bit: the hello packet's `adcMax` didn't propagate
  to the browser, or `analogReadResolution(12)` isn't taking effect in
  the firmware. The 10/12-bit toggle in the analog header should now
  match `adcMax` from the hello packet; if it's stuck on 10 when the
  board is 12-bit, the auto-detect path is broken. Click the "12" toggle
  manually as a workaround and tell me, that's a real bug.

**Tell me:** rough values shown on the A0 chip when input is floating,
grounded, and at 3.3V.

## Phase 8: PWM

- [ ] Click D11 (a PWM-capable pin) mode badge to cycle to PWM
- [ ] A slider appears under the pin face
- [ ] Drag it from 0 to 255
- [ ] If you have an LED + resistor on D11, watch the brightness ramp

**Expected pass:** smooth ramp, no flicker steps.

**Failure modes:**

- PWM duty doesn't change: `analogWrite` semantics on the Uno Q
  Zephyr core may be different. Capture the Wire Log for the `pwm`
  command and tell me.

## Phase 9: FREQ mode (if Phase 2 didn't already crater it)

- [ ] Wire D2 to D11 with a jumper. D11 is in PWM mode from Phase 8.
- [ ] Cycle D2's mode to FREQ
- [ ] D2's face should show a Hz reading

**Expected pass:** Hz value matches PWM frequency on the Uno Q
(typically 490 Hz for non-D5/D6 pins, but Zephyr core may differ).

**Failure modes:**

- "no interrupt on pin": `digitalPinToInterrupt(2)` returned
  `NOT_AN_INTERRUPT`. Try D3 instead. If all pins return NOT_AN_INTERRUPT,
  the Zephyr core doesn't expose the classic interrupt API and we need
  a Zephyr-native FREQ implementation.
- "0 Hz" persistently: ISR is attached but never firing. Could be
  edge polarity or signal-level issue.

**Tell me:** the Hz reading shown, or the error message.

## Phase 10: I2C scan (only if you have any I2C device wired up)

Skip this if you don't have a Qwiic device or any I2C breakout handy.

- [ ] Wire SDA/SCL/GND/3V3 to any I2C device (BME280, MPU6050,
      SSD1306, etc.)
- [ ] Click SCAN BUS in the I2C section
- [ ] Grid should highlight the address(es) of attached devices

**Expected pass:** at least one address lights up in the grid.

**Failure modes:**

- Empty scan: wiring (pull-ups? supply voltage? SDA/SCL swapped?) or
  the Wire library on Uno Q is not using the Qwiic pins by default.
  The Uno Q has a Qwiic connector; verify which SDA/SCL pins it routes
  to by default in the Uno Q docs.

**Tell me:** which device you wired up, and what addresses lit up.

## Phase 11: Session save + replay

The non-hardware sanity checks.

- [ ] Set up a couple of pins, calibrate A0, add a threshold alert
- [ ] Click EXPORT, save the JSON file
- [ ] Reload `pinscope.html` in the browser
- [ ] Reconnect
- [ ] Confirm: device card appears with the same alias, alerts list
      is populated, A0's calibration label shows on the chip

If autosave is working, just reloading should restore the session
without any manual import.

- [ ] Click CAPTURE on the strip chart
- [ ] Disconnect
- [ ] Click REPLAY, pick the capture JSON
- [ ] Confirm: a `[replay]` card appears with the captured trace
      looping

## What success looks like

Phases 1 to 8 all green = the core PinScope experience works on Uno Q.
Phase 9 + 10 + 11 are bonus and tell us how complete the Zephyr-core
compatibility is.

After bring-up, we can move on to:

- Patching `pinscope.ino` for any Uno Q quirks we found
- Updating the README to remove "experimental" from the Uno Q claim
- BLE bring-up on Uno Q (or punt to a more confirmed BLE target like
  Nano 33 IoT)
- A short blog post or video walkthrough showing PinScope on real
  hardware

## When you get stuck

Paste these in our next message:

1. Which phase you got to
2. What the expected pass criterion was
3. What actually happened (exact text or screenshot)
4. The relevant Wire Log entries if any

That's enough for me to figure out where to look. If it's a firmware
bug we'll fix it; if it's a tooling issue we'll work around it; if it's
a Zephyr-core gap we'll document it and pick a path forward.
