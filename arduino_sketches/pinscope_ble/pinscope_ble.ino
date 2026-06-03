/*
 * pinscope_ble.ino
 *
 * BLE-capable firmware for PINSCOPE (Field Instrument 005). Uses the
 * Arduino BLE service definition to expose the same line-delimited JSON
 * wire protocol that pinscope.ino speaks over USB Serial. Verified on:
 *
 *   - Arduino Uno R4 WiFi   (Renesas RA4M1 + ESP32-S3 BLE co-processor)
 *   - Arduino Nano 33 IoT   (SAMD21 + NINA-W102 BLE co-processor)
 *   - Arduino Uno Q         (STM32U585 native BLE; experimental under
 *                            Zephyr-based arduino:zephyr:unoq core)
 *
 * Build requirements:
 *   - Arduino IDE 2.x or arduino-cli
 *   - ArduinoBLE library (Tools > Manage Libraries > "ArduinoBLE")
 *   - Wire library (part of the core; bundled with each board package)
 *
 * BLE service schema (committed; must match pinscope.html constants):
 *   Service     7e2bf001-9d27-4e96-9c9f-1f4b8a0c5e6d  PinScope I/O console
 *   Notify char 7e2bf002-9d27-4e96-9c9f-1f4b8a0c5e6d  board -> host
 *   Write char  7e2bf003-9d27-4e96-9c9f-1f4b8a0c5e6d  host  -> board
 *
 * Both characteristics carry line-delimited JSON. Either side may chunk a
 * line into multiple ATT writes; the receiver buffers until '\n'. The
 * implementation here uses 200 bytes per write to stay safely under any
 * default ATT MTU on any of the three target boards.
 *
 * The wire protocol is identical to pinscope.ino. See README.md for the
 * full reference.
 *
 * GPL-3.0-or-later
 */

#include <Arduino.h>
#include <Wire.h>
#include <ArduinoBLE.h>

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

// -------- CONFIG -----------------------------------------------------------
static const uint8_t  NUM_DIGITAL   = 14;
static const uint8_t  NUM_ANALOG    = 6;
static const uint8_t  NUM_VIRTUAL   = 4;
static const uint16_t RX_BUF_SIZE   = 320;       // larger than serial; BLE can deliver bursts
static const uint8_t  I2C_MAX_READ  = 8;
static const uint16_t BLE_MTU_SAFE  = 200;       // bytes per notify chunk
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

// -------- BLE OBJECTS ------------------------------------------------------
const char* PS_SERVICE_UUID = "7e2bf001-9d27-4e96-9c9f-1f4b8a0c5e6d";
const char* PS_NOTIFY_UUID  = "7e2bf002-9d27-4e96-9c9f-1f4b8a0c5e6d";
const char* PS_WRITE_UUID   = "7e2bf003-9d27-4e96-9c9f-1f4b8a0c5e6d";

BLEService            psService(PS_SERVICE_UUID);
// Maximum length 240 is widely safe; the actual ATT_MTU may be larger but the
// declared characteristic size caps it. We always chunk writes to BLE_MTU_SAFE.
BLECharacteristic     psNotify(PS_NOTIFY_UUID, BLENotify, 240);
BLECharacteristic     psWrite (PS_WRITE_UUID,  BLEWrite | BLEWriteWithoutResponse, 240);

// -------- STATE ------------------------------------------------------------
static uint8_t  pinModes[NUM_DIGITAL];
static uint8_t  pwmVals[NUM_DIGITAL];

static char     rxBuf[RX_BUF_SIZE];
static uint16_t rxLen = 0;
static uint32_t lastState = 0;
static uint16_t statePeriod = STATE_PERIOD_DEFAULT;
static bool     centralConnected = false;

// Stream output buffer (we accumulate then flush to BLE in MTU-safe chunks)
static char     txAccum[256];
static uint16_t txAccumLen = 0;

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

// -------- OUTPUT (BLE) -----------------------------------------------------
// Flush the accumulated tx buffer in BLE_MTU_SAFE byte chunks via Notify.
static void txFlush() {
  if (!centralConnected || txAccumLen == 0) { txAccumLen = 0; return; }
  uint16_t pos = 0;
  while (pos < txAccumLen) {
    uint16_t chunk = (txAccumLen - pos > BLE_MTU_SAFE) ? BLE_MTU_SAFE : (txAccumLen - pos);
    psNotify.writeValue((const uint8_t*)(txAccum + pos), chunk);
    pos += chunk;
  }
  txAccumLen = 0;
}

// Append to tx buffer, auto-flushing when full or on newline.
static void txWrite(const char* s) {
  while (*s) {
    if (txAccumLen >= sizeof(txAccum) - 1) txFlush();
    txAccum[txAccumLen++] = *s;
    if (*s == '\n') txFlush();
    s++;
  }
}
static void txWriteInt(int32_t v) {
  char buf[16];
  itoa(v, buf, 10);
  txWrite(buf);
}
static void txWriteUInt(uint32_t v) {
  char buf[16];
  ultoa(v, buf, 10);
  txWrite(buf);
}
static void txWriteChar(char c) {
  if (txAccumLen >= sizeof(txAccum) - 1) txFlush();
  txAccum[txAccumLen++] = c;
  if (c == '\n') txFlush();
}

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
  uint16_t id = (uint16_t)(micros() & 0xFFFF);
  txWrite("{\"t\":\"hello\",\"id\":\"FI005-");
  for (int8_t shift = 12; shift >= 0; shift -= 4) {
    uint8_t nib = (id >> shift) & 0xF;
    txWriteChar((char)(nib < 10 ? '0' + nib : 'A' + nib - 10));
  }
  txWrite("\",\"name\":\"");
  // Identify which board family the firmware was built for.
#if defined(ARDUINO_UNOR4_WIFI)
  txWrite("Arduino Uno R4 WiFi");
#elif defined(ARDUINO_SAMD_NANO_33_IOT)
  txWrite("Arduino Nano 33 IoT");
#elif defined(ARDUINO_UNO_Q) || defined(ARDUINO_ARCH_STM32U5)
  txWrite("Arduino Uno Q");
#else
  txWrite("Arduino BLE");
#endif
  txWrite("\",\"hz\":");
  txWriteInt(1000 / statePeriod);
  txWrite("}\n");
}

static uint8_t readDigitalForReport(uint8_t p) {
  if (pinModes[p] == MODE_OFF) return 0;
  if (pinModes[p] == MODE_PWM) return 0;
  return (uint8_t)digitalRead(p);
}

static void sendState() {
  txWrite("{\"t\":\"state\",\"d\":[");
  for (uint8_t p = 0; p < NUM_DIGITAL; p++) {
    if (p) txWriteChar(',');
    txWriteInt(readDigitalForReport(p));
  }
  txWrite("],\"a\":[");
  for (uint8_t a = 0; a < NUM_ANALOG; a++) {
    if (a) txWriteChar(',');
    txWriteInt(analogRead(A0 + a));
  }
  txWrite("],\"m\":[");
  for (uint8_t p = 0; p < NUM_DIGITAL; p++) {
    if (p) txWriteChar(',');
    txWriteChar('"'); txWrite(modeName(pinModes[p])); txWriteChar('"');
  }
  txWrite("],\"v\":[");
  for (uint8_t i = 0; i < NUM_VIRTUAL; i++) {
    if (i) txWriteChar(',');
    if (polls[i].active && polls[i].hasValue) txWriteInt(polls[i].value);
    else                                      txWrite("null");
  }
  txWrite("],\"f\":[");
  for (uint8_t p = 0; p < NUM_DIGITAL; p++) {
    if (p) txWriteChar(',');
    if (pinModes[p] == MODE_FREQ) txWriteUInt(freqHz[p]);
    else                          txWrite("-1");
  }
  txWrite("]}\n");
}

static void sendAck(const char* cmd) {
  txWrite("{\"t\":\"ack\",\"cmd\":\""); txWrite(cmd); txWrite("\"}\n");
}

static void sendErr(const char* msg) {
  txWrite("{\"t\":\"err\",\"msg\":\""); txWrite(msg); txWrite("\"}\n");
}

// -------- TINY JSON SCANNER (same as serial firmware) ----------------------
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
  if (!strncmp(buf + p, "true",  4))  { *out = true;  return true; }
  if (!strncmp(buf + p, "false", 5))  { *out = false; return true; }
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
  txWrite("{\"t\":\"i2c\",\"op\":\"scan\",\"addrs\":[");
  bool first = true;
  for (uint8_t addr = 1; addr < 0x78; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      if (!first) txWriteChar(',');
      txWriteInt(addr);
      first = false;
    }
  }
  txWrite("]}\n");
}
static void doI2CRead(uint8_t addr, uint8_t reg, uint8_t count) {
  if (count == 0 || count > I2C_MAX_READ) { sendErr("bad i2c count"); return; }
  uint8_t buf[I2C_MAX_READ];
  if (!i2cReadBytes(addr, reg, count, buf)) { sendErr("i2c read failed"); return; }
  txWrite("{\"t\":\"i2c\",\"op\":\"read\",\"addr\":");
  txWriteInt(addr); txWrite(",\"reg\":");
  txWriteInt(reg);  txWrite(",\"data\":[");
  for (uint8_t i = 0; i < count; i++) {
    if (i) txWriteChar(',');
    txWriteInt(buf[i]);
  }
  txWrite("]}\n");
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
  sendErr("unknown cmd");
}

// -------- BLE EVENTS -------------------------------------------------------
static void onConnect(BLEDevice central) {
  centralConnected = true;
  // Reset the rx line buffer so a fresh session doesn't carry junk
  rxLen = 0;
  // Greet the central with hello + state immediately
  sendHello();
  sendState();
}
static void onDisconnect(BLEDevice central) {
  centralConnected = false;
  // Drain any pending tx (noop without a connection, but clear the buffer)
  txAccumLen = 0;
}

// Called when the write characteristic receives data from the central.
// Accumulate into rxBuf, dispatch complete lines on '\n'.
static void onRxWritten(BLEDevice central, BLECharacteristic ch) {
  uint16_t got = ch.valueLength();
  const uint8_t* data = ch.value();
  for (uint16_t i = 0; i < got; i++) {
    char c = (char)data[i];
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
}

// -------- SETUP / LOOP -----------------------------------------------------
void setup() {
  // Optional: open a serial monitor for debugging during bring-up.
  Serial.begin(115200);
  uint32_t serialDeadline = millis() + 1000;
  while (!Serial && millis() < serialDeadline) { /* don't block if no monitor */ }

  for (uint8_t i = 0; i < NUM_DIGITAL; i++) {
    pinModes[i] = 0;
    pwmVals[i]  = 0;
  }
  for (uint8_t i = 0; i < NUM_VIRTUAL; i++) polls[i].active = false;
  Wire.begin();

  if (!BLE.begin()) {
    Serial.println("BLE.begin() failed");
    while (1) { delay(1000); }
  }

  // Local name and advertised service. The local name is what the device
  // chooser shows in the browser; the advertised service UUID is what the
  // browser filters on.
  BLE.setLocalName("PinScope");
  BLE.setDeviceName("PinScope");
  BLE.setAdvertisedService(psService);

  psService.addCharacteristic(psNotify);
  psService.addCharacteristic(psWrite);
  BLE.addService(psService);

  psWrite.setEventHandler(BLEWritten, onRxWritten);
  BLE.setEventHandler(BLEConnected,    onConnect);
  BLE.setEventHandler(BLEDisconnected, onDisconnect);

  BLE.advertise();
  Serial.println("PinScope BLE advertising as 'PinScope'");
}

void loop() {
  BLE.poll();

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

  // Periodic state push, only while a central is subscribed
  if (centralConnected && now - lastState >= statePeriod) {
    lastState = now;
    sendState();
  }
}
