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
// The codec's input, which is this end's output. The other direction is the
// microphones and nothing here asks them anything.
constexpr int PIN_DOUT = 39;
constexpr int PIN_DIN = 47;
// The amplifier is only awake while there is something to say. Left on it puts
// a hiss on a board whose whole point is sitting quietly on a desk.
constexpr int PIN_PA = 9;

// 16kHz is above everything these sounds are made of and a quarter of the
// samples of a rate that would also be. The codec wants 256 of its own clocks
// per sample, which is the row every ordinary rate has in Espressif's table.
constexpr uint32_t RATE = 16000;
// A part of full scale, done in the samples rather than in the codec. The
// part's own volume register is in half decibels, so a percentage mapped
// straight onto it is a percentage of nothing anybody can hear - thirty of them
// lands at -58dB, which is silence with a bill attached.
constexpr uint8_t VOLUME = 30;
// Short of the rails even at full volume. A sine that reaches them is a sine
// that clips into something buzzing the moment anything is added to it.
constexpr float PEAK = 26000.0f;

// How long a note takes to come up, and how much of its tail it spends going
// away. Neither is taste: a note that starts at full amplitude clicks, and one
// that stops there clicks louder.
constexpr float ATTACK_MS = 6.0f;
constexpr float RELEASE = 0.45f;
// Between notes, so an arpeggio is heard as notes rather than as a slide.
constexpr uint16_t GAP_MS = 18;

struct Note {
  uint16_t hz;
  uint16_t ms;
};

// A major arpeggio, ending where it started an octave up. It is four notes
// because three is a doorbell and five is a tune.
constexpr Note HELLO[] = {{1047, 85}, {1319, 85}, {1568, 85}, {2093, 190}};
// Two notes and done - this one happens with a hand still on the cable.
constexpr Note PLUGGED[] = {{1568, 70}, {2093, 130}};
// Pleased with itself: up, a skip back, and up again to land.
constexpr Note CHEERED[] = {{1047, 80}, {1319, 80}, {1568, 80},
                            {1319, 80}, {1568, 80}, {2093, 240}};

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
  put(0x37, 0x08);
  // Nought decibels: neither cutting nor lifting what it is handed, so what
  // comes out is what was written.
  return put(0x32, 0xBF);
}

void note(const Note &n) {
  uint32_t total = (uint32_t)n.ms * RATE / 1000;
  uint32_t attack = (uint32_t)(ATTACK_MS * RATE / 1000.0f);
  uint32_t held = (uint32_t)(total * (1.0f - RELEASE));
  float step = 2.0f * (float)M_PI * (float)n.hz / (float)RATE;
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
      chunk[i] = (int16_t)(sinf(phase) * PEAK * (VOLUME / 100.0f) * level);
      phase += step;
    }
    i2s.write((uint8_t *)chunk, take * sizeof(chunk[0]));
    done += take;
  }

  uint32_t quiet = (uint32_t)GAP_MS * RATE / 1000;
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

  if (!codecBegin()) {
    Serial.println("audio: ES8311 not answering");
    return;
  }

  i2s.setPins(PIN_BCLK, PIN_LRCK, PIN_DOUT, PIN_DIN, PIN_MCLK);
  if (!i2s.begin(I2S_MODE_STD, RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
    Serial.println("audio: no I2S");
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
