#include <Arduino.h>

#include "board.h"
#include "buddy.h"
#include "face.h"

// A face on a round panel, and nothing else. It is not told anything and it
// does not ask - the whole of it is here, so what it does costs a flash rather
// than a protocol.
static Buddy buddy;
static Rect painted = {0, 0, 0, 0};
static uint32_t lastFrame = 0;

void setup() {
  Serial.begin(115200);
  if (!boardBegin()) {
    while (true) {
      delay(1000);
    }
  }
  buddyBegin(buddy);
  lastFrame = millis();
}

void loop() {
  uint32_t now = millis();
  float dt = (float)(now - lastFrame) * 0.001f;
  lastFrame = now;
  // A stall must not teleport it across the panel on the frame after.
  if (dt > 0.05f) {
    dt = 0.05f;
  }

  buddyUpdate(buddy, dt, now);
  FaceParams face = buddyFace(buddy, now);

  uint16_t *fb = boardFramebuffer();
  // Only last frame's rect can be holding anything: faceRender writes every
  // pixel of the box it returns, the black ones included.
  faceClear(fb, painted);
  painted = faceRender(fb, face);
  boardFlush();

  // Sixty is past what the panel or the eye wants; the rest goes back.
  uint32_t spent = millis() - now;
  if (spent < 16) {
    delay(16 - spent);
  }
}
