/*
 * pinscope.ino
 *
 * Reference firmware for PINSCOPE (Field Instrument 005), the Arduino Uno Q
 * I/O console. Implements the PinScope line-delimited JSON wire protocol
 * over Serial at 115200 baud.
 *
 * Compatible with the classic Uno API surface, so it also works on a
 * stock Arduino Uno R3 and most ATmega328P / SAMD / STM32 cores.
 *
 * Wire protocol (one JSON object per line, both directions):
 *
 *   Host -> board:
 *     {"cmd":"hello"}
 *     {"cmd":"poll"}
 *     {"cmd":"mode","pin":N,"mode":"off|in|inp|out|pwm"}
 *     {"cmd":"set","pin":N,"val":0|1}
 *     {"cmd":"pwm","pin":N,"val":0..255}
 *
 *   Board -> host:
 *     {"t":"hello","id":"FI005-XXXX","name":"Arduino Uno Q"}
 *     {"t":"state","d":[14 ints],"a":[6 ints],"m":[14 strings]}
 *     {"t":"ack","cmd":"..."}
 *     {"t":"err","msg":"..."}
 *
 * Notes:
 *   - D0 and D1 are reserved (USB serial UART) and are not driven.
 *   - PWM-capable pins on Uno-family: 3, 5, 6, 9, 10, 11.
 *   - State packets are pushed at STATE_HZ regardless of host polling.
 *   - The JSON reader is a minimal scanner, not a full parser. It tolerates
 *     extra whitespace but not nested objects, which this protocol never uses.
 *
 * GPL-3.0-or-later
 */

#include <Arduino.h>

// -------- CONFIG -----------------------------------------------------------
static const uint32_t BAUD          = 115200;
static const uint16_t STATE_PERIOD  = 100;   // ms; ~10 Hz state push
static const uint8_t  NUM_DIGITAL   = 14;
static const uint8_t  NUM_ANALOG    = 6;
static const uint8_t  RX_BUF_SIZE   = 96;

// PWM-capable pins on Uno-family
static bool isPwmPin(uint8_t p) {
  return p == 3 || p == 5 || p == 6 || p == 9 || p == 10 || p == 11;
}
// USB-shared UART
static bool isReserved(uint8_t p) { return p == 0 || p == 1; }

// -------- STATE ------------------------------------------------------------
// Modes: 0=off, 1=in, 2=inp, 3=out, 4=pwm
static uint8_t  pinModes[NUM_DIGITAL];
static uint8_t  pwmVals[NUM_DIGITAL];

static char     rxBuf[RX_BUF_SIZE];
static uint8_t  rxLen = 0;
static uint32_t lastState = 0;

// -------- OUTPUT HELPERS ---------------------------------------------------
static const char* modeName(uint8_t m) {
  switch (m) {
    case 1: return "in";
    case 2: return "inp";
    case 3: return "out";
    case 4: return "pwm";
    default: return "off";
  }
}

static void sendHello() {
  // Build a short ephemeral id from a free-running counter
  uint16_t id = (uint16_t)(micros() & 0xFFFF);
  Serial.print(F("{\"t\":\"hello\",\"id\":\"FI005-"));
  for (int8_t shift = 12; shift >= 0; shift -= 4) {
    uint8_t nib = (id >> shift) & 0xF;
    Serial.print((char)(nib < 10 ? '0' + nib : 'A' + nib - 10));
  }
  Serial.println(F("\",\"name\":\"Arduino Uno Q\"}"));
}

static uint8_t readDigitalForReport(uint8_t p) {
  // For OUTPUT pins, digitalRead reflects what we last drove on most cores,
  // which is what users want to see echoed back. PWM pins report 0 here;
  // the host already knows the PWM duty it set.
  if (pinModes[p] == 0) return 0;
  if (pinModes[p] == 4) return 0;
  return (uint8_t)digitalRead(p);
}

static void sendState() {
  Serial.print(F("{\"t\":\"state\",\"d\":["));
  for (uint8_t p = 0; p < NUM_DIGITAL; p++) {
    if (p) Serial.print(',');
    Serial.print(readDigitalForReport(p));
  }
  Serial.print(F("],\"a\":["));
  for (uint8_t a = 0; a < NUM_ANALOG; a++) {
    if (a) Serial.print(',');
    Serial.print(analogRead(A0 + a));
  }
  Serial.print(F("],\"m\":["));
  for (uint8_t p = 0; p < NUM_DIGITAL; p++) {
    if (p) Serial.print(',');
    Serial.print('"');
    Serial.print(modeName(pinModes[p]));
    Serial.print('"');
  }
  Serial.println(F("]}"));
}

static void sendAck(const char* cmd) {
  Serial.print(F("{\"t\":\"ack\",\"cmd\":\""));
  Serial.print(cmd);
  Serial.println(F("\"}"));
}

static void sendErr(const char* msg) {
  Serial.print(F("{\"t\":\"err\",\"msg\":\""));
  Serial.print(msg);
  Serial.println(F("\"}"));
}

// -------- TINY JSON SCANNER -----------------------------------------------
// findField returns the index of the first non-space, non-colon char after
// the matching "key": pattern, or -1 if not found.
static int findField(const char* buf, const char* key) {
  uint8_t klen = strlen(key);
  uint8_t len  = strlen(buf);
  if (len < klen + 3) return -1;
  for (uint8_t i = 0; i + klen + 2 < len; i++) {
    if (buf[i] != '"') continue;
    if (strncmp(&buf[i + 1], key, klen) != 0) continue;
    if (buf[i + 1 + klen] != '"') continue;
    uint8_t j = i + 2 + klen;
    while (j < len && (buf[j] == ' ' || buf[j] == ':')) j++;
    return (int)j;
  }
  return -1;
}

static bool readStringVal(const char* buf, const char* key, char* out, uint8_t outSize) {
  int p = findField(buf, key);
  if (p < 0 || buf[p] != '"') return false;
  p++;
  uint8_t o = 0;
  while (buf[p] && buf[p] != '"' && o < outSize - 1) out[o++] = buf[p++];
  out[o] = 0;
  return true;
}

static bool readIntVal(const char* buf, const char* key, int* out) {
  int p = findField(buf, key);
  if (p < 0) return false;
  *out = atoi(&buf[p]);
  return true;
}

// -------- COMMAND HANDLING -------------------------------------------------
static void applyMode(uint8_t pin, const char* m) {
  if (pin >= NUM_DIGITAL)   { sendErr("bad pin");      return; }
  if (isReserved(pin))      { sendErr("pin reserved"); return; }

  if      (!strcmp(m, "off")) { pinModes[pin] = 0; pinMode(pin, INPUT); }
  else if (!strcmp(m, "in"))  { pinModes[pin] = 1; pinMode(pin, INPUT); }
  else if (!strcmp(m, "inp")) { pinModes[pin] = 2; pinMode(pin, INPUT_PULLUP); }
  else if (!strcmp(m, "out")) { pinModes[pin] = 3; pinMode(pin, OUTPUT); digitalWrite(pin, LOW); }
  else if (!strcmp(m, "pwm")) {
    if (!isPwmPin(pin)) { sendErr("pin not PWM-capable"); return; }
    pinModes[pin] = 4;
    pinMode(pin, OUTPUT);
    analogWrite(pin, pwmVals[pin]);
  }
  else { sendErr("bad mode"); return; }
  sendAck("mode");
}

static void handleLine(const char* buf) {
  char cmd[10];
  if (!readStringVal(buf, "cmd", cmd, sizeof(cmd))) { sendErr("no cmd"); return; }

  if (!strcmp(cmd, "hello")) {
    sendHello();
    sendState();
    return;
  }
  if (!strcmp(cmd, "poll")) {
    sendState();
    return;
  }
  if (!strcmp(cmd, "mode")) {
    int pin; char m[8];
    if (!readIntVal(buf, "pin", &pin) || !readStringVal(buf, "mode", m, sizeof(m))) {
      sendErr("missing pin/mode");
      return;
    }
    applyMode((uint8_t)pin, m);
    return;
  }
  if (!strcmp(cmd, "set")) {
    int pin, val;
    if (!readIntVal(buf, "pin", &pin) || !readIntVal(buf, "val", &val)) {
      sendErr("missing pin/val");
      return;
    }
    if (pin < 0 || pin >= NUM_DIGITAL) { sendErr("bad pin");        return; }
    if (isReserved(pin))               { sendErr("pin reserved");   return; }
    if (pinModes[pin] != 3)            { sendErr("pin not output"); return; }
    digitalWrite(pin, val ? HIGH : LOW);
    sendAck("set");
    return;
  }
  if (!strcmp(cmd, "pwm")) {
    int pin, val;
    if (!readIntVal(buf, "pin", &pin) || !readIntVal(buf, "val", &val)) {
      sendErr("missing pin/val");
      return;
    }
    if (pin < 0 || pin >= NUM_DIGITAL) { sendErr("bad pin");     return; }
    if (pinModes[pin] != 4)            { sendErr("pin not PWM"); return; }
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    pwmVals[pin] = (uint8_t)val;
    analogWrite(pin, pwmVals[pin]);
    sendAck("pwm");
    return;
  }
  sendErr("unknown cmd");
}

// -------- ARDUINO ENTRY POINTS --------------------------------------------
void setup() {
  Serial.begin(BAUD);
  // Brief wait for the USB-CDC link to come up on boards that need it,
  // but don't block forever if running standalone.
  uint32_t deadline = millis() + 2000;
  while (!Serial && millis() < deadline) { /* wait */ }

  for (uint8_t i = 0; i < NUM_DIGITAL; i++) {
    pinModes[i] = 0;
    pwmVals[i]  = 0;
  }
  // Leave D0 / D1 alone (USB-shared UART).
}

void loop() {
  // Drain incoming bytes into the line buffer
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (rxLen > 0) {
        rxBuf[rxLen] = 0;
        handleLine(rxBuf);
        rxLen = 0;
      }
    } else if (rxLen < RX_BUF_SIZE - 1) {
      rxBuf[rxLen++] = c;
    } else {
      // overflow; reset buffer to avoid lockup
      rxLen = 0;
      sendErr("rx overflow");
    }
  }

  // Periodic state push
  uint32_t now = millis();
  if (now - lastState >= STATE_PERIOD) {
    lastState = now;
    sendState();
  }
}
