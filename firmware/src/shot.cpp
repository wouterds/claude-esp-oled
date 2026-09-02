#include "shot.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <PNGenc.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <new>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "pins.h"

namespace {

constexpr char DIR[] = "/shots";
constexpr char PREFIX[] = "shot-";
// "/shots/" and the longest name that goes after it.
constexpr size_t PATH_MAX_LEN = sizeof(DIR) + SHOT_NAME_MAX;

// Six figures, not four. Nothing here ever reuses a number - the next one is
// the highest already on the flash plus one - so both the order the page is
// drawn in and the choice of which file to evict come out of the names, and
// they only stay right while the counter has not been round. Six figures is a
// million presses of a button that wants three seconds each; four would have
// been eleven hours of holding it.
constexpr uint32_t NUMBER_MAX = 1000000;

// zlib's own default rather than the maximum. The encoder cuts its window to
// 4KB on parts this size, which is much the larger of the two levers, and nine
// against six buys a percent or so of the file for another second of the panel
// sitting still.
constexpr uint8_t LEVEL = 6;

bool mounted = false;
// Recursive, so the public lock a caller takes around an open file nests with
// the one every entry point below takes for itself.
SemaphoreHandle_t held = nullptr;
// Newest first, which is the order the page wants and the reverse of the order
// the last one is evicted from.
char names[SHOTS_MAX][SHOT_NAME_MAX] = {{0}};
uint8_t count = 0;
uint32_t next = 0;

void pathOf(const char *name, char *out, size_t size) { snprintf(out, size, "%s/%s", DIR, name); }

// The number out of a name this wrote, or -1 for anything else that turns up in
// the directory - which is the only thing that makes it something to leave
// alone rather than something to evict.
long numberOf(const char *name) {
  size_t prefix = strlen(PREFIX);
  if (strncmp(name, PREFIX, prefix) != 0) {
    return -1;
  }
  const char *digits = name + prefix;
  char *end = nullptr;
  long n = strtol(digits, &end, 10);
  if (end == digits || n < 0 || strcmp(end, ".png") != 0) {
    return -1;
  }
  return n;
}

// Puts a name in its place, newest first, and gives back whatever fell off the
// end - the file that has to go, or an empty string while there is still room.
void insert(const char *name, char *dropped, size_t size) {
  uint8_t at = 0;
  while (at < count && strcmp(names[at], name) > 0) {
    at++;
  }
  // Older than everything kept and there is no room for it: this one is the one
  // that goes, without ever having been in the list.
  if (at == SHOTS_MAX) {
    snprintf(dropped, size, "%s", name);
    return;
  }
  if (count == SHOTS_MAX) {
    snprintf(dropped, size, "%s", names[--count]);
  } else {
    dropped[0] = '\0';
  }
  for (uint8_t i = count; i > at; i--) {
    memcpy(names[i], names[i - 1], SHOT_NAME_MAX);
  }
  snprintf(names[at], SHOT_NAME_MAX, "%s", name);
  count++;
}

// Everything already on the flash, newest first, with the next number set past
// the highest of them. Whatever the list has no room for is unlinked after the
// walk rather than during it: removing an entry from a directory being iterated
// is not something littlefs promises anything about.
void scan() {
  count = 0;
  next = 0;
  char strays[SHOTS_MAX][SHOT_NAME_MAX];
  uint8_t stray = 0;

  File dir = LittleFS.open(DIR);
  if (!dir || !dir.isDirectory()) {
    return;
  }
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (f.isDirectory()) {
      continue;
    }
    // name() off a directory walk is the last element already, path() is the
    // whole thing - and the whole thing would not fit what is kept here.
    const char *name = f.name();
    long n = numberOf(name);
    if (n < 0) {
      continue;
    }
    if ((uint32_t)n >= next) {
      next = (uint32_t)n + 1;
    }
    char dropped[SHOT_NAME_MAX];
    insert(name, dropped, sizeof(dropped));
    if (dropped[0] && stray < SHOTS_MAX) {
      memcpy(strays[stray++], dropped, SHOT_NAME_MAX);
    }
  }
  dir.close();

  char path[PATH_MAX_LEN];
  for (uint8_t i = 0; i < stray; i++) {
    pathOf(strays[i], path, sizeof(path));
    LittleFS.remove(path);
    Serial.printf("shot: %s was past the last %u and is gone\n", strays[i], SHOTS_MAX);
  }
}

// Where a name asked for by a request turns into one this put there, and the
// only place it does. Nothing below it takes a name that did not come out of
// the kept list, so `..` and a leading slash are not cases to strip - they are
// names it does not have.
int8_t indexOf(const char *name) {
  for (uint8_t i = 0; i < count; i++) {
    if (strcmp(names[i], name) == 0) {
      return (int8_t)i;
    }
  }
  return -1;
}

// The framebuffer as a PNG in the buffer given, or nought for anything that
// went wrong on the way. Nothing here touches the flash or the list, so none of
// it is done holding the lock.
size_t encode(uint8_t *out, size_t cap) {
  // Forty-odd kilobytes of deflate window and line buffers, carried inside the
  // encoder itself: far past what the loop task's stack will take, and not
  // something to put in internal RAM even for a moment. That memory is what a
  // TLS handshake needs forty-eight contiguous kilobytes of, and the band
  // buffers already gave up most of their size to leave it alone. PSRAM makes
  // this slower and it is the right place for it.
  void *room = heap_caps_malloc(sizeof(PNGENC), MALLOC_CAP_SPIRAM);
  // Built where it was found rather than with new, which would take it out of
  // the internal heap this is going to some trouble to stay out of. Without
  // parentheses, so the forty kilobytes inside it are not zeroed on the way to
  // an open() that memsets them itself.
  PNGENC *png = room ? new (room) PNGENC : nullptr;
  // One row turned the right way up, and the encoder's own scratch for the
  // 24-bit line it makes out of it. Small and read for every pixel, so these
  // two stay in the fast memory.
  uint16_t *line = (uint16_t *)malloc((size_t)SCREEN_W * 2);
  uint8_t *temp = (uint8_t *)malloc((size_t)SCREEN_W * 3);
  size_t wrote = 0;

  if (png && line && temp && png->open(out, (int)cap) == PNG_SUCCESS &&
      png->encodeBegin(SCREEN_W, SCREEN_H, PNG_PIXEL_TRUECOLOR, 24, nullptr, LEVEL) ==
          PNG_SUCCESS) {
    uint16_t *fb = boardFramebuffer();
    int rc = PNG_SUCCESS;
    for (int16_t y = 0; y < SCREEN_H && rc == PNG_SUCCESS; y++) {
      // The framebuffer holds the scene turned half a turn, because both boards
      // have their glass mounted that way up and the turn is done where pixels
      // are written rather than by either panel. Undone here, or the file comes
      // out the other way up from the thing that was being looked at.
      const uint16_t *row = boardRow(fb, y);
      for (int16_t x = 0; x < SCREEN_W; x++) {
        line[x] = row[boardX(x)];
      }
      // Big-endian, because the framebuffer already holds the panel's byte
      // order - which is the reverse of this chip's, and precisely what the
      // encoder means by that flag.
      rc = png->addRGB565Line(line, temp, true);
    }
    int32_t size = png->close();
    if (rc == PNG_SUCCESS && size > 0) {
      wrote = (size_t)size;
    }
  }

  free(temp);
  free(line);
  heap_caps_free(room);
  return wrote;
}

}  // namespace

void shotBegin() {
  held = xSemaphoreCreateRecursiveMutex();
  // Formatted on the first boot after the partition table changed under it,
  // which is every board coming from a build before there was one.
  if (!LittleFS.begin(true)) {
    Serial.println("shot: no filesystem, so no screenshots");
    return;
  }
  if (!LittleFS.exists(DIR) && !LittleFS.mkdir(DIR)) {
    Serial.println("shot: nowhere to put screenshots");
    return;
  }
  mounted = true;
  scan();
  Serial.printf("shot: %u of %u kept, %u bytes free\n", count, SHOTS_MAX,
                (unsigned)(LittleFS.totalBytes() - LittleFS.usedBytes()));
}

void shotHold() {
  if (held) {
    xSemaphoreTakeRecursive(held, portMAX_DELAY);
  }
}

void shotDrop() {
  if (held) {
    xSemaphoreGiveRecursive(held);
  }
}

bool shotTake() {
  if (!mounted) {
    return false;
  }
  // Sized for a PNG that compressed to nothing at all: deflate's worst case is
  // its own input and a rounding error, and its input here is the panel as
  // 24-bit colour. No picture can overrun this, however busy the glass was.
  const size_t cap = (size_t)SCREEN_W * SCREEN_H * 3 + 4096;
  uint8_t *out = (uint8_t *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
  if (!out) {
    Serial.println("shot: no room to encode into");
    return false;
  }

  uint32_t began = millis();
  size_t size = encode(out, cap);
  if (!size) {
    heap_caps_free(out);
    Serial.println("shot: the encoder would not take the frame");
    return false;
  }

  shotHold();
  char name[SHOT_NAME_MAX];
  snprintf(name, sizeof(name), "%s%06lu.png", PREFIX, (unsigned long)(next % NUMBER_MAX));
  char path[PATH_MAX_LEN];
  pathOf(name, path, sizeof(path));

  bool wrote = false;
  File f = LittleFS.open(path, FILE_WRITE);
  if (f) {
    wrote = f.write(out, size) == size;
    f.close();
    // A short write is a full filesystem, and half a PNG on it is worse than
    // none: the page would list a file that no viewer will open.
    if (!wrote) {
      LittleFS.remove(path);
    }
  }
  heap_caps_free(out);

  if (!wrote) {
    shotDrop();
    Serial.printf("shot: %s would not write\n", name);
    return false;
  }
  next++;

  // Written first and only then let into the list, so a write that failed
  // leaves the ten that were already there alone. The eleventh sitting on the
  // flash for the length of this is a file, not a problem.
  char dropped[SHOT_NAME_MAX];
  insert(name, dropped, sizeof(dropped));
  if (dropped[0]) {
    char gone[PATH_MAX_LEN];
    pathOf(dropped, gone, sizeof(gone));
    LittleFS.remove(gone);
  }
  shotDrop();

  Serial.printf("shot: %s, %u bytes in %lu ms%s%s\n", name, (unsigned)size,
                (unsigned long)(millis() - began), dropped[0] ? ", replacing " : "", dropped);
  return true;
}

uint8_t shotCount() {
  shotHold();
  uint8_t n = count;
  shotDrop();
  return n;
}

bool shotAt(uint8_t at, char *name, uint32_t *bytes) {
  shotHold();
  bool found = at < count;
  if (found) {
    snprintf(name, SHOT_NAME_MAX, "%s", names[at]);
    char path[PATH_MAX_LEN];
    pathOf(names[at], path, sizeof(path));
    File f = LittleFS.open(path);
    *bytes = f ? (uint32_t)f.size() : 0;
    if (f) {
      f.close();
    }
  }
  shotDrop();
  return found;
}

bool shotTrash(const char *name) {
  shotHold();
  int8_t at = indexOf(name);
  if (at < 0) {
    shotDrop();
    return false;
  }
  char path[PATH_MAX_LEN];
  pathOf(names[at], path, sizeof(path));
  LittleFS.remove(path);
  for (uint8_t i = (uint8_t)at; i + 1 < count; i++) {
    memcpy(names[i], names[i + 1], SHOT_NAME_MAX);
  }
  count--;
  memset(names[count], 0, SHOT_NAME_MAX);
  shotDrop();
  Serial.printf("shot: %s trashed, %u left\n", name, count);
  return true;
}

File shotOpen(const char *name) {
  shotHold();
  File f;
  if (indexOf(name) >= 0) {
    char path[PATH_MAX_LEN];
    pathOf(name, path, sizeof(path));
    f = LittleFS.open(path);
  }
  shotDrop();
  return f;
}
