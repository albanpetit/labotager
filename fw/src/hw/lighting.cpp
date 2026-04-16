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
  uint16_t now_min   = minutes_since_midnight(data.hour,              data.minute);
  uint16_t start_min = minutes_since_midnight(settings.led_start_hour, settings.led_start_min);
  uint16_t end_min   = minutes_since_midnight(settings.led_end_hour,   settings.led_end_min);

  bool on;
  if (start_min == end_min) {
    on = false;  // start == end → no window defined
  } else if (start_min < end_min) {
    // Normal window (e.g. 08:00 → 22:00)
    on = (now_min >= start_min && now_min < end_min);
  } else {
    // Overnight window (e.g. 22:00 → 06:00)
    on = (now_min >= start_min || now_min < end_min);
  }

  if (on != data.led_on) {
    data.led_on = on;
    digitalWrite(GPIO_LED, on ? HIGH : LOW);
    // No Serial output — USB CDC Serial.println() can block if host disconnects.
    // LED state (led_on) is captured in the CSV log via logger_update().
  }
}
