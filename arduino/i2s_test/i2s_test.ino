/**
 * i2s_test.ino -- minimal PCM5102A test for picostepseq
 *
 * Plays a 440 Hz tone forever, alternating sine and square every 2 seconds.
 * The Pico's onboard LED blinks while audio is being written.
 *
 * Wiring:  BCK -> GP26, LCK -> GP27, DIN -> GP28, SCK -> GND, VIN -> VBUS/3V3, GND -> GND
 *          PCM5102 jumpers: FLT=L, DEMP=L, XSMT=H, FMT=L
 *
 * Board: Raspberry Pi Pico (arduino-pico core). No extra libraries needed.
 */

#include <I2S.h>

const int bck_pin = 26;   // LCK is automatically GP27
const int data_pin = 28;
const int sample_rate = 44100;
const float freq = 440.0;

I2S i2s(OUTPUT);

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);

  i2s.setBCLK(bck_pin);
  i2s.setDATA(data_pin);
  i2s.setBitsPerSample(32);
  if (!i2s.begin(sample_rate)) {
    // fast blink forever = I2S failed to start
    while (1) {
      digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
      delay(100);
    }
  }
}

float phase = 0;
uint32_t count = 0;

void loop() {
  bool square = (count / (sample_rate * 2)) % 2;  // swap every 2 secs
  float s = square ? (phase < 0.5f ? 1.0f : -1.0f) * 0.3f
                   : sinf(phase * 2 * PI) * 0.5f;
  int32_t v = (int32_t)(s * 2147483647.0f);
  i2s.write32(v, v);  // left, right (blocks until there's room)

  phase += freq / sample_rate;
  if (phase >= 1.0f) { phase -= 1.0f; }

  count++;
  if (count % (sample_rate / 2) == 0) {  // slow blink = audio running
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
    Serial.printf("running, %s\n", square ? "square" : "sine");
  }
}
