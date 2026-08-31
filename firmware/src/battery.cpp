#include "battery.h"

#include <Arduino.h>
#include <Wire.h>

#include "bus.h"
#include "pins.h"

namespace {

constexpr uint8_t REG_VOLTAGE = 0x08;
constexpr uint8_t REG_CURRENT = 0x0C;
constexpr uint8_t REG_FLAGS = 0x0A;
// Bit zero of the flags word: the gauge has decided the pack is discharging.
// It is the gauge's own answer to the question, rather than a threshold guessed
// at from a current reading that is zero for the first second after a boot and
// zero again whenever a full pack sits on a charger.
constexpr uint16_t FLAG_DISCHARGING = 0x0001;
constexpr uint8_t REG_CHARGE = 0x2C;
constexpr uint8_t REG_FULL_CAPACITY = 0x12;
constexpr uint8_t REG_OPERATION = 0x3A;
constexpr uint8_t REG_DESIGN_CAPACITY = 0x3C;
// Where a data memory write goes: the address and the data into 0x3E and up,
// then the checksum and the length into 0x60.
constexpr uint8_t REG_SUBCLASS = 0x3E;
constexpr uint8_t REG_MAC_SUM = 0x60;

// The pack Waveshare ships for the case. The gauge powers off the pack it is
// measuring, so running the board flat browns the gauge out, and it comes back
// with its learned capacity reset to a design figure that was never this pack's
// - 3000 mAh against a 500 mAh cell, which puts a full battery at a third.
constexpr uint16_t PACK_MAH = 500;
// Data memory, not standard commands. The gauge keeps a CEDV profile here and
// these are the two entries in it that say how big the pack is.
constexpr uint16_t DM_DESIGN_CAPACITY = 0x929F;
constexpr uint16_t DM_FULL_CAPACITY = 0x929D;
// Bit ten of the operation status: the gauge has stopped gauging and will accept
// a write to its data memory.
constexpr uint16_t OP_CFGUPDATE = 0x0400;
// Subcommands, written as a word. Unsealing takes two keys and then two more
// for the full access that a data memory write needs; the reinit on the way out
// is what makes the gauge work its charge out again from the cell in front of
// it, rather than carry on from the figure it browned out holding.
constexpr uint16_t CMD_UNSEAL_1 = 0x0414;
constexpr uint16_t CMD_UNSEAL_2 = 0x3672;
constexpr uint16_t CMD_FULL_ACCESS = 0xFFFF;
constexpr uint16_t CMD_ENTER_CFGUPDATE = 0x0090;
constexpr uint16_t CMD_EXIT_CFGUPDATE_REINIT = 0x0091;
constexpr uint16_t CMD_SEAL = 0x0030;
// A control write needs settling time before the next one lands, and a data
// memory write needs longer still before it can be read back. Both are off the
// working sequence rather than off the datasheet, whose own account of this is
// known to be wrong.
constexpr uint32_t SETTLE_MS = 5;
constexpr uint32_t COMMITTED_MS = 10;
// Often enough that plugging the cable in changes the icon while your hand is
// still on it. Three word reads is nothing now that the bus is quiet.
constexpr uint32_t EVERY_MS = 500;
// Not discharging is the useful question, not whether charge is going in. A
// full pack on a charger draws nothing and would read as running on battery,
// which is the one time you most want to see that it is plugged in. On battery
// the board pulls a hundred milliamps and more, so the two are never close.
constexpr int16_t DISCHARGING_MA = -20;

BatteryState cached = {0, 0, false, false};
uint32_t nextRead = 0;
// Whether the gauge answered, which is a different question from whether there
// is a pack on it. A board that does not carry the part never grows one, and
// asking it twice a second is a failing transaction twice a second - which the
// I2C driver is entitled to log, and which then buries everything else.
bool wired = false;

// Every register on this part is a little-endian pair, and a read is a write of
// the address followed by a read without releasing the bus.
bool readWord(uint8_t reg, uint16_t &out) {
  busTake();
  Wire.beginTransmission(GAUGE_ADDR);
  Wire.write(reg);
  bool got = Wire.endTransmission(false) == 0 && Wire.requestFrom((int)GAUGE_ADDR, 2) == 2;
  if (got) {
    uint8_t low = Wire.read();
    uint8_t high = Wire.read();
    out = (uint16_t)((high << 8) | low);
  }
  busGive();
  return got;
}

bool writeBytes(uint8_t reg, const uint8_t *data, uint8_t len) {
  busTake();
  Wire.beginTransmission(GAUGE_ADDR);
  Wire.write(reg);
  Wire.write(data, len);
  bool ok = Wire.endTransmission(true) == 0;
  busGive();
  return ok;
}

bool writeWord(uint8_t reg, uint16_t value) {
  const uint8_t pair[2] = {(uint8_t)(value & 0xFF), (uint8_t)(value >> 8)};
  return writeBytes(reg, pair, 2);
}

// One entry of the profile. The address goes in little end first and the value
// big end first, which is the one asymmetry in the whole sequence, and then the
// checksum stands for both of them together.
bool writeMemory(uint16_t address, uint16_t value) {
  const uint8_t body[4] = {(uint8_t)(address & 0xFF), (uint8_t)(address >> 8),
                           (uint8_t)(value >> 8), (uint8_t)(value & 0xFF)};
  if (!writeBytes(REG_SUBCLASS, body, sizeof(body))) {
    return false;
  }
  delay(1);
  uint8_t sum = 0;
  for (uint8_t i = 0; i < sizeof(body); i++) {
    sum = (uint8_t)(sum + body[i]);
  }
  // The length counts the address, the value, this checksum and itself.
  const uint8_t tail[2] = {(uint8_t)(0xFF - sum), (uint8_t)(sizeof(body) + 2)};
  bool ok = writeBytes(REG_MAC_SUM, tail, sizeof(tail));
  delay(COMMITTED_MS);
  return ok;
}

void controlWord(uint16_t command) {
  writeWord(0x00, command);
  delay(SETTLE_MS);
}

// Tell the gauge how big the pack actually is. Only ever called when it has it
// wrong, because this lands in non-volatile memory and there is no reason to
// spend a write on a figure that is already right.
bool teachCapacity() {
  controlWord(CMD_UNSEAL_1);
  controlWord(CMD_UNSEAL_2);
  controlWord(CMD_FULL_ACCESS);
  controlWord(CMD_FULL_ACCESS);

  // This one subcommand goes to the memory window rather than to control.
  if (!writeWord(REG_SUBCLASS, CMD_ENTER_CFGUPDATE)) {
    Serial.println("battery: could not ask the gauge to take a new profile");
    return false;
  }
  uint16_t status = 0;
  bool ready = false;
  for (uint8_t tries = 0; tries < 200 && !ready; tries++) {
    delay(10);
    ready = readWord(REG_OPERATION, status) && (status & OP_CFGUPDATE) != 0;
  }
  if (!ready) {
    Serial.printf("battery: gauge would not open its profile (status %04x)\n", status);
    return false;
  }

  bool wrote = writeMemory(DM_DESIGN_CAPACITY, PACK_MAH);
  wrote = writeMemory(DM_FULL_CAPACITY, PACK_MAH) && wrote;

  controlWord(CMD_EXIT_CFGUPDATE_REINIT);
  delay(COMMITTED_MS);
  controlWord(CMD_SEAL);
  return wrote;
}

void sample() {
  uint16_t millivolts = 0;
  if (!readWord(REG_VOLTAGE, millivolts)) {
    cached = {0, 0, false, false};
    return;
  }

  uint16_t percent = 0;
  uint16_t flags = 0;
  readWord(REG_CHARGE, percent);
  bool told = readWord(REG_FLAGS, flags);

  cached.millivolts = millivolts;
  cached.percent = percent > 100 ? 100 : (uint8_t)percent;
  if (told) {
    cached.charging = (flags & FLAG_DISCHARGING) == 0;
  }
  // A gauge with nothing on its terminals still answers; it just reads flat.
  cached.present = millivolts >= 2500;
}

// Read back rather than trust the write. A profile write that silently did
// nothing looks exactly like one that worked, and the figure being checked
// against is the whole point of having done it.
void teachIfWrong() {
  uint16_t design = 0;
  if (!readWord(REG_DESIGN_CAPACITY, design) || design == PACK_MAH) {
    return;
  }
  Serial.printf("battery: gauge says the pack is %u mAh, teaching it %u\n", design, PACK_MAH);
  if (!teachCapacity()) {
    Serial.println("battery: teaching the gauge failed, leaving it alone");
    return;
  }
  uint16_t full = 0;
  readWord(REG_DESIGN_CAPACITY, design);
  readWord(REG_FULL_CAPACITY, full);
  Serial.printf("battery: gauge now says design %u mAh, full-charge %u mAh\n", design, full);
  sample();
  Serial.printf("battery: %u mV now reads as %u%%\n", cached.millivolts, cached.percent);
}

}  // namespace

void batteryBegin() {
  busTake();
  Wire.beginTransmission(GAUGE_ADDR);
  wired = Wire.endTransmission() == 0;
  busGive();
  if (!wired) {
    Serial.println("battery: no gauge on this board");
    return;
  }

  sample();
  nextRead = millis() + EVERY_MS;
  if (!cached.present) {
    Serial.printf("battery: none attached or flat (%u mV)\n", cached.millivolts);
    return;
  }
  uint16_t current = 0;
  uint16_t flags = 0;
  readWord(REG_CURRENT, current);
  readWord(REG_FLAGS, flags);
  Serial.printf("battery: %u mV, %u%%, %s (flags %04x, %d mA)\n", cached.millivolts,
                cached.percent, cached.charging ? "external power" : "on battery", flags,
                (int)(int16_t)current);

  teachIfWrong();
}

BatteryState batteryRead() {
  if (!wired) {
    return cached;
  }
  uint32_t now = millis();
  if ((int32_t)(now - nextRead) >= 0) {
    nextRead = now + EVERY_MS;
    sample();
  }
  return cached;
}
