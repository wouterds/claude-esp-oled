#include "audio.h"

#include <Arduino.h>
#include <ESP_I2S.h>
#include <Wire.h>
#include <math.h>

namespace {

constexpr int I2C_SDA = 11;
constexpr int I2C_SCL = 10;
// Seven bits. The board's own documentation gives 0x30, which is the same
// address written for a bus that counts the read/write bit as part of it.
constexpr uint8_t CODEC = 0x18;

constexpr int PIN_MCLK = 2;
constexpr int PIN_BCLK = 48;
constexpr int PIN_LRCK = 38;
// Playback out on 47 and the microphones in on 39, which is the way round the
// board's own pin table has it - the summary line above that table calls them
// DOUT and DIN from the codec's point of view instead, and taking that at face
// value sends the sound to the microphones. That reads as an amplifier with
// nothing arriving at it, which is to say: hiss.
constexpr int PIN_DOUT = 47;
constexpr int PIN_DIN = 39;
// The amplifier is only awake while there is something to say. Left on it puts
// a hiss on a board whose whole point is sitting quietly on a desk.
constexpr int PIN_PA = 9;

// 16kHz is above everything these sounds are made of and a quarter of the
// samples of a rate that would also be. The codec wants 256 of its own clocks
// per sample, which is the row every ordinary rate has in Espressif's table.
constexpr uint32_t RATE = 16000;
// A part of full scale, done in the samples rather than in the codec. The
// part's own volume register is in half decibels, so a percentage written
// straight into it is a percentage of nothing anybody can hear - a third of the
// way up that register is -58dB, which is silence with a bill attached.
constexpr uint8_t VOLUME = 30;
// Short of the rails even at full volume. A sine that reaches them is a sine
// that clips into something buzzing the moment anything is added to it.
constexpr float PEAK = 26000.0f;

// How long a note takes to come up, and how much of its tail it spends going
// away. Neither is taste: a note that starts at full amplitude clicks, and one
// that stops there clicks louder.
constexpr float ATTACK_MS = 4.0f;
constexpr float RELEASE = 0.3f;

// A pulse rather than a sine, because that is the sound this is meant to be,
// and rounded off afterwards, because a square edge through a speaker this size
// is the difference between a chiptune and a smoke alarm. The coefficient puts
// the corner near 3kHz: above every note here and below most of what a square
// stacks on top of one.
constexpr float DUTY = 0.5f;
constexpr float ROUND = 0.7f;

struct Note {
  uint16_t hz;
  uint16_t ms;
  // Silence after it. Nought where notes are meant to run together, which is
  // how one voice pretends to be a chord.
  uint16_t gap;
};

// Two notes to set it going, then the chord it lands on - played the way a
// machine with one voice plays a chord, an octave at a time and fast enough to
// be heard as one sound. It ends where it has been climbing to rather than
// somewhere new, which is the whole of why it is satisfying and not a nag.
constexpr Note HELLO[] = {
    {523, 70, 10},  {784, 70, 10},  {1047, 80, 25}, {1047, 28, 0}, {1319, 28, 0},
    {1568, 28, 0},  {2093, 28, 0},  {1047, 28, 0},  {1319, 28, 0}, {1568, 28, 0},
    {2093, 28, 0},  {2093, 300, 0},
};
// Two notes and done - this one happens with a hand still on the cable.
constexpr Note PLUGGED[] = {{1568, 60, 0}, {2093, 130, 0}};
// Pleased with itself: up, a skip back, and up again to land.
constexpr Note CHEERED[] = {{1047, 70, 10}, {1319, 70, 10}, {1568, 70, 10},
                            {1319, 70, 10}, {1568, 70, 10}, {2093, 260, 0}};

I2SClass i2s;
bool ready = false;
volatile uint8_t wanted = 0;
// One note's worth at a time rather than a whole sound: a sixteenth of a second
// of samples is a kilobyte, and the whole of HELLO would be sixteen.
int16_t chunk[512];

bool put(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(CODEC);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

// Espressif's own sequence for this part, with the clock dividers resolved for
// one rate rather than carried as the table they come out of - at 256 clocks a
// sample there is nothing to divide, multiply or oversample, so the whole row
// is ones and zeroes.
bool codecBegin() {
  if (!put(0x00, 0x1F)) {
    return false;
  }
  delay(20);
  put(0x00, 0x00);
  // Powered, and a slave: bit six staying clear is what says the clocks are
  // somebody else's problem.
  put(0x00, 0x80);
  put(0x01, 0x3F);
  put(0x02, 0x00);
  put(0x03, 0x10);
  put(0x04, 0x10);
  put(0x05, 0x00);
  put(0x06, 0x03);
  put(0x07, 0x00);
  put(0x08, 0xFF);
  // Sixteen bits in and out, which is three in the word length field.
  put(0x09, 0x0C);
  put(0x0A, 0x0C);
  put(0x0D, 0x01);
  put(0x0E, 0x02);
  put(0x12, 0x00);
  put(0x13, 0x10);
  put(0x1C, 0x6A);
  // Not muted. The two bits mean it, and nothing else here would say so.
  put(0x31, 0x00);
  put(0x37, 0x08);
  // Nought decibels: neither cutting nor lifting what it is handed, so what
  // comes out is what was written.
  return put(0x32, 0xBF);
}

// Carried across notes rather than started fresh in each: the whole point of it
// is that nothing in the output jumps, and a filter that restarts at nought is
// itself a jump.
float rounded = 0.0f;

void note(const Note &n) {
  uint32_t total = (uint32_t)n.ms * RATE / 1000;
  uint32_t attack = (uint32_t)(ATTACK_MS * RATE / 1000.0f);
  uint32_t held = (uint32_t)(total * (1.0f - RELEASE));
  float step = (float)n.hz / (float)RATE;
  float phase = 0.0f;

  for (uint32_t done = 0; done < total;) {
    uint32_t take = total - done;
    if (take > sizeof(chunk) / sizeof(chunk[0])) {
      take = sizeof(chunk) / sizeof(chunk[0]);
    }
    for (uint32_t i = 0; i < take; i++) {
      uint32_t at = done + i;
      float level = 1.0f;
      if (at < attack) {
        level = (float)at / (float)attack;
      } else if (at > held) {
        level = (float)(total - at) / (float)(total - held);
        // Squared on the way out, so it fades rather than ramps.
        level *= level;
      }
      float pulse = phase < DUTY ? 1.0f : -1.0f;
      rounded += (pulse - rounded) * ROUND;
      chunk[i] = (int16_t)(rounded * PEAK * (VOLUME / 100.0f) * level);
      phase += step;
      if (phase >= 1.0f) {
        phase -= 1.0f;
      }
    }
    i2s.write((uint8_t *)chunk, take * sizeof(chunk[0]));
    done += take;
  }

  uint32_t quiet = (uint32_t)n.gap * RATE / 1000;
  memset(chunk, 0, sizeof(chunk));
  while (quiet > 0) {
    uint32_t take = quiet > sizeof(chunk) / sizeof(chunk[0]) ? sizeof(chunk) / sizeof(chunk[0])
                                                             : quiet;
    i2s.write((uint8_t *)chunk, take * sizeof(chunk[0]));
    quiet -= take;
  }
}

void play(const Note *notes, uint8_t count) {
  digitalWrite(PIN_PA, HIGH);
  // The amplifier takes a moment to come up, and whatever is written into it
  // before it has is the click of it coming up.
  delay(8);
  for (uint8_t i = 0; i < count; i++) {
    note(notes[i]);
  }
  digitalWrite(PIN_PA, LOW);
}

void task(void *) {
  for (;;) {
    uint8_t want = wanted;
    if (want != 0) {
      wanted = 0;
      switch (want) {
        case 1:
          play(HELLO, sizeof(HELLO) / sizeof(HELLO[0]));
          break;
        case 2:
          play(PLUGGED, sizeof(PLUGGED) / sizeof(PLUGGED[0]));
          break;
        default:
          play(CHEERED, sizeof(CHEERED) / sizeof(CHEERED[0]));
          break;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

}  // namespace

void audioBegin() {
  Wire.begin(I2C_SDA, I2C_SCL);
  pinMode(PIN_PA, OUTPUT);
  digitalWrite(PIN_PA, LOW);

  // The clocks first. The codec's own clock manager is set up against an MCLK
  // that has to already be arriving - configured in silence it takes the
  // settings and does nothing with them, which sounds exactly like a part that
  // is not there.
  i2s.setPins(PIN_BCLK, PIN_LRCK, PIN_DOUT, PIN_DIN, PIN_MCLK);
  if (!i2s.begin(I2S_MODE_STD, RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
    Serial.println("audio: no I2S");
    return;
  }
  delay(10);

  if (!codecBegin()) {
    Serial.println("audio: ES8311 not answering");
    return;
  }
  ready = true;
  Serial.printf("audio: ES8311 ready at %lu Hz, %u%%\n", (unsigned long)RATE, VOLUME);
  // Core 0, with the radio. A note is a tenth of a second of arithmetic and a
  // frame is a sixtieth: this may never be in the way of one.
  xTaskCreatePinnedToCore(task, "audio", 4096, nullptr, 1, nullptr, 0);
}

void audioHello() {
  if (ready) {
    wanted = 1;
  }
}

void audioPlugged() {
  if (ready) {
    wanted = 2;
  }
}

void audioCheered() {
  if (ready) {
    wanted = 3;
  }
}
