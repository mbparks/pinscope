/*
 * pinscope_mqtt.ino
 *
 * MQTT-capable firmware for PINSCOPE (Field Instrument 005). Publishes the
 * PinScope JSON wire protocol directly to an MQTT broker over WiFi, so the
 * browser console can talk to the board with no host-side bridge process.
 *
 * Verified on:
 *   - Arduino Uno R4 WiFi   (Renesas RA4M1 + ESP32-S3 WiFi/BLE coprocessor)
 *   - Arduino Nano 33 IoT   (SAMD21          + NINA-W102 WiFi/BLE coprocessor)
 *
 * Build requirements:
 *   - Arduino IDE 2.x or arduino-cli
 *   - WiFiNINA library              (Tools > Manage Libraries > "WiFiNINA")
 *   - ArduinoMqttClient library     (Tools > Manage Libraries > "ArduinoMqttClient")
 *   - Wire library (bundled with the core)
 *
 * The Uno R4 WiFi's coprocessor is an ESP32-S3 (not a NINA-W102) but it
 * presents the same WiFiNINA API surface via the Arduino-vendored shim;
 * the same sketch source compiles unchanged.
 *
 * Topic schema (must match pinscope.html constants):
 *   <base>/<deviceId>/out   board -> host  (line-delimited JSON)
 *   <base>/<deviceId>/in    host  -> board (one command per PUBLISH)
 *
 * The board picks its own device id at boot from a MAC-derived hex string.
 * Override SECRETS below before flashing (network creds and broker host).
 *
 * GPL-3.0-or-later
 */

#include <Arduino.h>
#include <Wire.h>
#include <WiFiNINA.h>
#include <ArduinoMqttClient.h>

// -------- BOARD DETECTION ---------------------------------------------------
// Macros mirror the serial firmware's detection block. Currently the MQTT
// compile matrix only targets Nano 33 IoT (WiFiNINA is incompatible with
// the R4 WiFi's WiFiS3 stack), but the Zephyr (Uno Q) branch is in place
// for forward compatibility.
#if defined(ARDUINO_ARCH_ZEPHYR)
  #define PINSCOPE_BOARD_NAME "Arduino Uno Q"
  // Zephyr-Arduino on Uno Q has been seen to hang silently if we call
  // analogReadResolution() in setup(). Leave ADC at the core default.
  #define PINSCOPE_ADC_BITS   10
  #define PINSCOPE_SET_ADC_RES 0
#elif defined(ARDUINO_SAMD_NANO_33_IOT)
  #define PINSCOPE_BOARD_NAME "Arduino Nano 33 IoT"
  #define PINSCOPE_ADC_BITS   12
  #define PINSCOPE_SET_ADC_RES 1
#else
  #define PINSCOPE_BOARD_NAME "Arduino WiFi"
  #define PINSCOPE_ADC_BITS   10
  #define PINSCOPE_SET_ADC_RES 0
#endif

// -------- INTERRUPT PORTABILITY --------------------------------------------
// digitalPinToInterrupt() returns int on most cores but pin_size_t (unsigned)
// on newer Renesas cores and Zephyr. NOT_AN_INTERRUPT is also not defined on
// every core. Cast to int and define the sentinel ourselves so the same
// "is this pin interrupt-capable" check works across all targets.
#ifndef NOT_AN_INTERRUPT
  #define NOT_AN_INTERRUPT -1
#endif
static inline bool pinscopeHasIrq(uint8_t pin) {
  return (int)digitalPinToInterrupt(pin) != NOT_AN_INTERRUPT;
}

// -------- SECRETS (edit before flashing) -----------------------------------
// You may also move these to a separate "arduino_secrets.h" and include it.
static const char* WIFI_SSID     = "YOUR_WIFI_SSID";
static const char* WIFI_PASS     = "YOUR_WIFI_PASSWORD";
static const char* MQTT_HOST     = "test.mosquitto.org";
static const uint16_t MQTT_PORT  = 1883;
// Topic base; can be overridden by an "in" command later.
static const char* TOPIC_BASE    = "pinscope";

// -------- CONFIG -----------------------------------------------------------
static const uint8_t  NUM_DIGITAL   = 14;
static const uint8_t  NUM_ANALOG    = 6;
static const uint8_t  NUM_VIRTUAL   = 4;
static const uint16_t RX_BUF_SIZE   = 320;
static const uint8_t  I2C_MAX_READ  = 8;
static const uint16_t STATE_PERIOD_DEFAULT = 100;

static const uint8_t MODE_OFF  = 0;
static const uint8_t MODE_IN   = 1;
static const uint8_t MODE_INP  = 2;
static const uint8_t MODE_OUT  = 3;
static const uint8_t MODE_PWM  = 4;
static const uint8_t MODE_FREQ = 5;

static bool isPwmPin(uint8_t p) {
  return p == 3 || p == 5 || p == 6 || p == 9 || p == 10 || p == 11;
}
static bool isReserved(uint8_t p) { return p == 0 || p == 1; }

// -------- NET / MQTT GLOBALS ----------------------------------------------
WiFiClient    wifiClient;
MqttClient    mqttClient(wifiClient);

static char   deviceId[16];        // e.g. "FI005-A1B2"
static char   topicOut[64];
static char   topicIn[64];

// -------- STATE ------------------------------------------------------------
static uint8_t  pinModes[NUM_DIGITAL];
static uint8_t  pwmVals[NUM_DIGITAL];

static char     rxBuf[RX_BUF_SIZE];
static uint16_t rxLen = 0;
static uint32_t lastState = 0;
static uint16_t statePeriod = STATE_PERIOD_DEFAULT;

// Stream output buffer: we accumulate JSON into a line then publish as one
// MQTT PUBLISH. ArduinoMqttClient lets us print into the in-progress
// publish, which lets us avoid allocating a huge intermediate string.
struct PublishContext {
  bool inMessage;
} pubCtx = { false };

// I2C poll slot
struct I2CPoll {
  bool     active;
  uint8_t  addr;
  uint8_t  reg;
  uint8_t  count;
  bool     isSigned;
  uint16_t periodMs;
  uint32_t lastReadMs;
  bool     hasValue;
  int32_t  value;
};
static I2CPoll polls[NUM_VIRTUAL];

// Frequency measurement
static volatile uint32_t pulseCount[NUM_DIGITAL] = { 0 };
static uint32_t          freqHz[NUM_DIGITAL]     = { 0 };
static uint32_t          lastFreqCalc            = 0;
static const uint16_t    FREQ_CALC_PERIOD        = 250;

#define ISR_FOR_PIN(N)  static void isr_pin_##N() { pulseCount[N]++; }
ISR_FOR_PIN(0)  ISR_FOR_PIN(1)  ISR_FOR_PIN(2)  ISR_FOR_PIN(3)
ISR_FOR_PIN(4)  ISR_FOR_PIN(5)  ISR_FOR_PIN(6)  ISR_FOR_PIN(7)
ISR_FOR_PIN(8)  ISR_FOR_PIN(9)  ISR_FOR_PIN(10) ISR_FOR_PIN(11)
ISR_FOR_PIN(12) ISR_FOR_PIN(13)
#undef ISR_FOR_PIN
typedef void (*VoidFn)();
static VoidFn pulseIsrs[NUM_DIGITAL] = {
  isr_pin_0,  isr_pin_1,  isr_pin_2,  isr_pin_3,
  isr_pin_4,  isr_pin_5,  isr_pin_6,  isr_pin_7,
  isr_pin_8,  isr_pin_9,  isr_pin_10, isr_pin_11,
  isr_pin_12, isr_pin_13,
};

// -------- BOARD NAME (compile-time) ---------------------------------------
static const char* boardName() {
  return PINSCOPE_BOARD_NAME;
}

// -------- PUBLISH HELPERS --------------------------------------------------
// Begin a publish to topicOut. Returns true on success. The body is written
// via mqttClient.print(...) and finished by endPublish().
static bool beginPublish() {
  if (!mqttClient.connected()) return false;
  if (!mqttClient.beginMessage(topicOut)) return false;
  pubCtx.inMessage = true;
  return true;
}
static void endPublish() {
  if (!pubCtx.inMessage) return;
  mqttClient.endMessage();
  pubCtx.inMessage = false;
}

// Convenience: print formatted text into the current publish.
// Overloaded inline wrappers around mqttClient.print() so call sites can
// stay terse. We deliberately avoid a function template here because some
// transfer pipelines (HTML sanitizers, certain text editors, web forms)
// silently strip "template <typename T>" thinking the angle brackets are
// HTML tags. Plain overloads sidestep the issue entirely.
static inline void px(const char* v)   { mqttClient.print(v); }
static inline void px(char v)          { mqttClient.print(v); }
static inline void px(int v)           { mqttClient.print(v); }
static inline void px(unsigned int v)  { mqttClient.print(v); }
static inline void px(long v)          { mqttClient.print(v); }
static inline void px(unsigned long v) { mqttClient.print(v); }

static const char* modeName(uint8_t m) {
  switch (m) {
    case MODE_IN:   return "in";
    case MODE_INP:  return "inp";
    case MODE_OUT:  return "out";
    case MODE_PWM:  return "pwm";
    case MODE_FREQ: return "freq";
    default:        return "off";
  }
}

static void sendHello() {
  if (!beginPublish()) return;
  px("{\"t\":\"hello\",\"id\":\""); px(deviceId);
  px("\",\"name\":\""); px(boardName());
  px("\",\"hz\":"); px(1000 / statePeriod);
  px(",\"adcMax\":"); px((int)((1UL << PINSCOPE_ADC_BITS) - 1));
  px("}\n");
  endPublish();
}

static uint8_t readDigitalForReport(uint8_t p) {
  if (pinModes[p] == MODE_OFF) return 0;
  if (pinModes[p] == MODE_PWM) return 0;
  return (uint8_t)digitalRead(p);
}

static void sendState() {
  if (!beginPublish()) return;
  px("{\"t\":\"state\",\"d\":[");
  for (uint8_t p = 0; p < NUM_DIGITAL; p++) {
    if (p) px(',');
    px((int)readDigitalForReport(p));
  }
  px("],\"a\":[");
  for (uint8_t a = 0; a < NUM_ANALOG; a++) {
    if (a) px(',');
    px(analogRead(A0 + a));
  }
  px("],\"m\":[");
  for (uint8_t p = 0; p < NUM_DIGITAL; p++) {
    if (p) px(',');
    px('"'); px(modeName(pinModes[p])); px('"');
  }
  px("],\"v\":[");
  for (uint8_t i = 0; i < NUM_VIRTUAL; i++) {
    if (i) px(',');
    if (polls[i].active && polls[i].hasValue) px(polls[i].value);
    else                                      px("null");
  }
  px("],\"f\":[");
  for (uint8_t p = 0; p < NUM_DIGITAL; p++) {
    if (p) px(',');
    if (pinModes[p] == MODE_FREQ) px((unsigned long)freqHz[p]);
    else                          px(-1);
  }
  px("]}\n");
  endPublish();
}

static void sendAck(const char* cmd) {
  if (!beginPublish()) return;
  px("{\"t\":\"ack\",\"cmd\":\""); px(cmd); px("\"}\n");
  endPublish();
}

static void sendErr(const char* msg) {
  if (!beginPublish()) return;
  px("{\"t\":\"err\",\"msg\":\""); px(msg); px("\"}\n");
  endPublish();
}

// -------- TINY JSON SCANNER (same as other firmwares) ----------------------
// A key must be followed by a colon to disambiguate it from a value that
// happens to be the same string. Without this check, searching for the key
// "mode" in {"cmd":"mode","pin":3,"mode":"in"} would match the value of cmd
// instead of the actual mode key.
static int findField(const char* buf, const char* key) {
  uint8_t klen = strlen(key);
  uint16_t len = strlen(buf);
  for (uint16_t i = 0; i + klen + 2 < len; i++) {
    if (buf[i] != '"') continue;
    if (strncmp(buf + i + 1, key, klen) != 0) continue;
    if (buf[i + 1 + klen] != '"') continue;
    uint16_t j = i + 2 + klen;
    while (j < len && buf[j] == ' ') j++;
    if (j >= len || buf[j] != ':') continue;  // it's a value, not a key
    j++;  // skip colon
    while (j < len && buf[j] == ' ') j++;
    return (int)j;
  }
  return -1;
}
static bool readIntVal(const char* buf, const char* key, int* out) {
  int p = findField(buf, key);
  if (p < 0) return false;
  int sign = 1;
  if (buf[p] == '-') { sign = -1; p++; }
  if (!(buf[p] >= '0' && buf[p] <= '9')) return false;
  long v = 0;
  while (buf[p] >= '0' && buf[p] <= '9') { v = v * 10 + (buf[p] - '0'); p++; }
  *out = (int)(v * sign);
  return true;
}
static bool readBoolVal(const char* buf, const char* key, bool* out) {
  int p = findField(buf, key);
  if (p < 0) return false;
  if (!strncmp(buf + p, "true",  4)) { *out = true;  return true; }
  if (!strncmp(buf + p, "false", 5)) { *out = false; return true; }
  return false;
}
static bool readStringVal(const char* buf, const char* key, char* out, uint8_t outsz) {
  int p = findField(buf, key);
  if (p < 0 || buf[p] != '"') return false;
  p++;
  uint8_t n = 0;
  while (buf[p] && buf[p] != '"' && n + 1 < outsz) out[n++] = buf[p++];
  out[n] = 0;
  return true;
}
static uint8_t readIntArray(const char* buf, const char* key, int* out, uint8_t outsz) {
  int p = findField(buf, key);
  if (p < 0 || buf[p] != '[') return 0;
  p++;
  uint8_t n = 0;
  uint16_t len = strlen(buf);
  while (p < (int)len && buf[p] != ']' && n < outsz) {
    while (p < (int)len && (buf[p] == ' ' || buf[p] == ',')) p++;
    if (buf[p] == ']') break;
    int sign = 1;
    if (buf[p] == '-') { sign = -1; p++; }
    if (!(buf[p] >= '0' && buf[p] <= '9')) break;
    long v = 0;
    while (buf[p] >= '0' && buf[p] <= '9') { v = v * 10 + (buf[p] - '0'); p++; }
    out[n++] = (int)(v * sign);
  }
  return n;
}

// -------- PIN MODE APPLICATION ---------------------------------------------
static void detachIfFreq(uint8_t pin) {
  if (pinModes[pin] == MODE_FREQ) {
    int irq = (int)digitalPinToInterrupt(pin);
    if (irq != NOT_AN_INTERRUPT) detachInterrupt(irq);
    noInterrupts();
    pulseCount[pin] = 0;
    interrupts();
    freqHz[pin] = 0;
  }
}

static void applyMode(uint8_t pin, const char* mode) {
  if (pin >= NUM_DIGITAL) { sendErr("bad pin"); return; }
  if (isReserved(pin))    { sendErr("pin reserved"); return; }
  uint8_t code = MODE_OFF;
  if      (!strcmp(mode, "off"))  code = MODE_OFF;
  else if (!strcmp(mode, "in"))   code = MODE_IN;
  else if (!strcmp(mode, "inp"))  code = MODE_INP;
  else if (!strcmp(mode, "out"))  code = MODE_OUT;
  else if (!strcmp(mode, "pwm"))  { if (!isPwmPin(pin)) { sendErr("not pwm pin"); return; } code = MODE_PWM; }
  else if (!strcmp(mode, "freq")) {
    if (!pinscopeHasIrq(pin)) { sendErr("no interrupt on pin"); return; }
    code = MODE_FREQ;
  }
  else { sendErr("bad mode"); return; }

  detachIfFreq(pin);
  pinModes[pin] = code;
  switch (code) {
    case MODE_OFF:  pinMode(pin, INPUT);        break;
    case MODE_IN:   pinMode(pin, INPUT);        break;
    case MODE_INP:  pinMode(pin, INPUT_PULLUP); break;
    case MODE_OUT:  pinMode(pin, OUTPUT); digitalWrite(pin, LOW); break;
    case MODE_PWM:  pinMode(pin, OUTPUT); analogWrite(pin, pwmVals[pin]); break;
    case MODE_FREQ:
      pinMode(pin, INPUT);
      noInterrupts();
      pulseCount[pin] = 0;
      interrupts();
      attachInterrupt(digitalPinToInterrupt(pin), pulseIsrs[pin], RISING);
      break;
  }
  sendAck("mode");
}

// -------- I2C --------------------------------------------------------------
static bool i2cReadBytes(uint8_t addr, uint8_t reg, uint8_t count, uint8_t* out) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom((int)addr, (int)count);
  uint8_t got = 0;
  while (Wire.available() && got < count) out[got++] = Wire.read();
  return got == count;
}
static int32_t assembleBytes(const uint8_t* bytes, uint8_t count, bool isSigned) {
  int32_t v = 0;
  for (uint8_t i = 0; i < count; i++) v = (v << 8) | bytes[i];
  if (isSigned && count > 0 && (bytes[0] & 0x80) && count < 4) {
    int32_t mask = ~((int32_t)0) << (count * 8);
    v |= mask;
  }
  return v;
}

static void doI2CScan() {
  if (!beginPublish()) return;
  px("{\"t\":\"i2c\",\"op\":\"scan\",\"addrs\":[");
  bool first = true;
  for (uint8_t addr = 1; addr < 0x78; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      if (!first) px(',');
      px((int)addr);
      first = false;
    }
  }
  px("]}\n");
  endPublish();
}

static void doI2CRead(uint8_t addr, uint8_t reg, uint8_t count) {
  if (count == 0 || count > I2C_MAX_READ) { sendErr("bad i2c count"); return; }
  uint8_t buf[I2C_MAX_READ];
  if (!i2cReadBytes(addr, reg, count, buf)) { sendErr("i2c read failed"); return; }
  if (!beginPublish()) return;
  px("{\"t\":\"i2c\",\"op\":\"read\",\"addr\":");
  px((int)addr); px(",\"reg\":");
  px((int)reg);  px(",\"data\":[");
  for (uint8_t i = 0; i < count; i++) {
    if (i) px(',');
    px((int)buf[i]);
  }
  px("]}\n");
  endPublish();
}

static void doI2CWrite(uint8_t addr, uint8_t reg, const int* data, uint8_t count) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  for (uint8_t i = 0; i < count; i++) Wire.write((uint8_t)(data[i] & 0xFF));
  if (Wire.endTransmission() != 0) { sendErr("i2c write failed"); return; }
  sendAck("i2c");
}

static void handleI2C(const char* buf) {
  char op[10];
  if (!readStringVal(buf, "op", op, sizeof(op))) { sendErr("no i2c op"); return; }
  if (!strcmp(op, "scan")) { doI2CScan(); return; }
  int addr, reg, count;
  if (!strcmp(op, "read")) {
    if (!readIntVal(buf, "addr", &addr) || !readIntVal(buf, "reg", &reg) || !readIntVal(buf, "count", &count)) {
      sendErr("i2c read missing args"); return;
    }
    doI2CRead((uint8_t)addr, (uint8_t)reg, (uint8_t)count);
    return;
  }
  if (!strcmp(op, "write")) {
    if (!readIntVal(buf, "addr", &addr) || !readIntVal(buf, "reg", &reg)) {
      sendErr("i2c write missing args"); return;
    }
    int data[16];
    uint8_t n = readIntArray(buf, "data", data, 16);
    if (n == 0) { sendErr("i2c write no data"); return; }
    doI2CWrite((uint8_t)addr, (uint8_t)reg, data, n);
    return;
  }
  if (!strcmp(op, "poll")) {
    int slot, hz;
    bool isSigned = false;
    if (!readIntVal(buf, "slot", &slot) || !readIntVal(buf, "addr",  &addr) ||
        !readIntVal(buf, "reg",  &reg)  || !readIntVal(buf, "count", &count) ||
        !readIntVal(buf, "hz",   &hz)) {
      sendErr("i2c poll missing args"); return;
    }
    readBoolVal(buf, "signed", &isSigned);
    if (slot < 0 || slot >= NUM_VIRTUAL) { sendErr("bad slot"); return; }
    if (count <= 0 || count > I2C_MAX_READ) { sendErr("bad count"); return; }
    if (hz <= 0) hz = 1;
    if (hz > 50) hz = 50;
    polls[slot].active     = true;
    polls[slot].addr       = (uint8_t)addr;
    polls[slot].reg        = (uint8_t)reg;
    polls[slot].count      = (uint8_t)count;
    polls[slot].isSigned   = isSigned;
    polls[slot].periodMs   = (uint16_t)(1000 / hz);
    polls[slot].lastReadMs = 0;
    polls[slot].hasValue   = false;
    polls[slot].value      = 0;
    sendAck("i2c");
    return;
  }
  if (!strcmp(op, "stoppoll")) {
    int slot;
    if (!readIntVal(buf, "slot", &slot)) { sendErr("no slot"); return; }
    if (slot < 0 || slot >= NUM_VIRTUAL) { sendErr("bad slot"); return; }
    polls[slot].active   = false;
    polls[slot].hasValue = false;
    sendAck("i2c");
    return;
  }
  sendErr("bad i2c op");
}

// -------- ONBOARD RGB LED -------------------------------------------------
// Mirror of the serial firmware's onboard LED handler. See pinscope.ino
// for the full design notes (active-low, lazy pinMode init, Zephyr gate).
static void handleLed(const char* buf) {
#if defined(ARDUINO_ARCH_ZEPHYR)
  int r, g, b;
  if (!readIntVal(buf, "r", &r) || !readIntVal(buf, "g", &g) || !readIntVal(buf, "b", &b)) {
    sendErr("missing r/g/b"); return;
  }
  if (r < 0) r = 0; if (r > 255) r = 255;
  if (g < 0) g = 0; if (g > 255) g = 255;
  if (b < 0) b = 0; if (b > 255) b = 255;
  static bool led3Ready = false;
  if (!led3Ready) {
    pinMode(LED3_R, OUTPUT);
    pinMode(LED3_G, OUTPUT);
    pinMode(LED3_B, OUTPUT);
    led3Ready = true;
  }
  // Zephyr-Arduino's analogWrite() handles the LED's active-low polarity
  // internally; write duty directly (0 = off, 255 = full brightness).
  analogWrite(LED3_R, r);
  analogWrite(LED3_G, g);
  analogWrite(LED3_B, b);
  sendAck("led");
#else
  (void)buf;
  sendErr("led not supported on this board");
#endif
}

static void handleLine(char* buf) {
  char cmd[10];
  if (!readStringVal(buf, "cmd", cmd, sizeof(cmd))) { sendErr("no cmd"); return; }
  if (!strcmp(cmd, "hello")) { sendHello(); sendState(); return; }
  if (!strcmp(cmd, "poll"))  { sendState(); return; }
  if (!strcmp(cmd, "mode")) {
    int pin; char m[8];
    if (!readIntVal(buf, "pin", &pin) || !readStringVal(buf, "mode", m, sizeof(m))) {
      sendErr("missing pin/mode"); return;
    }
    applyMode((uint8_t)pin, m);
    return;
  }
  if (!strcmp(cmd, "set")) {
    int pin, val;
    if (!readIntVal(buf, "pin", &pin) || !readIntVal(buf, "val", &val)) {
      sendErr("missing pin/val"); return;
    }
    if (pin < 0 || pin >= NUM_DIGITAL) { sendErr("bad pin");        return; }
    if (isReserved(pin))               { sendErr("pin reserved");   return; }
    if (pinModes[pin] != MODE_OUT)     { sendErr("pin not output"); return; }
    digitalWrite(pin, val ? HIGH : LOW);
    sendAck("set");
    return;
  }
  if (!strcmp(cmd, "pwm")) {
    int pin, val;
    if (!readIntVal(buf, "pin", &pin) || !readIntVal(buf, "val", &val)) {
      sendErr("missing pin/val"); return;
    }
    if (pin < 0 || pin >= NUM_DIGITAL) { sendErr("bad pin");     return; }
    if (pinModes[pin] != MODE_PWM)     { sendErr("pin not PWM"); return; }
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    pwmVals[pin] = (uint8_t)val;
    analogWrite(pin, pwmVals[pin]);
    sendAck("pwm");
    return;
  }
  if (!strcmp(cmd, "hz")) {
    int val;
    if (!readIntVal(buf, "val", &val)) { sendErr("missing val"); return; }
    if (val < 1) val = 1;
    if (val > 50) val = 50;
    statePeriod = (uint16_t)(1000 / val);
    sendAck("hz");
    return;
  }
  if (!strcmp(cmd, "i2c")) { handleI2C(buf); return; }
  if (!strcmp(cmd, "led")) { handleLed(buf); return; }
  sendErr("unknown cmd");
}

// -------- MQTT MESSAGE CALLBACK --------------------------------------------
// Called by ArduinoMqttClient when a PUBLISH on a subscribed topic arrives.
// We assume QoS 0 and a payload that contains one or more newline-terminated
// JSON commands. Each line is dispatched independently.
static void onMqttMessage(int messageSize) {
  while (mqttClient.available()) {
    char c = (char)mqttClient.read();
    if (c == '\n' || c == '\r') {
      if (rxLen > 0) {
        rxBuf[rxLen] = 0;
        handleLine(rxBuf);
        rxLen = 0;
      }
    } else if (rxLen < RX_BUF_SIZE - 1) {
      rxBuf[rxLen++] = c;
    } else {
      rxLen = 0;
      sendErr("rx overflow");
    }
  }
  // If the payload did not end with a newline, flush it as a single line.
  if (rxLen > 0) {
    rxBuf[rxLen] = 0;
    handleLine(rxBuf);
    rxLen = 0;
  }
}

// -------- ID GENERATION ----------------------------------------------------
// Derive a stable, board-unique id from the WiFi MAC (last 2 bytes).
static void buildDeviceId() {
  byte mac[6] = { 0 };
  WiFi.macAddress(mac);
  snprintf(deviceId, sizeof(deviceId), "FI005-%02X%02X", mac[4], mac[5]);
}

// -------- CONNECTION HELPERS ----------------------------------------------
static void wifiConnect() {
  Serial.print("Connecting to WiFi: "); Serial.println(WIFI_SSID);
  while (WiFi.begin(WIFI_SSID, WIFI_PASS) != WL_CONNECTED) {
    Serial.print('.');
    delay(2000);
  }
  Serial.println();
  Serial.print("WiFi connected, IP: "); Serial.println(WiFi.localIP());
}

static void mqttConnect() {
  Serial.print("Connecting to MQTT: ");
  Serial.print(MQTT_HOST); Serial.print(':'); Serial.println(MQTT_PORT);
  mqttClient.setId(deviceId);
  while (!mqttClient.connect(MQTT_HOST, MQTT_PORT)) {
    Serial.print("MQTT connect failed, error: ");
    Serial.println(mqttClient.connectError());
    delay(3000);
  }
  Serial.println("MQTT connected");
  // Subscribe to our inbound command topic
  mqttClient.subscribe(topicIn);
  Serial.print("Subscribed to: "); Serial.println(topicIn);
}

// -------- ARDUINO ENTRY POINTS --------------------------------------------
void setup() {
  Serial.begin(115200);
  uint32_t deadline = millis() + 2000;
  while (!Serial && millis() < deadline) { /* don't block if no monitor */ }

  for (uint8_t i = 0; i < NUM_DIGITAL; i++) {
    pinModes[i] = 0;
    pwmVals[i]  = 0;
  }
  for (uint8_t i = 0; i < NUM_VIRTUAL; i++) polls[i].active = false;

  // ADC resolution on 12-bit-capable boards. Skipped on Zephyr (see
  // board-detection block at top of file for why).
#if PINSCOPE_SET_ADC_RES
  analogReadResolution(PINSCOPE_ADC_BITS);
#endif

  Wire.begin();

  // Bring up WiFi
  if (WiFi.status() == WL_NO_MODULE) {
    Serial.println("WiFi module not detected; check WiFiNINA install");
    while (1) { delay(1000); }
  }
  wifiConnect();

  // Derive device id and topic names
  buildDeviceId();
  snprintf(topicOut, sizeof(topicOut), "%s/%s/out", TOPIC_BASE, deviceId);
  snprintf(topicIn,  sizeof(topicIn),  "%s/%s/in",  TOPIC_BASE, deviceId);
  Serial.print("Device id: "); Serial.println(deviceId);

  // Bring up MQTT
  mqttClient.onMessage(onMqttMessage);
  mqttConnect();
  sendHello();
}

void loop() {
  // Keep WiFi + MQTT alive
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi dropped; reconnecting");
    wifiConnect();
  }
  if (!mqttClient.connected()) {
    Serial.println("MQTT dropped; reconnecting");
    mqttConnect();
    sendHello();
  }
  mqttClient.poll();

  uint32_t now = millis();

  // I2C polls
  for (uint8_t i = 0; i < NUM_VIRTUAL; i++) {
    if (!polls[i].active) continue;
    if (now - polls[i].lastReadMs < polls[i].periodMs) continue;
    polls[i].lastReadMs = now;
    uint8_t buf[I2C_MAX_READ];
    if (i2cReadBytes(polls[i].addr, polls[i].reg, polls[i].count, buf)) {
      polls[i].value    = assembleBytes(buf, polls[i].count, polls[i].isSigned);
      polls[i].hasValue = true;
    } else {
      polls[i].hasValue = false;
    }
  }

  // Frequency calc
  if (now - lastFreqCalc >= FREQ_CALC_PERIOD) {
    uint32_t elapsed = now - lastFreqCalc;
    lastFreqCalc = now;
    for (uint8_t p = 0; p < NUM_DIGITAL; p++) {
      if (pinModes[p] != MODE_FREQ) { freqHz[p] = 0; continue; }
      noInterrupts();
      uint32_t c = pulseCount[p];
      pulseCount[p] = 0;
      interrupts();
      freqHz[p] = (c * 1000UL) / elapsed;
    }
  }

  // Periodic state push
  if (now - lastState >= statePeriod) {
    lastState = now;
    sendState();
  }
}
