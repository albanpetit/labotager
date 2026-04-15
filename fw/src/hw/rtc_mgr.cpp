#include "rtc_mgr.h"
#include "config.h"
#include <Arduino.h>
#include <Wire.h>
#include <DFRobot_DS3231M.h>

// Shared I2C bus (defined in main.cpp)
extern MbedI2C rtcWire;

static DFRobot_DS3231M rtc(&rtcWire);
static bool rtc_failed = false;

// ─── Public ───────────────────────────────────────────────────────────────────

void rtc_init() {
  if (!rtc.begin()) {
    if (Serial) Serial.println("[RTC] DS3231M not detected on I2C bus (addr=0x68) — check wiring");
    rtc_failed = true;
    return;
  }
  if (Serial) Serial.println("[RTC] DS3231M detected and running");

  if (rtc.lostPower()) {
    if (Serial) Serial.println("[RTC] Oscillator was stopped (OSF flag set) — restoring time from build timestamp");
    rtc.dateTime();
    rtc.adjust();
  }
}

void rtc_update(SensorData &data) {
  data.rtc_error = rtc_failed;
  if (rtc_failed) return;

  static uint32_t last_read = 0;
  if (millis() - last_read < 1000) return;
  last_read = millis();

  rtc.getNowTime();

  data.hour      = rtc.hour();
  data.minute    = rtc.minute();
  data.second    = rtc.second();
  data.day       = rtc.day();
  data.month     = rtc.month();
  data.year      = rtc.year();
  data.rtc_ready = true;
}

// Direct register write to DS3231M over I2C.
// Values are BCD-encoded before writing.
// The DFRobot library uses 1970 as year base: stored byte = year - 1970.
void rtc_set_datetime(uint16_t year, uint8_t month, uint8_t day,
                      uint8_t hour, uint8_t minute, uint8_t second) {
  auto to_bcd = [](uint8_t v) -> uint8_t {
    return ((v / 10) << 4) | (v % 10);
  };

  // DS3231M register map — write starting at address 0x00
  rtcWire.beginTransmission(DS3231_ADDR);
  rtcWire.write(0x00);                           // start address: seconds register
  rtcWire.write(to_bcd(second));                 // 0x00 — seconds
  rtcWire.write(to_bcd(minute));                 // 0x01 — minutes
  rtcWire.write(to_bcd(hour));                   // 0x02 — hours (24-h mode, bits 7:6 = 0)
  rtcWire.write(0x01);                           // 0x03 — day-of-week (unused, set to 1)
  rtcWire.write(to_bcd(day));                    // 0x04 — day-of-month
  rtcWire.write(to_bcd(month));                  // 0x05 — month (century bit 7 = 0)
  rtcWire.write(to_bcd((uint8_t)(year - 1970))); // 0x06 — year offset from 1970 (DFRobot convention)
  rtcWire.endTransmission();

  // Clear the OSF (Oscillator Stop Flag) in status register 0x0F.
  // Without this lostPower() stays true on the next boot and overwrites the time.
  rtcWire.beginTransmission(DS3231_ADDR);
  rtcWire.write(0x0F);
  rtcWire.endTransmission();
  rtcWire.requestFrom((int)DS3231_ADDR, 1);
  uint8_t status = rtcWire.read();
  rtcWire.beginTransmission(DS3231_ADDR);
  rtcWire.write(0x0F);
  rtcWire.write(status & 0x7F);   // clear bit 7 (OSF)
  rtcWire.endTransmission();

  char buf[56];
  snprintf(buf, sizeof(buf), "[RTC] Time set to %04d-%02d-%02d %02d:%02d:%02d",
      year, month, day, hour, minute, second);
  if (Serial) Serial.println(buf);
}
