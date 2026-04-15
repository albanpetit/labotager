#include "sensors.h"
#include "config.h"
#include <Arduino.h>
#include <Wire.h>

// Shared I2C bus (defined in main.cpp)
extern MbedI2C rtcWire;

// ─── AHT20 — non-blocking state machine ───────────────────────────────────────
// MbedI2C::available() can return 0 even when bytes are ready.
// Workaround: bypass available() and call read() directly after requestFrom().
//
// Failure handling:
//   When the AHT20 disappears mid-operation (loose wire, power glitch), the
//   shared I2C bus (also used by DS3231M) can lock up. To prevent this from
//   hanging the whole loop, we count consecutive communication failures.
//   After AHT20_MAX_FAILURES failures, I2C communication with AHT20 is
//   permanently disabled for this boot — the DS3231M continues unaffected.

#define AHT20_MAX_FAILURES  3

enum AhtState { AHT_IDLE, AHT_TRIGGERED };

static AhtState  aht_state        = AHT_IDLE;
static uint32_t  aht_trigger_ms   = 0;
static uint32_t  aht_last_cycle   = 0;   // millis() of the last completed measurement cycle
static uint8_t   aht_fail_count   = 0;   // consecutive I2C failures
static bool      aht_disabled     = false;

static bool aht20_detect_and_init() {
  // Check device presence on I2C bus
  rtcWire.beginTransmission(AHT20_ADDR);
  if (rtcWire.endTransmission() != 0) return false;

  // Read status byte
  rtcWire.requestFrom((uint8_t)AHT20_ADDR, (uint8_t)1);
  int raw = rtcWire.read();
  uint8_t status = (raw >= 0) ? (uint8_t)raw : 0x00;

  // Calibrate if needed (bit 3 = CAL flag)
  if (!(status & 0x08)) {
    rtcWire.beginTransmission(AHT20_ADDR);
    rtcWire.write(0xBE);
    rtcWire.write(0x08);
    rtcWire.write(0x00);
    rtcWire.endTransmission();
    delay(10);

    rtcWire.requestFrom((uint8_t)AHT20_ADDR, (uint8_t)1);
    raw    = rtcWire.read();
    status = (raw >= 0) ? (uint8_t)raw : 0x00;
    if (!(status & 0x08)) return false;   // calibration failed
  }
  return true;
}

// Record one I2C failure; disable AHT20 after AHT20_MAX_FAILURES consecutive faults.
// No Serial output here — USB CDC Serial.println() can block if host disconnects
// (race condition between the if(Serial) check and the actual write).
static void aht_record_failure(SensorData &data) {
  aht_fail_count++;
  data.aht20_ready = false;
  if (aht_fail_count >= AHT20_MAX_FAILURES && !aht_disabled) {
    aht_disabled = true;
  }
}

// ─── Public ───────────────────────────────────────────────────────────────────

void sensors_init() {
  analogReadResolution(12);

  bool ok = aht20_detect_and_init();
  if (ok) {
    if (Serial) Serial.println("[SENSORS] AHT20 detected and calibrated (I2C 0x38)");
  } else {
    if (Serial) Serial.println("[SENSORS] AHT20 not detected — disabling I2C polling (SDA=GP0, SCL=GP1, addr=0x38)");
    // Disable immediately — no loop-time I2C retries if absent at boot.
    aht_disabled = true;
  }
}

void sensors_update(SensorData &data) {
  uint32_t now = millis();

  // ── Soil moisture (ADC, every 500 ms) ────────────────────────────────────
  static uint32_t last_soil = 0;
  if (now - last_soil >= 500) {
    last_soil     = now;
    data.soil_raw = analogRead(GPIO_SOIL);
    data.soil_pct = (uint8_t)constrain(
        map(data.soil_raw, SOIL_DRY_VAL, SOIL_WET_VAL, 0, 100), 0, 100);
  }

  // ── AHT20 non-blocking measurement cycle (every 2 s) ─────────────────────
  // Skip entirely if the sensor has been disabled due to repeated I2C failures.
  data.aht20_error = aht_disabled;
  if (aht_disabled) return;

  if (aht_state == AHT_IDLE) {
    if (now - aht_last_cycle >= 2000) {
      // Send measurement trigger command
      rtcWire.beginTransmission(AHT20_ADDR);
      rtcWire.write(0xAC);
      rtcWire.write(0x33);
      rtcWire.write(0x00);
      uint8_t err = (uint8_t)rtcWire.endTransmission();
      if (err == 0) {
        aht_trigger_ms = now;
        aht_state      = AHT_TRIGGERED;
      } else {
        // Trigger failed — the device may have disappeared
        aht_last_cycle = now;   // back-off before retrying
        aht_record_failure(data);
      }
    }
  } else {
    // Wait 80 ms for conversion, then read result
    if (now - aht_trigger_ms >= 80) {
      aht_state      = AHT_IDLE;
      aht_last_cycle = now;

      uint8_t got = rtcWire.requestFrom((uint8_t)AHT20_ADDR, (uint8_t)6);
      uint8_t d[6];
      for (int i = 0; i < 6; i++) {
        int b = rtcWire.read();
        d[i]  = (b >= 0) ? (uint8_t)b : 0xFF;
      }

      // Sanity checks — treat any bad read as a failure
      if (got == 0 || (d[0] == 0xFF && d[1] == 0xFF)) {
        aht_record_failure(data);
        return;
      }
      if (d[0] & 0x80) return;   // sensor still busy — skip this reading (not a failure)

      // Successful read — reset the failure counter
      aht_fail_count = 0;

      uint32_t raw_hum  = ((uint32_t)d[1] << 12) | ((uint32_t)d[2] << 4) | (d[3] >> 4);
      uint32_t raw_temp = (((uint32_t)d[3] & 0x0F) << 16) | ((uint32_t)d[4] << 8) | d[5];

      data.air_humidity = (float)raw_hum  * 100.0f / 1048576.0f;
      data.air_temp     = (float)raw_temp * 200.0f / 1048576.0f - 50.0f;
      data.aht20_ready  = true;
    }
  }
}
