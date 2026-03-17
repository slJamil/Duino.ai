// ============================================================
//  Arduino AI Lab Instrument — Universal Sketch v2
//  ============================================================
//  ANALOG INPUTS:
//    A0 = CH1  Voltage      (0–5V direct)
//    A1 = CH2  Temperature  (NTC 10k thermistor + 10k pullup to 5V)
//    A2 = CH3  Resistance   (unknown R + 10k pullup to 5V)
//    A3 = CH4  Light/LDR    (LDR + 10k pullup to 5V)
//    A4 = CH5  Current      (0.1Ω shunt in series)
//    A5 = CH6  Generic      (any 0–5V signal)
//
//  DIGITAL OUTPUTS:
//    D2  = OUT1   HIGH/LOW
//    D3  = OUT2   HIGH/LOW
//    D4  = OUT3   HIGH/LOW
//    D5  = OUT4   HIGH/LOW
//    D13 = LED    built-in LED (blink command supported)
//    D6  = PWM1   hardware PWM 0–255
//    D9  = PWM2   hardware PWM 0–255
//    D10 = WAVE1  software waveform (sine/square/triangle)
//    D11 = WAVE2  software waveform (sine/square/triangle)
//
//  SERIAL: 115200 baud
//  Commands end with \n
// ============================================================

#include <math.h>

// ── CONSTANTS ─────────────────────────────────────────────────
#define LED_PIN     13
#define VREF        5.0
#define ADC_MAX     1023.0
#define BETA        3950.0
#define R_NOMINAL   10000.0
#define T_NOMINAL   25.0
#define R_SERIES    10000.0
#define SHUNT_OHM   0.1

const int OUT_PINS[]  = {2, 3, 4, 5};
const int PWM_PINS[]  = {6, 9};
const int WAVE_PINS[] = {10, 11};

// ── WAVEFORM STATE ─────────────────────────────────────────────
struct WaveCh {
  bool          active;
  uint8_t       type;      // 0=square 1=sine 2=triangle
  float         freq;      // Hz
  float         amp;       // 0.0–1.0
  float         offset;    // 0.0–1.0  DC bias
  float         phase;     // 0–1 accumulator
  unsigned long lastUs;
};

WaveCh wave[2] = {
  {false, 1, 1.0f, 1.0f, 0.5f, 0.0f, 0},
  {false, 1, 1.0f, 1.0f, 0.5f, 0.0f, 0}
};

// ── SETUP ──────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  while (!Serial) {}           // wait for Leonardo/Micro

  pinMode(LED_PIN, OUTPUT);
  for (int i = 0; i < 4; i++) pinMode(OUT_PINS[i],  OUTPUT);
  for (int i = 0; i < 2; i++) pinMode(PWM_PINS[i],  OUTPUT);
  for (int i = 0; i < 2; i++) pinMode(WAVE_PINS[i], OUTPUT);

  Serial.println("READY");
}

// ── LOOP ───────────────────────────────────────────────────────
void loop() {
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    // FIX: strip both \r and \n and leading/trailing spaces
    cmd.trim();
    if (cmd.length() > 0) handleCmd(cmd);
  }
  updateWaves();
}

// ── WAVEFORM ENGINE ────────────────────────────────────────────
void updateWaves() {
  unsigned long nowUs = micros();
  for (int i = 0; i < 2; i++) {
    if (!wave[i].active) continue;
    unsigned long dt = nowUs - wave[i].lastUs;
    wave[i].lastUs = nowUs;

    // advance phase
    wave[i].phase += wave[i].freq * (dt / 1000000.0f);
    while (wave[i].phase >= 1.0f) wave[i].phase -= 1.0f;

    float p = wave[i].phase;
    float s = 0.0f;
    switch (wave[i].type) {
      case 0: s = (p < 0.5f) ? 1.0f : -1.0f;                        break; // square
      case 1: s = sinf(2.0f * M_PI * p);                             break; // sine
      case 2: s = (p < 0.5f) ? (4.0f*p - 1.0f) : (3.0f - 4.0f*p); break; // triangle
    }

    float out = constrain(wave[i].amp * 0.5f * s + wave[i].offset, 0.0f, 1.0f);
    analogWrite(WAVE_PINS[i], (int)(out * 255.0f));
  }
}

// ── COMMAND PARSER ─────────────────────────────────────────────
void handleCmd(String c) {

  // READ_CH:<1-6>
  if (c.startsWith("READ_CH:")) {
    readChannel(c.substring(8).toInt());
    return;
  }

  // READ_ALL
  if (c == "READ_ALL") {
    for (int i = 1; i <= 6; i++) readChannel(i);
    Serial.println("OK");
    return;
  }

  // OUT:<pin>:<HIGH|LOW>
  if (c.startsWith("OUT:")) {
    int sep = c.indexOf(':', 4);
    if (sep < 0) { Serial.println("ERR:OUT_FMT"); return; }
    int    pin   = c.substring(4, sep).toInt();
    String state = c.substring(sep + 1);
    state.trim();
    digitalWrite(pin, (state == "HIGH") ? HIGH : LOW);
    Serial.print("PIN_STATE:"); Serial.print(pin);
    Serial.print(":"); Serial.println(state);
    return;
  }

  // LED_BLINK:<times>:<on_ms>:<off_ms>
  // e.g.  LED_BLINK:3:300:300
  if (c.startsWith("LED_BLINK:")) {
    // FIX: parse all 3 params properly
    String params = c.substring(10);
    int s1 = params.indexOf(':');
    int s2 = (s1 >= 0) ? params.indexOf(':', s1 + 1) : -1;
    int times  = params.toInt();
    int on_ms  = (s1 >= 0) ? params.substring(s1 + 1).toInt() : 300;
    int off_ms = (s2 >= 0) ? params.substring(s2 + 1).toInt() : 300;
    times  = constrain(times,  1,  20);
    on_ms  = constrain(on_ms,  50, 5000);
    off_ms = constrain(off_ms, 50, 5000);
    for (int i = 0; i < times; i++) {
      digitalWrite(LED_PIN, HIGH); delay(on_ms);
      digitalWrite(LED_PIN, LOW);  delay(off_ms);
    }
    Serial.println("OK");
    return;
  }

  // ALL_OFF
  if (c == "ALL_OFF") {
    digitalWrite(LED_PIN, LOW);
    for (int i = 0; i < 4; i++) digitalWrite(OUT_PINS[i], LOW);
    for (int i = 0; i < 2; i++) analogWrite(PWM_PINS[i],  0);
    for (int i = 0; i < 2; i++) {
      wave[i].active = false;
      analogWrite(WAVE_PINS[i], 0);
    }
    Serial.println("OK");
    return;
  }

  // PWM:<pin>:<duty 0-255>
  if (c.startsWith("PWM:")) {
    int sep = c.indexOf(':', 4);
    if (sep < 0) { Serial.println("ERR:PWM_FMT"); return; }
    int pin  = c.substring(4, sep).toInt();
    int duty = constrain(c.substring(sep + 1).toInt(), 0, 255);
    analogWrite(pin, duty);
    Serial.print("PWM_ACK:"); Serial.print(pin);
    Serial.print(":"); Serial.println(duty);
    return;
  }

  // WAVE:<ch>:<type>:<freq>:<amp>:<offset>
  // FIX: use explicit indexOf chain starting at position 5
  if (c.startsWith("WAVE:")) {
    int p0 = 5;                          // start of ch digit
    int p1 = c.indexOf(':', p0);         // after ch
    int p2 = (p1>0) ? c.indexOf(':', p1+1) : -1;  // after type
    int p3 = (p2>0) ? c.indexOf(':', p2+1) : -1;  // after freq
    int p4 = (p3>0) ? c.indexOf(':', p3+1) : -1;  // after amp

    if (p1 < 0 || p2 < 0 || p3 < 0) {
      Serial.println("ERR:WAVE_FMT"); return;
    }

    int    ch   = c.substring(p0, p1).toInt() - 1;
    String type = c.substring(p1+1, p2);
    float  freq = c.substring(p2+1, p3).toFloat();
    float  amp  = (p4 > 0) ? c.substring(p3+1, p4).toFloat() : 1.0f;
    float  offs = (p4 > 0) ? c.substring(p4+1).toFloat()     : 0.5f;

    if (ch < 0 || ch > 1) { Serial.println("ERR:WAVE_CH"); return; }

    wave[ch].active = true;
    wave[ch].freq   = constrain(freq, 0.01f, 500.0f);
    wave[ch].amp    = constrain(amp,  0.0f,  1.0f);
    wave[ch].offset = constrain(offs, 0.0f,  1.0f);
    wave[ch].phase  = 0.0f;
    wave[ch].lastUs = micros();
    wave[ch].type   = (type == "sine") ? 1 : (type == "triangle") ? 2 : 0;

    Serial.print("WAVE_ACK:"); Serial.print(ch+1);
    Serial.print(":"); Serial.print(type);
    Serial.print(":"); Serial.println(freq, 2);
    return;
  }

  // WAVE_STOP:<ch>
  if (c.startsWith("WAVE_STOP:")) {
    int ch = c.substring(10).toInt() - 1;
    if (ch >= 0 && ch <= 1) {
      wave[ch].active = false;
      analogWrite(WAVE_PINS[ch], 0);
    }
    Serial.println("OK");
    return;
  }

  Serial.print("ERR:UNKNOWN:");
  Serial.println(c);
}

// ── CHANNEL READER ─────────────────────────────────────────────
void readChannel(int ch) {
  int   raw;
  float val;
  char  buf[16];

  switch (ch) {

    case 1: {  // Voltage A0
      raw = analogRead(A0);
      val = raw * (VREF / ADC_MAX);
      dtostrf(val, 6, 4, buf);
      Serial.print("CH1_V:"); Serial.println(buf);
      break;
    }

    case 2: {  // Temperature A1 — NTC Steinhart-Hart
      raw = analogRead(A1);
      if (raw <= 0 || raw >= 1023) { Serial.println("CH2_T:ERR"); break; }
      float R   = R_SERIES * (ADC_MAX / raw - 1.0f);
      float sh  = logf(R / R_NOMINAL) / BETA + 1.0f / (T_NOMINAL + 273.15f);
      val       = 1.0f / sh - 273.15f;
      dtostrf(val, 6, 2, buf);
      Serial.print("CH2_T:"); Serial.println(buf);
      break;
    }

    case 3: {  // Resistance A2
      raw = analogRead(A2);
      if (raw <= 0) { Serial.println("CH3_R:OVR"); break; }
      val = R_SERIES * (ADC_MAX / raw - 1.0f);
      dtostrf(val, 8, 1, buf);
      Serial.print("CH3_R:"); Serial.println(buf);
      break;
    }

    case 4: {  // Light A3 — voltage across LDR
      raw = analogRead(A3);
      val = raw * (VREF / ADC_MAX);
      dtostrf(val, 6, 4, buf);
      Serial.print("CH4_L:"); Serial.println(buf);
      break;
    }

    case 5: {  // Current A4 — shunt
      raw = analogRead(A4);
      val = (raw * (VREF / ADC_MAX)) / SHUNT_OHM * 1000.0f;  // mA
      dtostrf(val, 7, 2, buf);
      Serial.print("CH5_I:"); Serial.println(buf);
      break;
    }

    case 6: {  // Generic A5
      raw = analogRead(A5);
      val = raw * (VREF / ADC_MAX);
      dtostrf(val, 6, 4, buf);
      Serial.print("CH6_G:"); Serial.println(buf);
      break;
    }

    default:
      Serial.println("ERR:CH_RANGE");
  }
}
