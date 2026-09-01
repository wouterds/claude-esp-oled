#include <Arduino.h>
#include <driver/spi_master.h>

#include "esp_lcd_st77916.h"
#include "panel.h"
#include "pins.h"
#include "st77916_waveshare.h"

// The backlight, driven as the vendor drives it. This is an IPS panel: black is
// the crystal blocking the backlight rather than a pixel that is off, and it
// never blocks all of it - so this duty is the only control over how black the
// black behind the scene looks, and it dims the scene with it.
static constexpr int BL_FREQUENCY = 20000;
static constexpr int BL_RESOLUTION = 10;
static constexpr int BL_MAX = 1023;

static bool dimmable = false;

static esp_lcd_panel_io_handle_t newPanelIo(uint32_t pclkHz,
                                            esp_lcd_panel_io_color_trans_done_cb_t done) {
  esp_lcd_panel_io_spi_config_t io = {};
  io.cs_gpio_num = LCD_CS;
  io.dc_gpio_num = -1;
  io.spi_mode = 0;
  io.pclk_hz = pclkHz;
  io.trans_queue_depth = 10;
  io.lcd_cmd_bits = 32;
  io.lcd_param_bits = 8;
  io.flags.quad_mode = true;
  io.on_color_trans_done = done;

  esp_lcd_panel_io_handle_t handle = nullptr;
  if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io, &handle) != ESP_OK) {
    return nullptr;
  }
  return handle;
}

// Two revisions of this panel exist and they want different power and gamma
// registers. Waveshare tells them apart by reading register 0x04, and this
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

  esp_lcd_panel_io_handle_t slow = newPanelIo(5 * 1000 * 1000, nullptr);
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

bool panelBegin(esp_lcd_panel_handle_t *out, esp_lcd_panel_io_color_trans_done_cb_t done) {
  st77916_vendor_config_t vendor = {};
  vendor.flags.use_qspi_interface = 1;
  chooseInit(vendor);

  // 80MHz is what the board's own driver runs this panel at, and it is really
  // arriving: asked for 40 instead, a frame's flush goes from about 6.9ms to
  // 10.3ms, which is the 3.4ms of wire time doubling. Worth writing down because
  // none of these pins is one of the S3's IOMUX SPI pins, and the reference
  // manual puts the GPIO matrix ceiling at 40MHz - so the number looks like it
  // cannot be honoured, and on this panel it is.
  esp_lcd_panel_io_handle_t io = newPanelIo(80 * 1000 * 1000, done);
  if (!io) {
    Serial.println("panel io failed");
    return false;
  }

  esp_lcd_panel_dev_config_t dev = {};
  dev.reset_gpio_num = LCD_RST;
  dev.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  dev.bits_per_pixel = 16;
  dev.vendor_config = &vendor;
  if (esp_lcd_new_panel_st77916(io, &dev, out) != ESP_OK) {
    Serial.println("panel create failed");
    return false;
  }

  esp_err_t rc = esp_lcd_panel_reset(*out);
  Serial.printf("panel reset: %s\n", esp_err_to_name(rc));
  rc = esp_lcd_panel_init(*out);
  Serial.printf("panel init: %s\n", esp_err_to_name(rc));
  if (rc != ESP_OK) {
    return false;
  }
  // A quarter turn, done by the panel rather than by the drawing. MADCTL's MV
  // bit swaps the controller's own axes, so the address window a flush asks for
  // is transposed with everything else and a band of rows lands as a band of
  // columns - which is the only reason a partial flush survives this. The panel
  // is square, so nothing has to be reshaped to fit.
  esp_lcd_panel_swap_xy(*out, true);
  esp_lcd_panel_mirror(*out, false, true);

  rc = esp_lcd_panel_disp_on_off(*out, true);
  Serial.printf("panel on: %s\n", esp_err_to_name(rc));

  // PWM rather than a pin held high, which is how the board's own driver runs
  // it, and what makes the brightness adjustable at all.
  dimmable = ledcAttach(LCD_BL, BL_FREQUENCY, BL_RESOLUTION);
  return true;
}

void panelBrightness(uint8_t percent) {
  if (!dimmable) {
    // A dark panel with a healthy init reads as a dead board, and the dimmer is
    // not worth that - full brightness beats nothing at all.
    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, percent > 0 ? HIGH : LOW);
    Serial.println("backlight: pwm unavailable, pin held high");
    return;
  }
  ledcWrite(LCD_BL, (int)percent * BL_MAX / 100);
}
