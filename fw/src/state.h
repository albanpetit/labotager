#pragma once
#include <stdint.h>

// Sensor readings + RTC timestamp, updated every loop cycle
struct SensorData {
  // AHT20 air sensor
  float    air_temp;          // ambient temperature (°C)
  float    air_humidity;      // ambient relative humidity (%RH)
  bool     aht20_ready;       // true once the sensor is initialised and returning valid data

  // Soil moisture (ADC)
  int      soil_raw;          // raw ADC value (0–4095)
  uint8_t  soil_pct;          // mapped soil moisture (0–100 %)

  // RTC DS3231M
  uint8_t  hour;
  uint8_t  minute;
  uint8_t  second;
  uint8_t  day;
  uint8_t  month;
  uint16_t year;
  bool     rtc_ready;         // true once the RTC is initialised and running

  // Actuator states (mirrored here for the logger and display)
  bool     pump_on;
  bool     led_on;

  // Hardware errors (set during init or after repeated failures)
  bool     aht20_error;       // true when AHT20 is permanently disabled (max I2C failures)
  bool     rtc_error;         // true when DS3231M failed to initialise

  // Logger
  bool     sd_error;          // true when the SD card is absent or inaccessible

  // Last successful CSV save (displayed on Details screen)
  uint8_t  last_save_hour;
  uint8_t  last_save_min;
  uint8_t  last_save_day;
  uint8_t  last_save_month;
  uint16_t last_save_year;
  bool     has_last_save;
};

// User-configurable parameters (loaded from /config.txt, persisted to SD)
struct Settings {
  uint8_t  soil_threshold;        // pump trigger threshold, % (default 5)
  int8_t   plant_temp_min;        // cold alert threshold (°C, default 15)
  int8_t   plant_temp_max;        // heat alert threshold (°C, default 30)
  uint16_t watering_check_s;      // seconds between pump re-checks (default 10)
  bool     watering_enabled;      // allow watering cycles (default true)
  uint16_t watering_duration_s;   // max pump on time per cycle in seconds (default 5)
  uint16_t watering_cooldown_min; // min time between pump cycles in minutes (default 30)

  uint8_t  led_start_hour;        // grow light on hour (default 8)
  uint8_t  led_start_min;         // grow light on minute (default 0)
  uint8_t  led_end_hour;          // grow light off hour (default 22)
  uint8_t  led_end_min;           // grow light off minute (default 0)

  uint16_t log_interval_s;        // seconds between CSV entries (default 300)

  uint8_t  grow_start_day;        // grow start date — day   (0 = not set)
  uint8_t  grow_start_month;      // grow start date — month (0 = not set)
  uint16_t grow_start_year;       // grow start date — year  (0 = not set)
  char     owner_name[32];        // owner display name (shown on Home)
};
