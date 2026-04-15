#include "lighting.h"
#include "config.h"
#include <Arduino.h>

// ─── Public ───────────────────────────────────────────────────────────────────

void lighting_init() {
  pinMode(GPIO_LED, OUTPUT);
  digitalWrite(GPIO_LED, LOW);
}

void lighting_update(SensorData &data, const Settings &settings) {
  if (!data.rtc_ready) return;

  // Convert current time and schedule boundaries to minutes since midnight
  uint16_t now_min   = (uint16_t)data.hour * 60 + data.minute;
  uint16_t start_min = (uint16_t)settings.led_start_hour * 60 + settings.led_start_min;
  uint16_t end_min   = start_min + (uint16_t)settings.led_duration_hours * 60;

  bool on;
  if (end_min <= 1440) {
    // Schedule fits within the same day
    on = (now_min >= start_min && now_min < end_min);
  } else {
    // Schedule wraps past midnight (e.g. 20:00 + 16 h → ends at 12:00 next day)
    uint16_t end_wrap = end_min - 1440;
    on = (now_min >= start_min || now_min < end_wrap);
  }

  if (on != data.led_on) {
    data.led_on = on;
    digitalWrite(GPIO_LED, on ? HIGH : LOW);
    // No Serial output — USB CDC Serial.println() can block if host disconnects.
    // LED state (led_on) is captured in the CSV log via logger_update().
  }
}
