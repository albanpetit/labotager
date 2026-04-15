#include <Arduino.h>
#include <Wire.h>
#include <mbed.h>
#include <hardware/watchdog.h>   // RP2040 scratch registers — survive watchdog resets

#include "config.h"
#include "state.h"
#include "hw/encoder.h"
#include "hw/sensors.h"
#include "hw/rtc_mgr.h"
#include "hw/pump.h"
#include "hw/lighting.h"
#include "storage/logger.h"
#include "ui/display.h"

// Shared I2C bus for AHT20 (0x38) and DS3231M (0x68).
// Declared here; referenced as extern in sensors.cpp and rtc_mgr.cpp.
MbedI2C rtcWire(I2C_SDA, I2C_SCL);

static SensorData data     = {};
static Settings   settings = {};   // populated with defaults in setup(), then overridden by /config.txt

// ─── Loop stage breadcrumb ────────────────────────────────────────────────────
// watchdog_hw->scratch[0] survives a watchdog reset.
// We write a stage code before each module so that if the board resets,
// setup() can read which module was executing when the freeze happened.
//
// Stage codes (also used as labels in the log):
static const char * const STAGE_NAMES[] = {
  "none",        // 0 — clean boot
  "sensors",     // 1
  "rtc",         // 2
  "pump",        // 3
  "lighting",    // 4
  "logger",      // 5
  "display",     // 6
};
#define STAGE_COUNT  7
#define BREADCRUMB_MAGIC  0xAB00   // upper word — confirms the value was written by us

static inline void set_stage(uint8_t stage) {
  watchdog_hw->scratch[0] = BREADCRUMB_MAGIC | stage;
}

void setup() {
  // 1. Initialise display first so the backlight stays OFF during the full boot
  //    sequence. It turns ON once the screen is filled with black, preventing
  //    the grey noise flash that would otherwise be visible.
  display_init();

  // 2. Apply compile-time defaults (overridden by /config.txt if SD is present)
  settings.soil_threshold     = DEFAULT_SOIL_THRESHOLD;
  settings.plant_temp_min     = DEFAULT_PLANT_TEMP_MIN;
  settings.plant_temp_max     = DEFAULT_PLANT_TEMP_MAX;
  settings.watering_check_s   = DEFAULT_WATERING_CHECK_S;
  settings.led_start_hour     = DEFAULT_LED_START_HOUR;
  settings.led_start_min      = DEFAULT_LED_START_MIN;
  settings.led_end_hour       = DEFAULT_LED_END_HOUR;
  settings.led_end_min        = DEFAULT_LED_END_MIN;
  settings.log_interval_s     = DEFAULT_LOG_INTERVAL_S;
  settings.grow_start_day     = 0;
  settings.grow_start_month   = 0;
  settings.grow_start_year    = 0;
  strncpy(settings.owner_name, DEFAULT_OWNER_NAME, sizeof(settings.owner_name) - 1);

  Serial.begin(SERIAL_BAUD);
  delay(2000);

  // 3. Check if this boot follows a watchdog reset, and if so, report which
  //    module was executing when the loop froze.
  if (watchdog_caused_reboot()) {
    uint32_t crumb = watchdog_hw->scratch[0];
    if ((crumb & 0xFF00) == BREADCRUMB_MAGIC) {
      uint8_t stage = (uint8_t)(crumb & 0xFF);
      const char *name = (stage < STAGE_COUNT) ? STAGE_NAMES[stage] : "unknown";
      if (Serial) { Serial.print("[SYS] WATCHDOG RESET — loop froze in module: "); Serial.println(name); }
    } else {
      if (Serial) Serial.println("[SYS] WATCHDOG RESET — freeze location unknown (no breadcrumb)");
    }
  }
  watchdog_hw->scratch[0] = 0;   // clear breadcrumb for this boot

  rtcWire.begin();

  data.sd_error = !logger_init(settings);   // SD init + load /config.txt
  sensors_init();                           // AHT20 calibration + ADC resolution
  rtc_init();                               // DS3231M clock
  encoder_init();                           // rotary encoder (polling)
  pump_init();                              // watering pump GPIO
  lighting_init();                          // grow light GPIO

  // 4. Enable the hardware watchdog — must be kicked every WATCHDOG_MS milliseconds.
  //    If loop() blocks for any reason (SD hang, I2C lockup, TFT stall…),
  //    the RP2040 resets automatically rather than staying frozen.
  mbed::Watchdog::get_instance().start(WATCHDOG_MS);
  if (Serial) {
    Serial.print("[SYS] Watchdog enabled (");
    Serial.print(WATCHDOG_MS);
    Serial.println(" ms timeout)");
    Serial.println("[SYS] System startup complete");
  }
}

void loop() {
  // Keep the watchdog alive — must be called at least every WATCHDOG_MS ms.
  mbed::Watchdog::get_instance().kick();

  set_stage(1); sensors_update(data);
  set_stage(2); rtc_update(data);

  // Premier démarrage : enregistre la date du jour comme date de début de pousse.
  if (settings.grow_start_day == 0 && data.rtc_ready) {
    settings.grow_start_day   = data.day;
    settings.grow_start_month = data.month;
    settings.grow_start_year  = data.year;
    logger_save_settings(settings);
  }

  set_stage(3); pump_update(data, settings);
  set_stage(4); lighting_update(data, settings);
  set_stage(5); logger_update(data, settings);
  set_stage(6); {
    // Process the first encoder event (or ENC_NONE for periodic refresh).
    // Then drain any events that accumulated in the ISR queue during the render
    // — each one is handled immediately without waiting for the next loop cycle.
    EncEvent ev  = encoder_poll();
    bool changed = display_update(data, settings, ev);
    while ((ev = encoder_poll()) != ENC_NONE) {
      changed |= display_update(data, settings, ev);
    }
    if (changed) logger_save_settings(settings);
  }
  set_stage(0);   // loop completed cleanly
}
