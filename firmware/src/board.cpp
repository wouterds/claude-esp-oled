#include "board.h"

#include <Arduino.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "esp_lcd_st77916.h"
#include "st77916_waveshare.h"

// Waveshare ESP32-S3-Touch-LCD-1.85B, from the board's own GPIO table. Same
// QSPI pins as the non-B board of this size; reset is not the same, because
// there is no TCA9554 on this one and LCD_RST is a pin of the S3.
static constexpr int LCD_CS = 21;
static constexpr int LCD_SCK = 40;
static constexpr int LCD_D0 = 46;
static constexpr int LCD_D1 = 45;
static constexpr int LCD_D2 = 42;
static constexpr int LCD_D3 = 41;
static constexpr int LCD_RST = 3;
static constexpr int LCD_BL = 5;

// The backlight, driven as the vendor drives it. This is an IPS panel: black is
// the crystal blocking the backlight rather than a pixel that is off, and it
// never blocks all of it - so this duty is the only control over how black the
// black behind the scene looks, and it dims the scene with it.
static constexpr int BL_FREQUENCY = 20000;
static constexpr int BL_RESOLUTION = 10;
static constexpr int BL_DUTY = 1023;

// The panel is fed a band at a time out of a buffer the DMA can actually read.
// The framebuffer itself is 253KB and lives in PSRAM, which leaves it out of
// reach of the SPI DMA - so each band is copied down into internal RAM on its
// way out. The copy is also where the bytes get swapped, which this panel
// wants and which would otherwise cost a second pass over every pixel.
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
static uint16_t *band = nullptr;

static esp_lcd_panel_io_handle_t newPanelIo(uint32_t pclkHz) {
  esp_lcd_panel_io_spi_config_t io = {};
  io.cs_gpio_num = LCD_CS;
  io.dc_gpio_num = -1;
  io.spi_mode = 0;
  io.pclk_hz = pclkHz;
  io.trans_queue_depth = 10;
  io.lcd_cmd_bits = 32;
  io.lcd_param_bits = 8;
  io.flags.quad_mode = true;
  if (bandSent) {
    io.on_color_trans_done = bandDone;
  }

  esp_lcd_panel_io_handle_t handle = nullptr;
  if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io, &handle) != ESP_OK) {
    return nullptr;
  }
  return handle;
}

// Two revisions of this panel exist and they want different power and gamma
// registers. Waveshare tells them apart by reading register 0x04 back, and this
// board answers that read with zeroes - it does not talk back over QSPI here -
// so the probe cannot decide it and the answer was found by trying both.
//
// Revision 1 on this glass renders a flat white fill with a brighter band
// across the middle, which reads as a framebuffer fault and is not one.
// Revision 2 renders it clean, so revision 2 is the default and the probe only
// ever overrides it if it actually manages to read something back.
static void chooseInit(st77916_vendor_config_t &vendor) {
  vendor.init_cmds = vendor_specific_init_version_2;
  vendor.init_cmds_size = sizeof(vendor_specific_init_version_2) / sizeof(st77916_lcd_init_cmd_t);

  esp_lcd_panel_io_handle_t slow = newPanelIo(5 * 1000 * 1000);
  if (!slow) {
    return;
  }
  uint8_t id[4] = {0, 0, 0, 0};
  int cmd = (0x0B << 24) | (0x04 << 8);
  if (esp_lcd_panel_io_rx_param(slow, cmd, id, sizeof(id)) == ESP_OK) {
    Serial.printf("panel id: %02x %02x %02x %02x\n", id[0], id[1], id[2], id[3]);
    if (id[0] == 0x00 && id[1] == 0x7F && id[2] == 0x7F && id[3] == 0x7F) {
      vendor.init_cmds = vendor_specific_init_version_1;
      vendor.init_cmds_size = sizeof(vendor_specific_init_version_1) / sizeof(st77916_lcd_init_cmd_t);
      Serial.println("panel: revision 1");
      esp_lcd_panel_io_del(slow);
      return;
    }
  }
  Serial.println("panel: revision 2");
  esp_lcd_panel_io_del(slow);
}

bool boardBegin() {
  bandSent = xSemaphoreCreateBinary();
  framebuffer = (uint16_t *)heap_caps_malloc((size_t)SCREEN_W * SCREEN_H * 2, MALLOC_CAP_SPIRAM);
  band = (uint16_t *)heap_caps_malloc(BAND_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!framebuffer || !band) {
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

  st77916_vendor_config_t vendor = {};
  vendor.flags.use_qspi_interface = 1;
  chooseInit(vendor);

  esp_lcd_panel_io_handle_t io = newPanelIo(40 * 1000 * 1000);
  if (!io) {
    Serial.println("panel io failed");
    return false;
  }

  esp_lcd_panel_dev_config_t dev = {};
  dev.reset_gpio_num = LCD_RST;
  dev.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  dev.bits_per_pixel = 16;
  dev.vendor_config = &vendor;
  if (esp_lcd_new_panel_st77916(io, &dev, &panel) != ESP_OK) {
    Serial.println("panel create failed");
    return false;
  }

  esp_err_t rc = esp_lcd_panel_reset(panel);
  Serial.printf("panel reset: %s\n", esp_err_to_name(rc));
  rc = esp_lcd_panel_init(panel);
  Serial.printf("panel init: %s\n", esp_err_to_name(rc));
  if (rc != ESP_OK) {
    return false;
  }
  rc = esp_lcd_panel_disp_on_off(panel, true);
  Serial.printf("panel on: %s\n", esp_err_to_name(rc));

  Serial.printf("panel up, psram free %u\n", (unsigned)ESP.getFreePsram());
  boardFlush();

  // Only now. The panel powers up holding whatever was last in its RAM, and
  // lighting it before the first black frame lands shows that to the room.
  //
  // PWM rather than a pin held high, which is how the board's own driver runs
  // it, and what makes the duty above adjustable at all.
  if (ledcAttach(LCD_BL, BL_FREQUENCY, BL_RESOLUTION)) {
    ledcWrite(LCD_BL, BL_DUTY);
    Serial.printf("backlight: pwm at %d/1023\n", BL_DUTY);
  } else {
    // A dark panel with a healthy init reads as a dead board, and the dimmer
    // is not worth that - full brightness beats nothing at all.
    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, HIGH);
    Serial.println("backlight: pwm unavailable, pin held high");
  }
  return true;
}

uint16_t *boardFramebuffer() { return framebuffer; }

void boardFlush() {
  for (int16_t y = 0; y < SCREEN_H; y += BAND_ROWS) {
    int16_t rows = (y + BAND_ROWS <= SCREEN_H) ? BAND_ROWS : (SCREEN_H - y);
    const uint16_t *src = framebuffer + (int32_t)y * SCREEN_W;
    int32_t count = (int32_t)rows * SCREEN_W;
    for (int32_t i = 0; i < count; i++) {
      band[i] = __builtin_bswap16(src[i]);
    }
    esp_lcd_panel_draw_bitmap(panel, 0, y, SCREEN_W, y + rows, band);
    // The buffer is not ours again until the transfer says so.
    xSemaphoreTake(bandSent, portMAX_DELAY);
  }
}
