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

// Full. Both panels reach it by different means and neither has a reason not to.
static constexpr uint8_t BRIGHTNESS = 100;

// The panel is fed a band at a time out of a buffer the DMA can actually read.
// The framebuffer lives in PSRAM, which leaves it out of reach of the SPI DMA -
// so each band is copied down into internal RAM on its way out. The copy is a
// straight memcpy because the framebuffer already holds the panel's byte order;
// see boardColour.
static constexpr int BAND_ROWS = 40;
static constexpr size_t BAND_BYTES = (size_t)SCREEN_W * BAND_ROWS * 2;

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

bool boardBegin() {
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
  panelBrightness(BRIGHTNESS);
  return true;
}

uint16_t *boardFramebuffer() { return framebuffer; }

void boardFlush() { boardFlushRows(0, SCREEN_H - 1); }

// Only the rows that changed. The face is a third of the panel and the label a
// twentieth of it, so most frames leave the rest of the glass alone - and the
// wire, not the drawing, is what a frame waits on.
void boardFlushRows(int16_t from, int16_t to) {
  if (from < 0) {
    from = 0;
  }
  if (to > SCREEN_H - 1) {
    to = SCREEN_H - 1;
  }
  // Fill, hand over, fill the other one while that one goes out. The wait moves
  // to just before the next hand-over, which is the whole point: the copy for the
  // band after this one has already happened by the time the panel is ready for
  // it. Only ever one transfer outstanding, so the one semaphore still answers
  // for it.
  uint8_t slot = 0;
  bool flying = false;
  for (int16_t y = from; y <= to; y += BAND_ROWS) {
    int16_t rows = (y + BAND_ROWS <= to + 1) ? BAND_ROWS : (to + 1 - y);
    memcpy(band[slot], framebuffer + (int32_t)y * SCREEN_W, (size_t)rows * SCREEN_W * 2);
    if (flying) {
      xSemaphoreTake(bandSent, portMAX_DELAY);
    }
    esp_lcd_panel_draw_bitmap(panel, 0, y, SCREEN_W, y + rows, band[slot]);
    flying = true;
    slot ^= 1;
  }
  // The last one is still going, and the buffer is not ours again until it says.
  if (flying) {
    xSemaphoreTake(bandSent, portMAX_DELAY);
  }
}
