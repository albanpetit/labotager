#include "pump.h"
#include "config.h"
#include <Arduino.h>

// ─── Watering state machine ────────────────────────────────────────────────────
// IDLE    : pump OFF.
//           Starts a cycle when: led_on AND soil_pct < threshold AND cooldown elapsed.
// PUMPING : pump ON.
//           Stops when any of:
//             • watering_duration_s elapsed (max cycle duration)
//             • soil_pct >= threshold at next re-check (target reached)
//             • led_on goes false (lighting window closed)
//           After stopping, a cooldown of watering_cooldown_min minutes must
//           pass before the next cycle can begin.
//
// NOTE: Serial.println is intentionally absent from this module.
// On arduino-mbed RP2040, USB CDC Serial.println() blocks indefinitely when
// the host disconnects — even with an `if (Serial)` guard (race condition).
// Pump state changes are captured in the CSV log via logger_update().

enum PumpState { PUMP_IDLE, PUMP_PUMPING };

static PumpState pump_state        = PUMP_IDLE;
static uint32_t  pump_check_ms     = 0;   // last periodic soil re-check
static uint32_t  pump_start_ms     = 0;   // when the current cycle started
static uint32_t  pump_last_stop_ms = 0;   // when the last cycle ended
static bool      pump_ever_run     = false;  // false until the first cycle completes

static void stop_pump(SensorData &data) {
  digitalWrite(GPIO_MOTOR, LOW);
  pump_state        = PUMP_IDLE;
  pump_last_stop_ms = millis();
  pump_ever_run     = true;
  data.pump_on      = false;
}

// ─── Public ───────────────────────────────────────────────────────────────────

void pump_init() {
  pinMode(GPIO_MOTOR, OUTPUT);
  digitalWrite(GPIO_MOTOR, LOW);
}

void pump_update(SensorData &data, const Settings &settings) {
  uint32_t now = millis();

  switch (pump_state) {
    case PUMP_IDLE: {
      bool cooldown_ok = !pump_ever_run ||
                         (now - pump_last_stop_ms >=
                          (uint32_t)settings.watering_cooldown_min * 60000UL);
      if (data.led_on && data.soil_pct < settings.soil_threshold && cooldown_ok) {
        digitalWrite(GPIO_MOTOR, HIGH);
        pump_state    = PUMP_PUMPING;
        pump_start_ms = now;
        pump_check_ms = now;
        data.pump_on  = true;
      }
      break;
    }

    case PUMP_PUMPING:
      // Immediate stop when LEDs turn off (end of the watering window)
      if (!data.led_on) { stop_pump(data); break; }

      // Stop when max cycle duration is reached
      if (now - pump_start_ms >= (uint32_t)settings.watering_duration_s * 1000UL) {
        stop_pump(data);
        break;
      }

      // Periodic soil re-check
      if (now - pump_check_ms >= (uint32_t)settings.watering_check_s * 1000UL) {
        pump_check_ms = now;
        if (data.soil_pct >= settings.soil_threshold) {
          stop_pump(data);
        }
      }
      break;
  }
}
