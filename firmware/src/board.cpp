#include "board.h"

#include <Arduino.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "panel.h"
#include "pins.h"

// The panel is fed a band at a time out of a buffer the DMA can actually read.
// The framebuffer lives in PSRAM, which leaves it out of reach of the SPI DMA -
// so each band is copied down into internal RAM on its way out. The copy is a
// straight memcpy because the framebuffer already holds the panel's byte order;
// see boardColour.
// A budget in bytes rather than a count of rows, and a small one, because this
// is the scarcest memory on the board: internal RAM the DMA can reach. Two
// buffers sized off the panel's width came to seventy-four kilobytes of it on
// the AMOLED, and what that costs is not drawing - it is the radio. A TLS
// handshake wants a contiguous thirty-odd kilobytes and there is only so much
// internal RAM to begin with, so the panel holding that much of it is the
// difference between a handshake that fits and one that comes back
// `BIGNUM - Memory allocation failed`, which reads as the network being down.
//
// Sixteen kilobytes is still a transfer worth setting up - four hundred
// microseconds of wire against a few tens of a of setup - and past that the
// extra rows buy nothing. The row count each flush cuts from this is worked out
// below and kept even there, so nothing here has to be.
static constexpr size_t BAND_BYTES = 16 * 1024;

// esp_lcd queues a colour transfer and returns while the DMA is still reading
// the buffer it was handed. Refilling that buffer for the next band walks over
// a transfer in flight, and the panel shows bands from two different frames at
// once - which reads as the panel being broken rather than as a race.
static SemaphoreHandle_t bandSent = nullptr;

static bool bandDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(bandSent, &woken);
  return woken == pdTRUE;
}

static esp_lcd_panel_handle_t panel = nullptr;
static uint16_t *framebuffer = nullptr;
// Two of them, alternating. The copy out of PSRAM and the transfer out of the
// buffer are the two halves of a flush and neither needs the other: filling the
// next band while the panel is still taking the last one costs a second buffer
// and hides whichever of the two is quicker behind the other.
static uint16_t *band[2] = {nullptr, nullptr};

bool boardBegin(uint8_t brightness) {
  bandSent = xSemaphoreCreateBinary();
  framebuffer = (uint16_t *)heap_caps_malloc((size_t)SCREEN_W * SCREEN_H * 2, MALLOC_CAP_SPIRAM);
  band[0] = (uint16_t *)heap_caps_malloc(BAND_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  band[1] = (uint16_t *)heap_caps_malloc(BAND_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!framebuffer || !band[0] || !band[1]) {
    Serial.println("no room for a framebuffer");
    return false;
  }
  memset(framebuffer, 0, (size_t)SCREEN_W * SCREEN_H * 2);

  // The vendor's config macro is designated initialisers in declaration order
  // that C++ will not take, so the fields go in by hand.
  spi_bus_config_t bus = {};
  bus.sclk_io_num = LCD_SCK;
  bus.data0_io_num = LCD_D0;
  bus.data1_io_num = LCD_D1;
  bus.data2_io_num = LCD_D2;
  bus.data3_io_num = LCD_D3;
  bus.max_transfer_sz = (int)BAND_BYTES + 64;
  if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
    Serial.println("spi bus init failed");
    return false;
  }

  if (!panelBegin(&panel, bandDone)) {
    return false;
  }
  Serial.printf("panel up, psram free %u\n", (unsigned)ESP.getFreePsram());
  boardFlush();

  // Only now. The panel powers up holding whatever was last in its RAM, and
  // lighting it before the first black frame lands shows that to the room.
  panelBrightness(brightness);
  Serial.printf("brightness: %u%%\n", brightness);
  return true;
}

uint16_t *boardFramebuffer() { return framebuffer; }

void boardFlush() { boardFlushRows(0, SCREEN_H - 1); }

void boardFlushRows(int16_t from, int16_t to) { boardFlushRect(0, SCREEN_W - 1, from, to); }

// Only the box that changed. The face is a third of the panel and the label a
// twentieth of it, so most frames leave the rest of the glass alone - and the
// wire, not the drawing, is what a frame waits on.
void boardFlushRect(int16_t x0, int16_t x1, int16_t from, int16_t to) {
  if (from < 0) {
    from = 0;
  }
  if (to > SCREEN_H - 1) {
    to = SCREEN_H - 1;
  }
  if (x0 < 0) {
    x0 = 0;
  }
  if (x1 > SCREEN_W - 1) {
    x1 = SCREEN_W - 1;
  }
  if (from > to || x0 > x1) {
    return;
  }
  // The AMOLED's controller ignores the low bit of a window coordinate, so a
  // window asked to start on an odd row or column lands a pixel off instead of
  // failing. The face's box starts wherever the face is, so its parity changed
  // frame to frame - rows parked static figures one pixel up or down depending
  // on which frame last sent them, and columns left slivers of the face's own
  // edge behind it, a pixel to the side of where anything would clear them.
  // Even starts, odd ends, both axes, always; the bands below keep it because
  // their row counts stay even.
  if (LCD_EVEN_WINDOWS) {
    from = (int16_t)(from & ~1);
    to = (int16_t)(to | 1);
    x0 = (int16_t)(x0 & ~1);
    x1 = (int16_t)(x1 | 1);
  }
  // A narrower box holds more rows in the same buffer, so the number of bands
  // goes down as the box gets thinner rather than the buffer going to waste.
  // Kept even, or every second band would start on an odd row after all.
  const int16_t wide = (int16_t)(x1 - x0 + 1);
  int16_t bandRows = (int16_t)((BAND_BYTES / ((size_t)wide * 2)) & ~(size_t)1);
  if (bandRows < 2) {
    bandRows = 2;
  }
  // Fill, hand over, fill the other one while that one goes out. The wait moves
  // to just before the next hand-over, which is the whole point: the copy for the
  // band after this one has already happened by the time the panel is ready for
  // it. Only ever one transfer outstanding, so the one semaphore still answers
  // for it.
  uint8_t slot = 0;
  bool flying = false;
  for (int16_t y = from; y <= to; y += bandRows) {
    int16_t rows = (y + bandRows <= to + 1) ? bandRows : (int16_t)(to + 1 - y);
    // A row of the box at a time now, because the box is not the whole row.
    for (int16_t r = 0; r < rows; r++) {
      memcpy(band[slot] + (size_t)r * wide,
             framebuffer + (int32_t)(y + r) * SCREEN_W + x0, (size_t)wide * 2);
    }
    if (flying) {
      xSemaphoreTake(bandSent, portMAX_DELAY);
    }
    // Nothing was queued if this refused, so there is no transfer to wait on -
    // and the wait above is for a done callback that never comes, with the whole
    // scene frozen behind it while the rest of the board carries on answering as
    // if the panel were fine. Giving up on the frame is the recoverable half of
    // that. Safe to leave here: the take above means the previous band's DMA has
    // finished, so both buffers are ours again.
    if (esp_lcd_panel_draw_bitmap(panel, x0, y, x1 + 1, y + rows, band[slot]) != ESP_OK) {
      Serial.println("flush: the panel refused a band");
      return;
    }
    flying = true;
    slot ^= 1;
  }
  // The last one is still going, and the buffer is not ours again until it says.
  if (flying) {
    xSemaphoreTake(bandSent, portMAX_DELAY);
  }
}
