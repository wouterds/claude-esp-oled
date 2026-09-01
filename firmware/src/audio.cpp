#include "audio.h"

#include <Arduino.h>
#include <ESP_I2S.h>
#include <Wire.h>
#include <math.h>

#include "bus.h"
#include "pins.h"

namespace {


// 16kHz is above everything these sounds are made of and a quarter of the
// samples of a rate that would also be. The codec wants 256 of its own clocks
// per sample, which is the row every ordinary rate has in Espressif's table.
constexpr uint32_t RATE = 16000;
// A part of full scale, done in the samples rather than in the codec. The
// part's own volume register is in half decibels, so a percentage written
// straight into it is a percentage of nothing anybody can hear - a fraction of
// the way up that register is tens of decibels down, which is silence with a
// bill attached.
//
// Squared on the way in. The ear hears ratios, and a slider that is linear in
// amplitude has all of its range in the first fifth - everything past that is
// loud and a bit louder. A third of the way up is the tenth of full scale this
// shipped at.
volatile uint8_t volume = 32;
// Short of the rails even at full volume. A sine that reaches them is a sine
// that clips into something buzzing the moment anything is added to it.
constexpr float PEAK = 26000.0f;

// How long a note takes to come up, and how much of its tail it spends going
// away. Neither is taste: a note that starts at full amplitude clicks, and one
// that stops there clicks louder.
constexpr float ATTACK_MS = 4.0f;
// Most of the note is its own tail. What makes a short sound cute rather than
// abrupt is that it stops by fading, not by ending.
constexpr float RELEASE = 0.65f;

// A triangle. A square was the honest chiptune answer and it is horrible out of
// a speaker this size: its harmonics fall off half as fast, so at these pitches
// what reaches the ear is mostly the ones above the note. A triangle's fall off
// as the square of them, and the filter takes what is left of the corners off.
constexpr float ROUND = 0.85f;

struct Note {
  uint16_t hz;
  uint16_t ms;
  // Silence after it. Nought where notes are meant to run together, which is
  // how one voice pretends to be a chord.
  uint16_t gap;
  // Where the pitch ends up, for a note that does not stay where it started.
  // Nought to hold still, which is what every note here does but one: stepping
  // between pitches is a tune however sad the pitches are, and the one thing on
  // this board that must not sound like a tune is the death.
  //
  // Last on purpose - left off, it is nought, so the tables above that hold
  // still say nothing about it.
  uint16_t to;
};

// A blip and the note it lands on, a fifth up. Two notes is the shortest thing
// that can still go somewhere, and going up is the whole of why it reads as
// pleased. This one happens with a hand still on the cable, so it is the
// shorter of the two.
constexpr Note PLUGGED[] = {{1319, 45, 0}, {1760, 120, 0}};
// One voice climbing a major triad and clearing the octave, which is what
// arriving somewhere sounds like. The steps run together rather than being
// sounded at, and the last is held four times as long as any of them: the climb
// is the news and the note it lands on is where the news settles.
constexpr Note CHEERED[] = {{1047, 70, 0}, {1319, 70, 0}, {1568, 70, 0}, {2093, 260, 0}};
// Down where the other two go up, and down by the one interval that has never
// sounded settled to anybody - which is the whole of why it reads as wrong
// rather than as sad. Said twice, or a single fall is a notification.
constexpr Note ERRORED[] = {{1760, 60, 50}, {1245, 150, 90}, {1760, 60, 50}, {1245, 190, 0}};
// Pitch bleeding out, and then the floor. The slide is one note rather than
// several because an interval - any interval - is a musical idea, and three
// notes down a major triad is the cheer played backwards, which is why the
// first attempt at this sounded like a doorbell.
//
// The gap is the whole trick: the slide runs out, nothing happens for a beat,
// and then something lands underneath it. That note is below anything else on
// the board and near the bottom of what this speaker can move at all - it comes
// out soft and buzzy, which is the point.
constexpr Note DIED[] = {{1568, 620, 140, 600}, {415, 420, 0}};
// The forever song, for as long as the cat has the glass: daniwell's tune, as
// the one voice this has - the melody and none of what sits under it, which is
// what it sounds like out of anything with a piezo in it. A rest is a note with
// no length and a gap the length of the rest, so the engine never has to know.
// Sixteenths at a hundred and forty-two to the minute, as written.
constexpr Note NYAN[] = {
    {740, 212, 0}, {831, 212, 0}, {622, 106, 0}, {622, 106, 0}, {0, 0, 106}, {494, 106, 0},
    {587, 106, 0}, {554, 106, 0}, {494, 106, 0}, {0, 0, 106}, {494, 212, 0}, {554, 212, 0},
    {587, 212, 0}, {587, 106, 0}, {554, 106, 0}, {494, 106, 0}, {554, 106, 0}, {622, 106, 0},
    {740, 106, 0}, {831, 106, 0}, {622, 106, 0}, {740, 106, 0}, {554, 106, 0}, {622, 106, 0},
    {494, 106, 0}, {554, 106, 0}, {494, 106, 0}, {622, 212, 0}, {740, 212, 0}, {831, 106, 0},
    {622, 106, 0}, {740, 106, 0}, {554, 106, 0}, {622, 106, 0}, {494, 106, 0}, {587, 106, 0},
    {622, 106, 0}, {587, 106, 0}, {554, 106, 0}, {494, 106, 0}, {554, 106, 0}, {587, 212, 0},
    {494, 106, 0}, {554, 106, 0}, {622, 106, 0}, {740, 106, 0}, {554, 106, 0}, {622, 106, 0},
    {554, 106, 0}, {494, 106, 0}, {554, 212, 0}, {494, 212, 0}, {554, 212, 0}, {740, 212, 0},
    {831, 212, 0}, {622, 106, 0}, {622, 106, 0}, {0, 0, 106}, {494, 106, 0}, {587, 106, 0},
    {554, 106, 0}, {494, 106, 0}, {0, 0, 106}, {494, 212, 0}, {554, 212, 0}, {587, 212, 0},
    {587, 106, 0}, {554, 106, 0}, {494, 106, 0}, {554, 106, 0}, {622, 106, 0}, {740, 106, 0},
    {831, 106, 0}, {622, 106, 0}, {740, 106, 0}, {554, 106, 0}, {622, 106, 0}, {494, 106, 0},
    {554, 106, 0}, {494, 106, 0}, {622, 212, 0}, {740, 212, 0}, {831, 106, 0}, {622, 106, 0},
    {740, 106, 0}, {554, 106, 0}, {622, 106, 0}, {494, 106, 0}, {587, 106, 0}, {622, 106, 0},
    {587, 106, 0}, {554, 106, 0}, {494, 106, 0}, {554, 106, 0}, {587, 212, 0}, {494, 106, 0},
    {554, 106, 0}, {622, 106, 0}, {740, 106, 0}, {554, 106, 0}, {622, 106, 0}, {554, 106, 0},
    {494, 106, 0}, {554, 212, 0}, {494, 212, 0}, {554, 212, 0}, {494, 212, 0}, {370, 106, 0},
    {415, 106, 0}, {494, 212, 0}, {370, 106, 0}, {415, 106, 0}, {494, 106, 0}, {554, 106, 0},
    {622, 106, 0}, {494, 106, 0}, {659, 106, 0}, {622, 106, 0}, {659, 106, 0}, {740, 106, 0},
    {494, 212, 0}, {494, 212, 0}, {370, 106, 0}, {415, 106, 0}, {494, 106, 0}, {370, 106, 0},
    {659, 106, 0}, {622, 106, 0}, {554, 106, 0}, {494, 106, 0}, {370, 106, 0}, {311, 106, 0},
    {330, 106, 0}, {370, 106, 0}, {494, 212, 0}, {370, 106, 0}, {415, 106, 0}, {494, 212, 0},
    {370, 106, 0}, {415, 106, 0}, {494, 106, 0}, {494, 106, 0}, {554, 106, 0}, {622, 106, 0},
    {494, 106, 0}, {370, 106, 0}, {415, 106, 0}, {370, 106, 0}, {494, 212, 0}, {494, 106, 0},
    {466, 106, 0}, {494, 106, 0}, {370, 106, 0}, {415, 106, 0}, {330, 106, 0}, {659, 106, 0},
    {622, 106, 0}, {659, 106, 0}, {740, 106, 0}, {494, 212, 0}, {466, 212, 0}, {494, 212, 0},
    {370, 106, 0}, {415, 106, 0}, {494, 212, 0}, {370, 106, 0}, {415, 106, 0}, {494, 106, 0},
    {554, 106, 0}, {622, 106, 0}, {494, 106, 0}, {659, 106, 0}, {622, 106, 0}, {659, 106, 0},
    {740, 106, 0}, {494, 212, 0}, {494, 212, 0}, {370, 106, 0}, {415, 106, 0}, {494, 106, 0},
    {370, 106, 0}, {659, 106, 0}, {622, 106, 0}, {554, 106, 0}, {494, 106, 0}, {370, 106, 0},
    {311, 106, 0}, {330, 106, 0}, {370, 106, 0}, {494, 212, 0}, {370, 106, 0}, {415, 106, 0},
    {494, 212, 0}, {370, 106, 0}, {415, 106, 0}, {494, 106, 0}, {494, 106, 0}, {554, 106, 0},
    {622, 106, 0}, {494, 106, 0}, {370, 106, 0}, {415, 106, 0}, {370, 106, 0}, {494, 212, 0},
    {494, 106, 0}, {466, 106, 0}, {494, 106, 0}, {370, 106, 0}, {415, 106, 0}, {494, 106, 0},
    {659, 106, 0}, {622, 106, 0}, {659, 106, 0}, {740, 106, 0}, {494, 212, 0}, {554, 212, 0},
};
constexpr uint16_t NYAN_NOTES = sizeof(NYAN) / sizeof(NYAN[0]);

I2SClass i2s;
bool ready = false;
volatile uint8_t wanted = 0;
volatile bool singing = false;
bool amped = false;
// One note's worth at a time rather than a whole sound: a sixteenth of a second
// of samples is a kilobyte, and a whole sound would be several.
int16_t chunk[512];

bool put(uint8_t reg, uint8_t value) {
  busTake();
  Wire.beginTransmission(CODEC_ADDR);
  Wire.write(reg);
  Wire.write(value);
  bool sent = Wire.endTransmission() == 0;
  busGive();
  return sent;
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
  float from = (float)n.hz / (float)RATE;
  float to = (float)(n.to ? n.to : n.hz) / (float)RATE;
  // Worked out once rather than as a fraction of the way through per sample,
  // which would be a divide sixteen thousand times a second for a number that
  // moves by the same amount every time.
  float slide = total ? (to - from) / (float)total : 0.0f;
  float phase = 0.0f;
  // Read once a note rather than once a sample: a finger on the slider moves it
  // between notes and a note that changes level halfway through clicks.
  float gain = (float)volume / 100.0f;
  gain *= gain;

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
      float triangle = 4.0f * fabsf(phase - 0.5f) - 1.0f;
      rounded += (triangle - rounded) * ROUND;
      chunk[i] = (int16_t)(rounded * PEAK * gain * level);
      phase += from + slide * (float)at;
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

void amp(bool on) {
  if (on == amped) {
    return;
  }
  amped = on;
  digitalWrite(AUDIO_PA, on ? HIGH : LOW);
  // The amplifier takes a moment to come up, and whatever is written into it
  // before it has is the click of it coming up.
  if (on) {
    delay(8);
  }
}

void play(const Note *notes, uint8_t count) {
  amp(true);
  for (uint8_t i = 0; i < count; i++) {
    note(notes[i]);
  }
  // Left up through a song, or every sound in it would come with the click.
  if (!singing) {
    amp(false);
  }
}

void task(void *) {
  uint16_t at = 0;
  for (;;) {
    uint8_t want = wanted;
    if (want != 0) {
      wanted = 0;
      if (want == 1) {
        play(PLUGGED, sizeof(PLUGGED) / sizeof(PLUGGED[0]));
      } else if (want == 2) {
        play(CHEERED, sizeof(CHEERED) / sizeof(CHEERED[0]));
      } else if (want == 3) {
        play(ERRORED, sizeof(ERRORED) / sizeof(ERRORED[0]));
      } else {
        play(DIED, sizeof(DIED) / sizeof(DIED[0]));
      }
    }
    // A note at a time round the loop rather than the whole song in one go, so
    // the flag is seen between notes and a sound asked for lands between two of
    // them rather than after all of them.
    if (singing) {
      amp(true);
      note(NYAN[at]);
      at = (uint16_t)((at + 1) % NYAN_NOTES);
      continue;
    }
    at = 0;
    amp(false);
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

}  // namespace

void audioBegin() {
  pinMode(AUDIO_PA, OUTPUT);
  digitalWrite(AUDIO_PA, LOW);

  // The clocks first. The codec's own clock manager is set up against an MCLK
  // that has to already be arriving - configured in silence it takes the
  // settings and does nothing with them, which sounds exactly like a part that
  // is not there.
  i2s.setPins(AUDIO_BCLK, AUDIO_LRCK, AUDIO_DOUT, AUDIO_DIN, AUDIO_MCLK);
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
  Serial.printf("audio: ES8311 ready at %lu Hz\n", (unsigned long)RATE);
  // Core 0, with the radio. A note is a tenth of a second of arithmetic and a
  // frame is a sixtieth: this may never be in the way of one.
  xTaskCreatePinnedToCore(task, "audio", 4096, nullptr, 1, nullptr, 0);
}

void audioVolume(uint8_t percent) { volume = percent > 100 ? 100 : percent; }

void audioSong(bool on) { singing = ready && on; }

void audioPlugged() {
  if (ready) {
    wanted = 1;
  }
}

// The same two notes as the cable: short, up, and over before the hand is off
// the glass. What is being demonstrated is the level, not a sound of its own.
void audioSampled() {
  if (ready) {
    wanted = 1;
  }
}

void audioCheered() {
  if (ready) {
    wanted = 2;
  }
}

void audioErrored() {
  if (ready) {
    wanted = 3;
  }
}

void audioDied() {
  if (ready) {
    wanted = 4;
  }
}
