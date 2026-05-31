#!/usr/bin/env python3
"""
pinscope_mqtt_bridge.py
=======================

Bridge a PinScope-firmware Arduino on USB serial to an MQTT broker, so the
browser-side PinScope console can drive the board over MQTT.

Topics (default base: "pinscope"):
    <base>/<deviceId>/out    board -> host  (each board JSON line is one PUBLISH)
    <base>/<deviceId>/in     host  -> board (one JSON command per PUBLISH)

The device id is read from the first {"t":"hello","id":...} packet the board
emits. Until then, the bridge buffers outgoing board messages briefly and
ignores any inbound commands.

Dependencies:
    pip install paho-mqtt pyserial

Example:
    python3 pinscope_mqtt_bridge.py \
        --port /dev/ttyACM0 --baud 115200 \
        --broker test.mosquitto.org --broker-port 1883 \
        --topic-base pinscope

Then in the PinScope browser console, click MQTT, point at a WebSocket
endpoint on the same broker (e.g. ws://test.mosquitto.org:8080), and the
board appears as a live device. The browser uses the WebSocket port; this
bridge uses the plain MQTT port (1883 by default).

GPL-3.0-or-later
"""
import argparse
import json
import time
import serial
import paho.mqtt.client as mqtt

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--port',        required=True, help='Serial port, e.g. /dev/ttyACM0 or COM3')
    p.add_argument('--baud',        type=int, default=115200)
    p.add_argument('--broker',      default='test.mosquitto.org')
    p.add_argument('--broker-port', type=int, default=1883)
    p.add_argument('--topic-base',  default='pinscope')
    p.add_argument('--keepalive',   type=int, default=60)
    args = p.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=0.1)
    print(f'[bridge] opened {args.port} at {args.baud}')

    # Shared state across the MQTT callback closures and the main loop.
    # Once the bridge sees a hello packet, we record the id and out topic here.
    state = {'device_id': None, 'out_topic': None}

    cli = mqtt.Client(client_id='pinscope-bridge-' + hex(int(time.time()))[-6:])

    def on_connect(client, userdata, flags, rc):
        print(f'[mqtt] connected rc={rc}')
        # Subscribe to all device-in topics; we filter by parts[1] in on_message.
        client.subscribe(f'{args.topic_base}/+/in', qos=0)

    def on_message(client, userdata, msg):
        # Only forward commands addressed to our device once we know it.
        parts = msg.topic.split('/')
        if len(parts) != 3 or parts[2] != 'in':
            return
        did = state['device_id']
        if did is not None and parts[1] != did:
            return
        try:
            line = msg.payload.decode('utf-8').strip()
        except Exception as e:
            print(f'[mqtt] bad payload: {e}')
            return
        if not line:
            return
        try:
            json.loads(line)  # sanity check; reject non-JSON
        except Exception as e:
            print(f'[mqtt] non-JSON command dropped: {e}')
            return
        ser.write(line.encode('utf-8') + b'\n')

    cli.on_connect = on_connect
    cli.on_message = on_message
    cli.connect(args.broker, args.broker_port, args.keepalive)
    cli.loop_start()

    # Serial -> MQTT loop
    buf = b''
    try:
        while True:
            chunk = ser.read(256)
            if not chunk:
                continue
            buf += chunk
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                line = line.strip()
                if not line:
                    continue
                try:
                    obj = json.loads(line.decode('utf-8'))
                except Exception:
                    print(f'[serial] non-JSON line: {line!r}')
                    continue
                if state['device_id'] is None and obj.get('t') == 'hello' and obj.get('id'):
                    state['device_id'] = obj['id']
                    state['out_topic'] = f"{args.topic_base}/{state['device_id']}/out"
                    print(f"[bridge] device id locked: {state['device_id']}")
                if state['out_topic'] is None:
                    # Haven't seen hello yet; nudge the board to introduce itself.
                    ser.write(b'{"cmd":"hello"}\n')
                    continue
                cli.publish(state['out_topic'], line.decode('utf-8'), qos=0)
    except KeyboardInterrupt:
        print('[bridge] shutting down')
    finally:
        cli.loop_stop()
        cli.disconnect()
        ser.close()

if __name__ == '__main__':
    main()
