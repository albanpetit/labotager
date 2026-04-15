#include "logger.h"
#include "config.h"
#include <Arduino.h>
#include <SPI.h>
#include <SdFat.h>

// SPI1 hardware — pins defined in config.h
static MbedSPI sdSPI(GPIO_SD_MISO, GPIO_SD_MOSI, GPIO_SD_SCK);
static SdFat32  sd;
static bool     sd_ready = false;

// ─── Helpers ──────────────────────────────────────────────────────────────────

static void ensure_log_structure() {
  if (!sd.exists(LOG_DIR)) sd.mkdir(LOG_DIR);
  if (!sd.exists(LOG_FILE)) {
    File32 f = sd.open(LOG_FILE, O_WRONLY | O_CREAT);
    if (f) {
      f.println("datetime,air_temp_c,air_hum_pct,soil_pct,pump,led");
      f.close();
    }
  }
}

static void load_config(Settings &s) {
  File32 f = sd.open(CONFIG_FILE, O_RDONLY);
  if (!f) return;

  char line[64];
  while (f.available()) {
    uint8_t len = 0;
    while (f.available() && len < sizeof(line) - 1) {
      char c = (char)f.read();
      if (c == '\n') break;
      if (c != '\r') line[len++] = c;
    }
    line[len] = '\0';
    if (len == 0 || line[0] == '#') continue;

    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq        = '\0';
    const char *key = line;
    int         val = atoi(eq + 1);

    if      (strcmp(key, "soil_threshold")    == 0) s.soil_threshold     = (uint8_t)val;
    else if (strcmp(key, "watering_check_s")  == 0) s.watering_check_s   = (uint16_t)val;
    else if (strcmp(key, "led_start_hour")    == 0) s.led_start_hour = (uint8_t)val;
    else if (strcmp(key, "led_start_min")     == 0) s.led_start_min  = (uint8_t)val;
    else if (strcmp(key, "led_end_hour")      == 0) s.led_end_hour   = (uint8_t)val;
    else if (strcmp(key, "led_end_min")       == 0) s.led_end_min    = (uint8_t)val;
    else if (strcmp(key, "log_interval_s")    == 0) s.log_interval_s     = (uint16_t)val;
    else if (strcmp(key, "growth_days")       == 0) s.growth_days        = (uint16_t)val;
    else if (strcmp(key, "owner_name")        == 0) {
      strncpy(s.owner_name, eq + 1, sizeof(s.owner_name) - 1);
      s.owner_name[sizeof(s.owner_name) - 1] = '\0';
    }
  }
  f.close();
  if (Serial) Serial.println("[SD] Config loaded from " CONFIG_FILE);
}

// ─── Public ───────────────────────────────────────────────────────────────────

bool logger_init(Settings &settings) {
  SdSpiConfig cfg(GPIO_SD_CS, DEDICATED_SPI, SD_SCK_MHZ(SD_SPEED_MHZ), &sdSPI);
  if (!sd.begin(cfg)) {
    if (Serial) Serial.println("[SD] Init failed — card missing or unreadable (SPI1, CS=GP13)");
    return false;
  }
  if (Serial) Serial.println("[SD] Card initialized successfully");
  sd_ready = true;

  ensure_log_structure();
  load_config(settings);
  return true;
}

// NOTE: logger_update contains no Serial output.
// USB CDC Serial.println() blocks indefinitely when the host disconnects —
// even with an if(Serial) guard (race condition). SD state is visible via
// data.sd_error (shown on the display error screen).
void logger_update(SensorData &data, const Settings &settings) {
  static uint32_t last_retry  = 0;
  static uint32_t last_health = 0;

  // ── Periodic retry if SD is unavailable ───────────────────────────────────
  if (!sd_ready) {
    data.sd_error = true;
    uint32_t now  = millis();
    if (now - last_retry >= 5000UL) {
      last_retry = now;
      SdSpiConfig cfg(GPIO_SD_CS, DEDICATED_SPI, SD_SCK_MHZ(SD_SPEED_MHZ), &sdSPI);
      if (sd.begin(cfg)) {
        sd_ready      = true;
        data.sd_error = false;
        ensure_log_structure();
       }
    }
    return;
  }

  data.sd_error = false;

  // ── Periodic SD health check (every 10 s) — detects card removal ──────────
  uint32_t now = millis();
  if (now - last_health >= 10000UL) {
    last_health = now;
    if (!sd.exists(LOG_DIR)) {
      sd_ready      = false;
      last_retry    = now;
      data.sd_error = true;
      return;
    }
  }

  // ── Periodic CSV write ────────────────────────────────────────────────────
  now = millis();
  if (now - data.last_log_millis < (uint32_t)settings.log_interval_s * 1000UL) return;
  data.last_log_millis = now;

  File32 f = sd.open(LOG_FILE, O_WRONLY | O_APPEND);
  if (!f) {
    sd_ready      = false;
    last_retry    = millis();
    data.sd_error = true;
    return;
  }

  char buf[80];
  snprintf(buf, sizeof(buf),
      "%04d-%02d-%02dT%02d:%02d:%02d,%.2f,%.1f,%d,%d,%d",
      data.year, data.month, data.day,
      data.hour, data.minute, data.second,
      data.air_temp, data.air_humidity,
      data.soil_pct,
      data.pump_on ? 1 : 0,
      data.led_on  ? 1 : 0);
  f.println(buf);
  f.close();

  data.last_save_hour  = data.hour;
  data.last_save_min   = data.minute;
  data.last_save_day   = data.day;
  data.last_save_month = data.month;
  data.last_save_year  = data.year;
  data.has_last_save   = true;
}

void logger_save_settings(const Settings &s) {
  if (!sd_ready) {
    if (Serial) Serial.println("[SD] Cannot save settings — card not available");
    return;
  }

  // Write to a temp file first so the original config.txt is never truncated
  // before the new content is fully on disk.  A reset mid-write leaves the
  // old file intact; only after a successful close do we replace it.
  static const char *TMP_FILE = "/config.tmp";

  File32 f = sd.open(TMP_FILE, O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) {
    if (Serial) Serial.println("[SD] Failed to open temp config file for writing");
    return;
  }

  f.print("soil_threshold=");     f.println(s.soil_threshold);
  f.print("watering_check_s=");   f.println(s.watering_check_s);
  f.print("led_start_hour="); f.println(s.led_start_hour);
  f.print("led_start_min=");  f.println(s.led_start_min);
  f.print("led_end_hour=");   f.println(s.led_end_hour);
  f.print("led_end_min=");    f.println(s.led_end_min);
  f.print("log_interval_s=");     f.println(s.log_interval_s);
  f.print("growth_days=");        f.println(s.growth_days);
  f.print("owner_name=");         f.println(s.owner_name);
  f.close();

  // Replace config.txt atomically: remove old, rename temp into place.
  sd.remove(CONFIG_FILE);
  if (!sd.rename(TMP_FILE, CONFIG_FILE)) {
    if (Serial) Serial.println("[SD] Failed to rename temp config to " CONFIG_FILE);
    return;
  }

  if (Serial) Serial.println("[SD] Settings saved to " CONFIG_FILE);
}
