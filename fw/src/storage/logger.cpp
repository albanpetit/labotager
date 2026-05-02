#include "logger.h"
#include "config.h"
#include <Arduino.h>
#include <SPI.h>
#include <SdFat.h>

// SD timing constants
#define SD_RETRY_INTERVAL_MS   5000   // delay before re-attempting SD init after failure (ms)
#define SD_HEALTH_INTERVAL_MS 10000   // interval between SD card health checks (ms)
#define CSV_LOG_LINE_LEN         80   // "N,DD/MM/YYYY,HH:MM:SS,GGGG,±TT.TT,HHH.H,SSS,DDDD\0" ~58 chars; 80 gives margin

// SPI1 hardware — pins defined in config.h
static MbedSPI   sdSPI(GPIO_SD_MISO, GPIO_SD_MOSI, GPIO_SD_SCK);
static SdFat32   sd;
static bool      sd_ready = false;
static uint32_t  log_id   = 0;   // row counter — resumed from file at boot, incremented on each write

// ─── Helpers ──────────────────────────────────────────────────────────────────

// Read the ID from the last data row in LOG_FILE.
// O(1): seeks to the tail of the file rather than reading every byte.
// Called at init and on SD reconnect so log_id resumes across reboots.
static const uint16_t LOG_TAIL_LEN = 128;

static uint32_t read_last_log_id() {
  File32 f = sd.open(LOG_FILE, O_RDONLY);
  if (!f) return 0;
  uint32_t fsize = f.fileSize();
  if (fsize == 0) { f.close(); return 0; }

  uint32_t seek_pos = (fsize > LOG_TAIL_LEN) ? fsize - LOG_TAIL_LEN : 0;
  if (!f.seekSet(seek_pos)) { f.close(); return 0; }

  char buf[LOG_TAIL_LEN + 1];
  int got = (int)f.read(buf, LOG_TAIL_LEN);
  f.close();
  if (got <= 0) return 0;
  buf[got] = '\0';

  // Strip trailing CR/LF
  int end = got - 1;
  while (end >= 0 && (buf[end] == '\n' || buf[end] == '\r')) end--;
  if (end < 0) return 0;
  buf[end + 1] = '\0';

  // Find start of the last line
  char *line = buf;
  for (int i = end; i >= 0; i--) {
    if (buf[i] == '\n') { line = &buf[i + 1]; break; }
  }

  // The header row starts with "id," — no data rows yet
  if (strncmp(line, "id,", 3) == 0) return 0;

  char *comma = strchr(line, ',');
  if (!comma) return 0;
  *comma = '\0';
  char *endptr;
  long id = strtol(line, &endptr, 10);
  if (endptr == line || id <= 0) return 0;
  return (uint32_t)id;
}

// Julian Day Number — Fliegel & Van Flandern integer formula (CACM vol.11, 1968).
static int32_t logger_julian_day(int d, int m, int y) {
  return 367L*y - 7*(y+(m+9)/12)/4 + 275*m/9 + d + 1721013L;
}

// Days elapsed since grow start. Returns 0 if grow date is not set or RTC is unavailable.
static int32_t grow_day_number(const SensorData &data, const Settings &s) {
  if (s.grow_start_day == 0 || !data.rtc_ready) return 0;
  int32_t diff = logger_julian_day(data.day, data.month, data.year)
               - logger_julian_day(s.grow_start_day, s.grow_start_month, s.grow_start_year);
  return diff < 0 ? 0 : diff;
}

// Scheduled daily LED-on duration in minutes derived from the lighting window.
static uint16_t led_daily_duration_min(const Settings &s) {
  uint16_t start = (uint16_t)s.led_start_hour * 60u + s.led_start_min;
  uint16_t end   = (uint16_t)s.led_end_hour   * 60u + s.led_end_min;
  if (start == end) return 0;
  return (end > start) ? (uint16_t)(end - start)
                       : (uint16_t)(MINUTES_PER_DAY - start + end);
}

static void ensure_log_structure() {
  if (!sd.exists(LOG_DIR)) sd.mkdir(LOG_DIR);
  if (!sd.exists(LOG_FILE)) {
    File32 f = sd.open(LOG_FILE, O_WRONLY | O_CREAT);
    if (f) {
      f.println("id,date,heure,jour_croissance,temp_air_c,hum_air_pct,hum_sol_pct,duree_eclairage_min");
      f.close();
    }
  }
}

// Clamp all numeric settings to their valid ranges after loading from disk.
// Guards against hand-edited or corrupted config.txt producing invalid state
// (e.g. led_start_hour=99, plant_temp_min=-200).
static void settings_sanitize(Settings &s) {
  s.soil_threshold   = (uint8_t) constrain(s.soil_threshold,   0,    100);
  s.plant_temp_min   = (int8_t)  constrain(s.plant_temp_min,   -40,  60);
  s.plant_temp_max   = (int8_t)  constrain(s.plant_temp_max,   -40,  60);
  s.watering_check_s    = (uint16_t)constrain(s.watering_check_s,    1,    3600);
  s.watering_enabled    = s.watering_enabled ? true : false;
  s.watering_duration_s = (uint16_t)constrain(s.watering_duration_s, 1,    300);
  s.watering_cooldown_min = (uint16_t)constrain(s.watering_cooldown_min, 1, 1440);
  s.led_start_hour   = (uint8_t) constrain(s.led_start_hour,   0,    23);
  s.led_start_min    = (uint8_t) constrain(s.led_start_min,    0,    59);
  s.led_end_hour     = (uint8_t) constrain(s.led_end_hour,     0,    23);
  s.led_end_min      = (uint8_t) constrain(s.led_end_min,      0,    59);
  s.log_interval_s   = (uint16_t)constrain(s.log_interval_s,   1,    86400);
  s.grow_start_day   = (uint8_t) constrain(s.grow_start_day,   0,    31);
  s.grow_start_month = (uint8_t) constrain(s.grow_start_month, 0,    12);
  // grow_start_year: 0 = not set; otherwise clamp to a plausible range
  if (s.grow_start_year != 0)
    s.grow_start_year = (uint16_t)constrain(s.grow_start_year, 2020, 2099);
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
    *eq             = '\0';
    const char *key = line;
    char       *endptr;
    long        val_long = strtol(eq + 1, &endptr, 10);
    if (endptr == eq + 1) continue;   // no valid digits — skip malformed line
    int val = (int)val_long;

    if      (strcmp(key, "soil_threshold")    == 0) s.soil_threshold = (uint8_t)val;
    else if (strcmp(key, "plant_temp_min")    == 0) s.plant_temp_min = (int8_t)val;
    else if (strcmp(key, "plant_temp_max")    == 0) s.plant_temp_max = (int8_t)val;
    else if (strcmp(key, "watering_check_s")     == 0) s.watering_check_s     = (uint16_t)val;
    else if (strcmp(key, "watering_enabled")     == 0) s.watering_enabled     = (val != 0);
    else if (strcmp(key, "watering_duration_s")  == 0) s.watering_duration_s  = (uint16_t)val;
    else if (strcmp(key, "watering_cooldown_min")== 0) s.watering_cooldown_min= (uint16_t)val;
    else if (strcmp(key, "led_start_hour")    == 0) s.led_start_hour = (uint8_t)val;
    else if (strcmp(key, "led_start_min")     == 0) s.led_start_min  = (uint8_t)val;
    else if (strcmp(key, "led_end_hour")      == 0) s.led_end_hour   = (uint8_t)val;
    else if (strcmp(key, "led_end_min")       == 0) s.led_end_min    = (uint8_t)val;
    else if (strcmp(key, "log_interval_s")    == 0) s.log_interval_s   = (uint16_t)val;
    else if (strcmp(key, "grow_start_day")    == 0) s.grow_start_day   = (uint8_t)val;
    else if (strcmp(key, "grow_start_month")  == 0) s.grow_start_month = (uint8_t)val;
    else if (strcmp(key, "grow_start_year")   == 0) s.grow_start_year  = (uint16_t)val;
    else if (strcmp(key, "owner_name")        == 0) {
      strncpy(s.owner_name, eq + 1, sizeof(s.owner_name) - 1);
      s.owner_name[sizeof(s.owner_name) - 1] = '\0';
      // Strip trailing CR/LF that may remain after the line read
      int olen = strlen(s.owner_name);
      while (olen > 0 && (s.owner_name[olen-1] == '\r' || s.owner_name[olen-1] == '\n'))
        s.owner_name[--olen] = '\0';
    }
  }
  f.close();
  settings_sanitize(s);
  if (Serial) Serial.println("[SD] Config loaded from " CONFIG_FILE);
}

// ─── Public ───────────────────────────────────────────────────────────────────

void settings_apply_defaults(Settings &s) {
  s.soil_threshold   = DEFAULT_SOIL_THRESHOLD;
  s.plant_temp_min   = DEFAULT_PLANT_TEMP_MIN;
  s.plant_temp_max   = DEFAULT_PLANT_TEMP_MAX;
  s.watering_check_s      = DEFAULT_WATERING_CHECK_S;
  s.watering_enabled      = DEFAULT_WATERING_ENABLED;
  s.watering_duration_s   = DEFAULT_WATERING_DURATION_S;
  s.watering_cooldown_min = DEFAULT_WATERING_COOLDOWN_MIN;
  s.led_start_hour   = DEFAULT_LED_START_HOUR;
  s.led_start_min    = DEFAULT_LED_START_MIN;
  s.led_end_hour     = DEFAULT_LED_END_HOUR;
  s.led_end_min      = DEFAULT_LED_END_MIN;
  s.log_interval_s   = DEFAULT_LOG_INTERVAL_S;
  s.grow_start_day   = 0;
  s.grow_start_month = 0;
  s.grow_start_year  = 0;
  strncpy(s.owner_name, DEFAULT_OWNER_NAME, sizeof(s.owner_name) - 1);
  s.owner_name[sizeof(s.owner_name) - 1] = '\0';
}

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
  log_id = read_last_log_id();   // resume row counter so IDs are unique across reboots
  return true;
}

// NOTE: logger_update contains no Serial output.
// USB CDC Serial.println() blocks indefinitely when the host disconnects —
// even with an if(Serial) guard (race condition). SD state is visible via
// data.sd_error (shown on the display error screen).
void logger_update(SensorData &data, const Settings &settings, void (*kick_wdt)()) {
  static uint32_t last_retry  = 0;
  static uint32_t last_health = 0;

  // ── Periodic retry if SD is unavailable ───────────────────────────────────
  if (!sd_ready) {
    data.sd_error = true;
    uint32_t now  = millis();
    if (now - last_retry >= SD_RETRY_INTERVAL_MS) {
      last_retry = now;
      SdSpiConfig cfg(GPIO_SD_CS, DEDICATED_SPI, SD_SCK_MHZ(SD_SPEED_MHZ), &sdSPI);
      if (sd.begin(cfg)) {
        sd_ready      = true;
        data.sd_error = false;
        ensure_log_structure();
        log_id = read_last_log_id();
       }
    }
    return;
  }

  data.sd_error = false;

  // ── Periodic SD health check (every 10 s) — detects card removal ──────────
  uint32_t now = millis();
  if (now - last_health >= SD_HEALTH_INTERVAL_MS) {
    last_health = now;
    if (!sd.exists(LOG_DIR)) {
      sd_ready      = false;
      last_retry    = now;
      data.sd_error = true;
      return;
    }
  }

  // ── Periodic CSV write ────────────────────────────────────────────────────
  static uint32_t last_log_ms = 0;
  static bool     first_write = true;
  now = millis();
  if (!first_write && now - last_log_ms < (uint32_t)settings.log_interval_s * 1000UL) return;
  first_write = false;
  last_log_ms = now;

  // Require a valid RTC timestamp — a row with no date is useless.
  if (!data.rtc_ready) return;

  // Kick the watchdog before the SD write — a slow card can block for ~1-2 s
  // which, combined with other loop work, risks approaching the 8 s timeout.
  if (kick_wdt) kick_wdt();

  File32 f = sd.open(LOG_FILE, O_WRONLY | O_APPEND);
  if (!f) {
    sd_ready      = false;
    last_retry    = millis();
    data.sd_error = true;
    return;
  }

  char buf[CSV_LOG_LINE_LEN];
  if (data.aht20_ready) {
    snprintf(buf, sizeof(buf),
        "%lu,%02d/%02d/%04d,%02d:%02d:%02d,%ld,%.2f,%.1f,%d,%u",
        (unsigned long)++log_id,
        data.day, data.month, data.year,
        data.hour, data.minute, data.second,
        (long)grow_day_number(data, settings),
        data.air_temp, data.air_humidity,
        data.soil_pct,
        (unsigned)led_daily_duration_min(settings));
  } else {
    snprintf(buf, sizeof(buf),
        "%lu,%02d/%02d/%04d,%02d:%02d:%02d,%ld,,,%d,%u",
        (unsigned long)++log_id,
        data.day, data.month, data.year,
        data.hour, data.minute, data.second,
        (long)grow_day_number(data, settings),
        data.soil_pct,
        (unsigned)led_daily_duration_min(settings));
  }
  f.print(buf);
  f.print('\n');
  f.close();

  data.last_save_hour  = data.hour;
  data.last_save_min   = data.minute;
  data.last_save_day   = data.day;
  data.last_save_month = data.month;
  data.last_save_year  = data.year;
  data.has_last_save   = true;
}

void logger_save_settings(const Settings &s, void (*kick_wdt)()) {
  if (!sd_ready) {
    if (Serial) Serial.println("[SD] Cannot save settings — card not available");
    return;
  }

  // Write to a temp file first so the original config.txt is never truncated
  // before the new content is fully on disk.  A reset mid-write leaves the
  // old file intact; only after a successful close do we replace it.
  static const char *TMP_FILE = "/config.tmp";

  if (kick_wdt) kick_wdt();

  File32 f = sd.open(TMP_FILE, O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) {
    if (Serial) Serial.println("[SD] Failed to open temp config file for writing");
    return;
  }

  f.print("soil_threshold="); f.println(s.soil_threshold);
  f.print("plant_temp_min="); f.println(s.plant_temp_min);
  f.print("plant_temp_max="); f.println(s.plant_temp_max);
  f.print("watering_check_s=");      f.println(s.watering_check_s);
  f.print("watering_enabled=");      f.println(s.watering_enabled ? 1 : 0);
  f.print("watering_duration_s=");   f.println(s.watering_duration_s);
  f.print("watering_cooldown_min="); f.println(s.watering_cooldown_min);
  f.print("led_start_hour="); f.println(s.led_start_hour);
  f.print("led_start_min=");  f.println(s.led_start_min);
  f.print("led_end_hour=");   f.println(s.led_end_hour);
  f.print("led_end_min=");    f.println(s.led_end_min);
  f.print("log_interval_s=");     f.println(s.log_interval_s);
  f.print("grow_start_day=");     f.println(s.grow_start_day);
  f.print("grow_start_month=");   f.println(s.grow_start_month);
  f.print("grow_start_year=");    f.println(s.grow_start_year);
  f.print("owner_name=");         f.println(s.owner_name);
  f.close();

  // Replace config.txt atomically: remove old, rename temp into place.
  sd.remove(CONFIG_FILE);
  if (!sd.rename(TMP_FILE, CONFIG_FILE)) {
    if (Serial) Serial.println("[SD] Failed to rename temp config to " CONFIG_FILE);
    sd.remove(TMP_FILE);
    return;
  }

  if (Serial) Serial.println("[SD] Settings saved to " CONFIG_FILE);
}
