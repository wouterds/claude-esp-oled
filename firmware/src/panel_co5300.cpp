#include <Arduino.h>
#include <driver/spi_master.h>

#include "esp_lcd_co5300.h"
#include "panel.h"
#include "pins.h"

// The visible glass does not start at the controller's first column: its RAM is
// wider than the 466 that are lit, and the panel's own examples address it from
// column six.
//
// Six is right for a panel written the way round it is scanned. This one is
// mirrored below, and the gap is added to the address *before* the controller
// mirrors it - so a window of 6..471 comes out at the far end of the frame
// instead, two columns off. Those two columns at the near edge are then never
// addressed at all, and hold whatever was in the panel's RAM before this
// firmware ever ran: they survive a reboot, they survive a reflash, and they
// look like a stray line drawn just outside the bars that nothing will clear.
//
// The gap that lands the window back on the glass is the frame's width less the
// last visible column - eight on a 480 wide frame.
static constexpr int X_GAP = 8;

static esp_lcd_panel_handle_t lit = nullptr;

bool panelBegin(esp_lcd_panel_handle_t *out, esp_lcd_panel_io_color_trans_done_cb_t done) {
  esp_lcd_panel_io_spi_config_t io = {};
  io.cs_gpio_num = LCD_CS;
  io.dc_gpio_num = -1;
  io.spi_mode = 0;
  // 80MHz, which is what the LCD board's glass runs at. This board's own Arduino
  // examples use 40 - but that is the graphics library's default for every panel
  // it drives rather than anything this one asked for, and at 40 a full 466x466
  // frame is 22ms of wire time on its own, which is the frame rate's ceiling
  // before anything has been drawn. Halving it is worth about twelve frames a
  // second here. A QSPI bus run past what the panel will take does not fail, it
  // corrupts the picture - so this is the first thing to put back if the glass
  // starts showing torn or speckled frames.
  io.pclk_hz = 80 * 1000 * 1000;
  io.trans_queue_depth = 10;
  io.lcd_cmd_bits = 32;
  io.lcd_param_bits = 8;
  io.flags.quad_mode = true;
  io.on_color_trans_done = done;

  esp_lcd_panel_io_handle_t handle = nullptr;
  if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io, &handle) != ESP_OK) {
    Serial.println("panel io failed");
    return false;
  }

  co5300_vendor_config_t vendor = {};
  vendor.flags.use_qspi_interface = 1;

  esp_lcd_panel_dev_config_t dev = {};
  dev.reset_gpio_num = LCD_RST;
  dev.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  dev.bits_per_pixel = 16;
  dev.vendor_config = &vendor;
  if (esp_lcd_new_panel_co5300(handle, &dev, out) != ESP_OK) {
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

  esp_lcd_panel_set_gap(*out, X_GAP, 0);
  // A half turn, undoing the one the drawing applies. Every scene here is
  // written into the framebuffer upside down because the LCD board's glass is
  // mounted that way; this one is not, so the panel turns it back rather than
  // the drawing being taught which board it is on.
  esp_lcd_panel_mirror(*out, true, true);

  rc = esp_lcd_panel_disp_on_off(*out, true);
  Serial.printf("panel on: %s\n", esp_err_to_name(rc));

  lit = *out;
  return true;
}

void panelBrightness(uint8_t percent) {
  if (!lit) {
    return;
  }
  // No backlight on this board. The pixels emit, so this is a register on the
  // panel and black really is black whatever it is set to.
  esp_err_t rc = esp_lcd_panel_co5300_set_brightness(lit, percent);
  Serial.printf("brightness: %u%% (%s)\n", percent, esp_err_to_name(rc));
}
