#pragma once

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <stdint.h>

// What differs between the boards past a pin number: which controller sits
// behind the QSPI bus, and what "brighter" means to it. Exactly one panel_*.cpp
// is compiled, picked by the env in platformio.ini.
//
// The SPI bus, the framebuffer and the banded flush are the same on both and
// stay in board.cpp - only the controller's own quirks live behind this.

// Creates the panel on an already-initialised SPI2_HOST and leaves it on. The
// callback goes to the panel IO, which is how the flush waits on its own DMA.
// False means the panel never came up and there is nothing to draw on.
bool panelBegin(esp_lcd_panel_handle_t *out, esp_lcd_panel_io_color_trans_done_cb_t done);

// 0-100. The LCD dims a backlight beside the glass; the AMOLED has no backlight
// to dim and takes a register on the panel instead.
void panelBrightness(uint8_t percent);
