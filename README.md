# PinScope

Field Instrument 005. A single-file HTML console for connecting to one or more Arduino Uno Q boards over USB serial or WebSocket, with a visual readout of all I/O and live toggles for outputs.

## Contents

- `pinscope.html`. The console. Open it directly in a browser, no build step.
- `pinscope.ino`. Reference firmware sketch for the Uno Q (also runs on classic Uno R3 and most Arduino cores).
- `README.md`. This file.

## What it does

Connects to one or more Arduino Uno Q boards and shows:

- All 14 digital pins (D0 to D13) with mode (off, input, input-pullup, output, PWM), state lamp, and live toggle or PWM slider.
- All 6 analog inputs (A0 to A5) as horizontal bar graphs reading 0 to 1023.
- A wire log showing every JSON packet in both directions.

Each board appears as its own card. Connect as many as you need. Pin modes are configured from the UI by clicking the mode badge to cycle through `OFF -> IN -> IN-PU -> OUT -> PWM`. PWM is skipped on non-PWM pins.

D0 and D1 are marked reserved because they share the USB-serial UART when using the serial transport.

## Running

### Browser side

Open `pinscope.html` in Chrome, Edge, or Opera (Web Serial API is required for USB serial connections; Firefox and Safari can still use the WiFi transport). Click `SERIAL` to attach a USB-connected board, or `WIFI` to attach a WebSocket endpoint.

For Web Serial to work the page must be served from `https://`, `http://localhost`, or opened as a local file with the appropriate browser flag. The simplest path is opening the file directly from disk.

### Board side, serial

Flash `pinscope.ino` to the Uno Q (or any Uno-family board) using the Arduino IDE. Board type: Arduino Uno Q. Baud: 115200. No external libraries required.

Connect via USB, click `SERIAL` in the console, and select the port in the browser picker.

### Board side, WiFi

The Uno Q runs Linux on the MPU side and Arduino sketches on the MCU side. The recommended pattern is to run a small WebSocket bridge on the Linux side that relays JSON lines to and from the MCU UART. A minimal bridge looks like this in Python:

```python
# bridge.py, runs on the Uno Q Linux side
import asyncio, serial_asyncio, websockets

PORT = '/dev/ttyAMA0'   # adjust to your MCU UART
BAUD = 115200
WS_PORT = 81

async def bridge(ws, ser_w, ser_r):
    async def ws_to_serial():
        async for msg in ws:
            ser_w.write((msg.strip() + '\n').encode())
    async def serial_to_ws():
        buf = b''
        while True:
            chunk = await ser_r.read(256)
            buf += chunk
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                await ws.send(line.decode(errors='ignore').strip())
    await asyncio.gather(ws_to_serial(), serial_to_ws())

async def main():
    reader, writer = await serial_asyncio.open_serial_connection(url=PORT, baudrate=BAUD)
    async def handler(ws):
        await bridge(ws, writer, reader)
    async with websockets.serve(handler, '0.0.0.0', WS_PORT):
        await asyncio.Future()

asyncio.run(main())
```

Then click `WIFI` in the console and enter `ws://<board-ip>:81`. The same firmware sketch runs on the MCU; only the transport changes.

The same WebSocket approach works for a desktop bridge if you would rather keep the board pure-MCU and run the bridge on your workstation.

## Wire protocol

Line-delimited JSON over a duplex byte stream, 115200 8N1. One object per line, terminated with `\n`. The protocol is symmetric across serial and WebSocket transports.

### Host to board

```
{"cmd":"hello"}
{"cmd":"poll"}
{"cmd":"mode","pin":<0..13>,"mode":"off"|"in"|"inp"|"out"|"pwm"}
{"cmd":"set","pin":<0..13>,"val":0|1}
{"cmd":"pwm","pin":<0..13>,"val":<0..255>}
```

### Board to host

```
{"t":"hello","id":"FI005-XXXX","name":"Arduino Uno Q"}
{"t":"state","d":[14 ints, 0|1],"a":[6 ints, 0..1023],"m":[14 strings]}
{"t":"ack","cmd":"<command name>"}
{"t":"err","msg":"<short message>"}
```

The board pushes a `state` packet at roughly 10 Hz regardless of polling.

## Architecture

The PinScope console is structured as a small set of cooperating classes so transports and views can be added without touching the rest.

```
Transport (EventTarget)              open, close, message, error
  ├── SerialTransport                Web Serial API
  ├── WiFiTransport                  WebSocket
  └── (extend here: BLE, MQTT, USB-CDC, etc.)

Device (EventTarget)                 wraps one Transport; tracks pin state
DeviceManager                        collection of Devices
DeviceCard                           per-device DOM view
```

To add a new transport, subclass `Transport`, implement `connect`, `disconnect`, and `send`, and emit `open`, `close`, `message`, and `error` events. The rest of the system needs no changes.

To support additional pin types (encoders, I2C sensors, the Qwiic bus, the built-in RGB LEDs) extend the wire protocol with new `cmd` and `t` values and add render methods to `DeviceCard`. The state packet already has room to grow by adding new keys; older fields stay backward compatible.

## Notes specific to the Uno Q

- The Uno Q runs at 3.3 V logic. Do not feed 5 V signals into GPIO without a level shifter.
- The on-board user LED is on D13.
- Pins 0 and 1 are the USB-shared UART. The console marks them reserved.
- PWM-capable pins are 3, 5, 6, 9, 10, 11.
- The MCU ADC is 12-bit native, but the default Arduino `analogRead` returns a 10-bit value (0 to 1023) for Uno compatibility. The console scales bars to that range.
- The Qwiic connector and the high-speed MIPI / DSI connectors on the bottom of the board are not exposed by this protocol yet. They are good extension points.

## License

```
PinScope (Field Instrument 005)
Copyright (C) 2026 Michael B. Parks

This program is free software: you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation, either version 3 of the License, or (at your
option) any later version.

This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see https://www.gnu.org/licenses/.
```
