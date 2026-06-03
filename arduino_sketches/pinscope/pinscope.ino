/*
 * pinscope.ino
 *
 * Reference firmware for PINSCOPE (Field Instrument 005), the Arduino Uno Q
 * I/O console. Implements the PinScope line-delimited JSON wire protocol
 * over Serial at 115200 baud.
 *
 * Compatible with the classic Uno API surface, so it also works on a stock
 * Arduino Uno R3 and most ATmega328P / SAMD / STM32 cores. Requires Wire.h
 * (part of the Arduino core, no external library needed) for I2C support.
 *
 * Wire protocol (one JSON object per line, both directions):
 *
 *   Host -> board:
 *     {"cmd":"hello"}
 *     {"cmd":"poll"}
 *     {"cmd":"mode","pin":N,"mode":"off|in|inp|out|pwm|freq"}
 *     {"cmd":"set","pin":N,"val":0|1}
 *     {"cmd":"pwm","pin":N,"val":0..255}
 *     {"cmd":"hz","val":N}                              <- set state push rate (1..50 Hz)
 *     {"cmd":"i2c","op":"scan"}
 *     {"cmd":"i2c","op":"read","addr":N,"reg":N,"count":N}
 *     {"cmd":"i2c","op":"write","addr":N,"reg":N,"data":[N,N,...]}
 *     {"cmd":"i2c","op":"poll","slot":N,"addr":N,"reg":N,"count":N,"hz":N,"signed":bool}
 *     {"cmd":"i2c","op":"stoppoll","slot":N}
 *
 *   Board -> host:
 *     {"t":"hello","id":"FI005-XXXX","name":"Arduino Uno Q","hz":N}
 *     {"t":"state","d":[14 ints],"a":[6 ints],"m":[14 strings],"v":[4 ints/null],"f":[14 ints]}
 *     {"t":"ack","cmd":"..."}
 *     {"t":"err","msg":"..."}
 *     {"t":"i2c","op":"scan","addrs":[N,N,...]}
 *     {"t":"i2c","op":"read","addr":N,"reg":N,"data":[N,N,...]}
 *
 * Notes:
 *   - D0 and D1 are reserved (USB serial UART) and are not driven.
 *   - PWM-capable pins on Uno-family: 3, 5, 6, 9, 10, 11.
 *   - FREQ mode requires hardware interrupt support. On classic Uno R3 only
 *     D2 and D3 work. On Uno Q most pins support EXTI so the firmware
 *     attempts attachInterrupt on the requested pin and reports an error
 *     if it returns NOT_AN_INTERRUPT.
 *   - State packets are pushed at the rate set by {"cmd":"hz"}; default 10 Hz.
 *   - Four virtual channels (V0-V3) can poll I2C registers; values appear
 *     in the state packet's "v" array as int32 (or null if inactive).
 *   - The JSON reader is a minimal scanner, not a full parser. It tolerates
 *     extra whitespace and a single nested int array (for i2c write data).
 *
 * GPL-3.0-or-later
 */

#include <Arduino.h>
#include <Wire.h>

// -------- BOARD DETECTION ---------------------------------------------------
// The board-specific defines let the firmware adapt to each MCU's quirks:
//   - ADC bit width (Uno R4, Nano 33 IoT, Uno Q are 12-bit; classic Uno is 10)
//   - Whether we should call analogReadResolution() at boot
//   - A name we report in the hello packet so the browser can label the card
// All three families use the standard Arduino API surface, so attachInterrupt,
// digitalPinToInterrupt, pinMode, analogRead, Wire all behave the same way.
#if defined(ARDUINO_ARCH_ZEPHYR)
  #define PINSCOPE_BOARD_NAME "Arduino Uno Q"
  // The Zephyr-Arduino core on the Uno Q is new and we've seen the firmware
  // hang silently if we try to call analogReadResolution() in setup. Until
  // that's understood, leave ADC resolution at the core's default and
  // advertise 10-bit so the browser's scaling matches whatever the core
  // does. If it turns out reads are actually 12-bit on this core, the bars
  // will just look short, not broken; bump these to 12 and SET_ADC_RES=1.
  #define PINSCOPE_ADC_BITS   10
  #define PINSCOPE_SET_ADC_RES 0
#elif defined(ARDUINO_UNOR4_WIFI) || defined(ARDUINO_UNOR4_MINIMA)
  #define PINSCOPE_BOARD_NAME "Arduino Uno R4"
  #define PINSCOPE_ADC_BITS   12
  #define PINSCOPE_SET_ADC_RES 1
#elif defined(ARDUINO_SAMD_NANO_33_IOT)
  #define PINSCOPE_BOARD_NAME "Arduino Nano 33 IoT"
  #define PINSCOPE_ADC_BITS   12
  #define PINSCOPE_SET_ADC_RES 1
#elif defined(ARDUINO_AVR_UNO) || defined(ARDUINO_ARCH_AVR)
  #define PINSCOPE_BOARD_NAME "Arduino Uno"
  #define PINSCOPE_ADC_BITS   10
  #define PINSCOPE_SET_ADC_RES 0
#else
  // Unknown board: assume the safe defaults (10-bit ADC, don't change).
  #define PINSCOPE_BOARD_NAME "Arduino"
  #define PINSCOPE_ADC_BITS   10
  #define PINSCOPE_SET_ADC_RES 0
#endif

// -------- INTERRUPT PORTABILITY --------------------------------------------
// digitalPinToInterrupt() returns int on most cores (AVR, SAMD, Renesas R4
// older) but pin_size_t (unsigned) on newer Renesas cores and Zephyr. The
// sentinel NOT_AN_INTERRUPT is also not defined on every core. Wrap the
// "is this pin interrupt-capable" check in a helper that works on all of
// them by promoting the return value to int and comparing against -1.
#ifndef NOT_AN_INTERRUPT
  #define NOT_AN_INTERRUPT -1
#endif
static inline bool pinscopeHasIrq(uint8_t pin) {
  return (int)digitalPinToInterrupt(pin) != NOT_AN_INTERRUPT;
}

// -------- CONFIG -----------------------------------------------------------
static const uint32_t BAUD          = 115200;
static const uint8_t  NUM_DIGITAL   = 14;
static const uint8_t  NUM_ANALOG    = 6;
static const uint8_t  NUM_VIRTUAL   = 4;
static const uint8_t  RX_BUF_SIZE   = 160;
static const uint8_t  I2C_MAX_READ  = 8;

static const uint16_t STATE_PERIOD_DEFAULT = 100;   // ms, ~10 Hz

// Mode codes: 0=off, 1=in, 2=inp, 3=out, 4=pwm, 5=freq
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

// -------- STATE ------------------------------------------------------------
static uint8_t  pinModes[NUM_DIGITAL];
static uint8_t  pwmVals[NUM_DIGITAL];

static char     rxBuf[RX_BUF_SIZE];
static uint8_t  rxLen = 0;
static uint32_t lastState = 0;
static uint16_t statePeriod = STATE_PERIOD_DEFAULT;

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

// Frequency measurement: one volatile counter per digital pin. Each pin's
// ISR is a tiny wrapper that increments its own counter; the main loop
// converts counts to Hz periodically.
static volatile uint32_t pulseCount[NUM_DIGITAL] = { 0 };
static uint32_t          freqHz[NUM_DIGITAL]     = { 0 };
static uint32_t          lastFreqCalc            = 0;
static const uint16_t    FREQ_CALC_PERIOD        = 250;  // ms

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

// -------- OUTPUT HELPERS ---------------------------------------------------
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
  Serial.print(F("{\"t\":\"hello\",\"id\":\"FI005-"));
  for (int8_t shift = 12; shift >= 0; shift -= 4) {
    uint8_t nib = (id >> shift) & 0xF;
    Serial.print((char)(nib < 10 ? '0' + nib : 'A' + nib - 10));
  }
  Serial.print(F("\",\"name\":\""));
  Serial.print(F(PINSCOPE_BOARD_NAME));
  Serial.print(F("\",\"hz\":"));
  Serial.print(1000 / statePeriod);
  Serial.print(F(",\"adcMax\":"));
  Serial.print((uint16_t)((1UL << PINSCOPE_ADC_BITS) - 1));
  Serial.println(F("}"));
}

static uint8_t readDigitalForReport(uint8_t p) {
  if (pinModes[p] == MODE_OFF)  return 0;
  if (pinModes[p] == MODE_PWM)  return 0;
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
  Serial.print(F("],\"v\":["));
  for (uint8_t i = 0; i < NUM_VIRTUAL; i++) {
    if (i) Serial.print(',');
    if (polls[i].active && polls[i].hasValue) Serial.print(polls[i].value);
    else                                      Serial.print(F("null"));
  }
  Serial.print(F("],\"f\":["));
  for (uint8_t p = 0; p < NUM_DIGITAL; p++) {
    if (p) Serial.print(',');
    if (pinModes[p] == MODE_FREQ) Serial.print(freqHz[p]);
    else                          Serial.print(-1);
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
// Find the position of a key's VALUE in a JSON object string. Returns the
// index of the first character of the value, or -1 if the key isn't present.
//
// Critical: a key must be followed by a colon (after optional whitespace).
// Without that check, the scanner would match the literal substring "<key>"
// anywhere in the buffer including inside values. For example, searching
// for the key "mode" in {"cmd":"mode","pin":3,"mode":"in"} would otherwise
// match the value of cmd at position 7 instead of the actual mode key at
// position 22. The colon disambiguates key from value.
static int findField(const char* buf, const char* key) {
  uint8_t klen = strlen(key);
  uint8_t len  = strlen(buf);
  for (uint8_t i = 0; i + klen + 2 < len; i++) {
    if (buf[i] != '"') continue;
    if (strncmp(buf + i + 1, key, klen) != 0) continue;
    if (buf[i + 1 + klen] != '"') continue;
    // Verify this is a key (followed by ':') and not a value
    uint8_t j = i + 2 + klen;
    while (j < len && buf[j] == ' ') j++;
    if (j >= len || buf[j] != ':') continue;  // not a key, keep searching
    j++;  // skip the colon
    while (j < len && buf[j] == ' ') j++;
    return j;
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
  uint8_t len = strlen(buf);
  while (p < len && buf[p] != ']' && n < outsz) {
    while (p < len && (buf[p] == ' ' || buf[p] == ',')) p++;
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

// -------- I2C HELPERS ------------------------------------------------------
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
  Serial.print(F("{\"t\":\"i2c\",\"op\":\"scan\",\"addrs\":["));
  bool first = true;
  for (uint8_t addr = 1; addr < 0x78; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      if (!first) Serial.print(',');
      Serial.print(addr);
      first = false;
    }
  }
  Serial.println(F("]}"));
}

static void doI2CRead(uint8_t addr, uint8_t reg, uint8_t count) {
  if (count == 0 || count > I2C_MAX_READ) { sendErr("bad i2c count"); return; }
  uint8_t buf[I2C_MAX_READ];
  if (!i2cReadBytes(addr, reg, count, buf)) { sendErr("i2c read failed"); return; }
  Serial.print(F("{\"t\":\"i2c\",\"op\":\"read\",\"addr\":"));
  Serial.print(addr);
  Serial.print(F(",\"reg\":"));
  Serial.print(reg);
  Serial.print(F(",\"data\":["));
  for (uint8_t i = 0; i < count; i++) {
    if (i) Serial.print(',');
    Serial.print(buf[i]);
  }
  Serial.println(F("]}"));
}

static void doI2CWrite(uint8_t addr, uint8_t reg, const int* data, uint8_t count) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  for (uint8_t i = 0; i < count; i++) Wire.write((uint8_t)(data[i] & 0xFF));
  if (Wire.endTransmission() != 0) { sendErr("i2c write failed"); return; }
  sendAck("i2c");
}

// -------- COMMAND DISPATCH -------------------------------------------------
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

// -------- ARDUINO ENTRY POINTS --------------------------------------------
void setup() {
  Serial.begin(BAUD);
  uint32_t deadline = millis() + 2000;
  while (!Serial && millis() < deadline) { /* wait */ }

  // Boot phase markers so silent hangs in setup() are visible in the
  // serial monitor. If you only see "boot:start" and nothing after, the
  // hang is between then and the next marker.
  Serial.println(F("{\"t\":\"boot\",\"phase\":\"start\"}"));

  for (uint8_t i = 0; i < NUM_DIGITAL; i++) {
    pinModes[i] = 0;
    pwmVals[i]  = 0;
  }
  for (uint8_t i = 0; i < NUM_VIRTUAL; i++) polls[i].active = false;

  // ADC resolution on 12-bit-capable boards. Classic AVR ignores this.
  // Skipped on Zephyr until we understand whether the call hangs.
#if PINSCOPE_SET_ADC_RES
  Serial.println(F("{\"t\":\"boot\",\"phase\":\"pre-adc\"}"));
  analogReadResolution(PINSCOPE_ADC_BITS);
#endif

  Serial.println(F("{\"t\":\"boot\",\"phase\":\"pre-wire\"}"));
  Wire.begin();
  Serial.println(F("{\"t\":\"boot\",\"phase\":\"ready\"}"));
}

void loop() {
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
      rxLen = 0;
      sendErr("rx overflow");
    }
  }

  uint32_t now = millis();

  // Service I2C polls
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

  // Recompute frequency counts every FREQ_CALC_PERIOD ms
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
