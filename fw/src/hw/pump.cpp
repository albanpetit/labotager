#include "pump.h"
#include "config.h"
#include <Arduino.h>

// ─── Watering state machine ────────────────────────────────────────────────────
// IDLE    : pump OFF. If soil moisture < threshold AND led_on → switch to PUMPING.
// PUMPING : pump ON. Every watering_check_s seconds re-reads soil_pct.
//           If soil moisture >= threshold OR led_on goes false → switch back to IDLE.
//
// NOTE: Serial.println is intentionally absent from this module.
// On arduino-mbed RP2040, USB CDC Serial.println() blocks indefinitely when
// the host disconnects — even with an `if (Serial)` guard (race condition).
// Pump state changes are captured in the CSV log via logger_update().

enum PumpState { PUMP_IDLE, PUMP_PUMPING };

static PumpState pump_state    = PUMP_IDLE;
static uint32_t  pump_check_ms = 0;   // millis() of the last watering check

// ─── Public ───────────────────────────────────────────────────────────────────

void pump_init() {
  pinMode(GPIO_MOTOR, OUTPUT);
  digitalWrite(GPIO_MOTOR, LOW);
}

void pump_update(SensorData &data, const Settings &settings) {
  uint32_t now = millis();

  switch (pump_state) {
    case PUMP_IDLE:
      if (data.led_on && data.soil_pct < settings.soil_threshold) {
        digitalWrite(GPIO_MOTOR, HIGH);
        pump_state    = PUMP_PUMPING;
        pump_check_ms = now;
        data.pump_on  = true;
      }
      break;

    case PUMP_PUMPING:
      // Immediate stop when LEDs turn off (end of the watering window)
      if (!data.led_on) {
        digitalWrite(GPIO_MOTOR, LOW);
        pump_state   = PUMP_IDLE;
        data.pump_on = false;
        break;
      }
      // Periodic re-check
      if (now - pump_check_ms >= (uint32_t)settings.watering_check_s * 1000UL) {
        pump_check_ms = now;
        if (data.soil_pct >= settings.soil_threshold) {
          digitalWrite(GPIO_MOTOR, LOW);
          pump_state   = PUMP_IDLE;
          data.pump_on = false;
        }
      }
      break;
  }
}
