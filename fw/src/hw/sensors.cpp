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

#define AHT20_MAX_FAILURES      3

// AHT20 I2C command bytes (datasheet section 5.4)
#define AHT20_CMD_INIT          0xBE   // initialisation / calibration command
#define AHT20_CMD_INIT_P1       0x08   // parameter 1: enable calibration
#define AHT20_CMD_INIT_P2       0x00   // parameter 2: reserved
#define AHT20_CMD_TRIGGER       0xAC   // start measurement command
#define AHT20_CMD_TRIGGER_P1    0x33   // parameter 1: fixed per datasheet
#define AHT20_CMD_TRIGGER_P2    0x00   // parameter 2: reserved
#define AHT20_STATUS_CAL_BIT    0x08   // bit 3 of status byte: calibration done
#define AHT20_STATUS_BUSY_BIT   0x80   // bit 7 of status byte: measurement in progress

// AHT20 measurement timing (datasheet section 5.4)
#define AHT20_CYCLE_MS          2000   // minimum interval between measurement cycles (ms)
#define AHT20_CONV_MS             80   // conversion time after trigger (ms)
#define AHT20_INIT_SETTLE_MS      10   // settle time after sending the init/calibration command
#define AHT20_FRAME_BYTES          6   // bytes returned by a single measurement read

// Soil moisture ADC
#define SOIL_READ_INTERVAL_MS    500   // ADC sampling interval (ms)

// AHT20 raw-to-physical conversion (datasheet section 6.1)
// Both humidity and temperature use 20-bit unsigned values (range 0..2^20-1).
//   Humidity (%RH) = raw_hum  * 100 / 2^20
//   Temp     (°C)  = raw_temp * 200 / 2^20 - 50
#define AHT20_RAW_MAX           1048576.0f   // 2^20
#define AHT20_TEMP_RANGE_C      200.0f       // full-scale temperature span (°C)
#define AHT20_TEMP_OFFSET_C     50.0f        // offset subtracted after scaling

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

  // Calibrate if needed (CAL bit not set in status)
  if (!(status & AHT20_STATUS_CAL_BIT)) {
    rtcWire.beginTransmission(AHT20_ADDR);
    rtcWire.write(AHT20_CMD_INIT);
    rtcWire.write(AHT20_CMD_INIT_P1);
    rtcWire.write(AHT20_CMD_INIT_P2);
    rtcWire.endTransmission();
    delay(AHT20_INIT_SETTLE_MS);

    rtcWire.requestFrom((uint8_t)AHT20_ADDR, (uint8_t)1);
    raw    = rtcWire.read();
    status = (raw >= 0) ? (uint8_t)raw : 0x00;
    if (!(status & AHT20_STATUS_CAL_BIT)) return false;   // calibration failed
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
  if (now - last_soil >= SOIL_READ_INTERVAL_MS) {
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
    if (now - aht_last_cycle >= AHT20_CYCLE_MS) {
      // Send measurement trigger command
      rtcWire.beginTransmission(AHT20_ADDR);
      rtcWire.write(AHT20_CMD_TRIGGER);
      rtcWire.write(AHT20_CMD_TRIGGER_P1);
      rtcWire.write(AHT20_CMD_TRIGGER_P2);
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
    // Wait for conversion to complete
    if (now - aht_trigger_ms >= AHT20_CONV_MS) {
      aht_state      = AHT_IDLE;
      aht_last_cycle = now;

      uint8_t got = rtcWire.requestFrom((uint8_t)AHT20_ADDR, (uint8_t)AHT20_FRAME_BYTES);
      uint8_t d[AHT20_FRAME_BYTES];
      for (int i = 0; i < AHT20_FRAME_BYTES; i++) {
        int b = rtcWire.read();
        d[i]  = (b >= 0) ? (uint8_t)b : 0xFF;
      }

      // Sanity checks — treat any bad read as a failure
      if (got == 0 || (d[0] == 0xFF && d[1] == 0xFF)) {
        aht_record_failure(data);
        return;
      }
      if (d[0] & AHT20_STATUS_BUSY_BIT) return;   // sensor still busy — skip this reading (not a failure)

      // Successful read — reset the failure counter
      aht_fail_count = 0;

      uint32_t raw_hum  = ((uint32_t)d[1] << 12) | ((uint32_t)d[2] << 4) | (d[3] >> 4);
      uint32_t raw_temp = (((uint32_t)d[3] & 0x0F) << 16) | ((uint32_t)d[4] << 8) | d[5];

      data.air_humidity = (float)raw_hum  * 100.0f / AHT20_RAW_MAX;
      data.air_temp     = (float)raw_temp * AHT20_TEMP_RANGE_C / AHT20_RAW_MAX - AHT20_TEMP_OFFSET_C;
      data.aht20_ready  = true;
    }
  }
}
