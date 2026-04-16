#include "display.h"
#include "ui.h"
#include "config.h"
#include "hw/rtc_mgr.h"
#include <Arduino.h>
#include <TFT_eSPI.h>

static TFT_eSPI tft = TFT_eSPI();

// ─── UI state machine ─────────────────────────────────────────────────────────
//
// MODE_TAB         : rotation cycles through tabs (Home ↔ Details ↔ Settings)
// MODE_PARAM_SELECT: inside Settings, rotation moves the cursor
// MODE_PARAM_EDIT  : inside Settings, rotation changes the selected value
//
// Transitions:
//   MODE_TAB          + PRESS (on SCREEN_PARAMS)         → MODE_PARAM_SELECT
//   MODE_PARAM_SELECT + PRESS                            → MODE_PARAM_EDIT (param_edit_sub_idx=0)
//   MODE_PARAM_SELECT + LONG_PRESS                       → MODE_TAB (applies RTC if dirty)
//   MODE_PARAM_EDIT   + PRESS (combined HH:MM row)       → cycle param_edit_sub_idx h↔m
//   MODE_PARAM_EDIT   + LONG_PRESS                       → MODE_PARAM_SELECT (confirms + applies RTC if dirty)

enum UIMode { MODE_TAB, MODE_DETAILS_SCROLL, MODE_PARAM_SELECT, MODE_PARAM_EDIT };

static Screen  current_tab      = SCREEN_HOME;
static UIMode  ui_mode          = MODE_TAB;
static int     param_cursor     = 0;
static int     params_scroll    = 0;
static int     details_cursor   = 0;
static int     details_scroll   = 0;
static bool    settings_dirty   = false;
static bool    need_full_redraw = true;
static uint32_t last_refresh_ms = 0;
static bool    sd_error_shown    = false;

// Hardware diagnostic screens — re-triggered each time an error (re)appears.
// The _displayed flags prevent re-showing while the error is still active.
// The prev_* flags detect error-clear→re-error transitions so a sensor that
// disappears after boot and comes back will produce a fresh diagnostic screen.
static bool     hw_error_aht20_displayed  = false;
static bool     hw_error_rtc_displayed    = false;
static bool     prev_aht20_error          = false;
static bool     prev_rtc_error            = false;
static uint32_t diag_until_ms     = 0;
static const uint32_t DIAG_DURATION_MS = 3000;

// ─── Staged date/time (edited in Settings, applied to RTC on confirm) ─────────

static uint16_t stg_year;
static uint8_t  stg_month, stg_day, stg_hour, stg_min;
static bool     stg_rtc_dirty = false;

// Apply the staged date/time to the RTC if it was modified in Settings.
// Called on LONG_PRESS to confirm edits before returning to the previous mode.
static void commit_staged_rtc() {
  if (!stg_rtc_dirty) return;
  rtc_set_datetime(stg_year, stg_month, stg_day, stg_hour, stg_min, 0);
  stg_rtc_dirty = false;
}

static void init_staged(const SensorData &data) {
  stg_year  = data.year;
  stg_month = data.month;
  stg_day   = data.day;
  stg_hour  = data.hour;
  stg_min   = data.minute;
  stg_rtc_dirty = false;
}

// ─── Editable parameters ──────────────────────────────────────────────────────
//
// Combined rows (multiple sub-fields cycled with ENC_PRESS in edit mode):
//   Index 0 — Time           : HH:MM       (sub 0=h, 1=m)
//   Index 1 — Date           : DD/MM/YYYY  (sub 0=d, 1=m, 2=y)
//   Index 3 — Light start    : HH:MM       (sub 0=h, 1=m)
//   Index 4 — Light end      : HH:MM       (sub 0=h, 1=m)
// ENC_LONG_PRESS confirms and returns to MODE_PARAM_SELECT.

enum ParamIndex {
  PARAM_TIME        = 0,   // combined HH:MM        (stg_hour, stg_min)
  PARAM_DATE        = 1,   // combined DD/MM/YYYY   (stg_day, stg_month, stg_year)
  PARAM_GROW_START  = 2,   // combined DD/MM/YYYY   (grow_start_day/month/year)
  PARAM_LED_START   = 3,   // combined HH:MM        (led_start_hour, led_start_min)
  PARAM_LED_END     = 4,   // combined HH:MM        (led_end_hour, led_end_min)
  PARAM_SOIL        = 5,   // soil moisture threshold %
  PARAM_TEMP_MIN    = 6,   // minimum temperature °C
  PARAM_TEMP_MAX    = 7,   // maximum temperature °C
  PARAM_COUNT       = 8,
};

static const char * const PARAM_LABELS[PARAM_COUNT] = {
  "Heure",            // PARAM_TIME
  "Date",             // PARAM_DATE
  "Debut pousse",     // PARAM_GROW_START
  "Debut eclairage",  // PARAM_LED_START
  "Fin eclairage",    // PARAM_LED_END
  "Seuil humidite %", // PARAM_SOIL
  "Temp min C",       // PARAM_TEMP_MIN
  "Temp max C",       // PARAM_TEMP_MAX
};

// Active sub-field within a combined row:
//   HH:MM rows  (PARAM_TIME, LED_START, LED_END) : 0 = hour, 1 = minute
//   DD/MM/YYYY rows (PARAM_DATE, PARAM_GROW_START): 0 = day,  1 = month, 2 = year
static int param_edit_sub_idx = 0;

// Number of sub-fields in a combined row (0 = not combined)
static int combined_sub_count(int i) {
  if (i == PARAM_TIME || i == PARAM_LED_START || i == PARAM_LED_END) return 2;  // h, m
  if (i == PARAM_DATE || i == PARAM_GROW_START)                       return 3;  // d, m, y
  return 0;
}

static int get_param_val(int i, const Settings &s) {
  switch (i) {
    case PARAM_SOIL:     return s.soil_threshold;
    case PARAM_TEMP_MIN: return s.plant_temp_min;
    case PARAM_TEMP_MAX: return s.plant_temp_max;
    default:             return 0;
  }
}

static void apply_delta(int i, int delta, Settings &s) {
  switch (i) {
    case PARAM_TIME:  // combined HH:MM
      if (param_edit_sub_idx == 0) stg_hour = (uint8_t)constrain((int)stg_hour + delta, 0, 23);
      else                     stg_min  = (uint8_t)constrain((int)stg_min  + delta, 0, 59);
      stg_rtc_dirty = true; break;
    case PARAM_DATE:  // combined DD/MM/YYYY
      if      (param_edit_sub_idx == 0) stg_day   = (uint8_t)constrain((int)stg_day   + delta, 1, 31);
      else if (param_edit_sub_idx == 1) stg_month = (uint8_t)constrain((int)stg_month + delta, 1, 12);
      else                          stg_year  = (uint16_t)constrain((int)stg_year + delta, 2024, 2069);
      stg_rtc_dirty = true; break;
    case PARAM_GROW_START:  // combined DD/MM/YYYY
      if      (param_edit_sub_idx == 0) s.grow_start_day   = (uint8_t)constrain((int)s.grow_start_day   + delta, 1, 31);
      else if (param_edit_sub_idx == 1) s.grow_start_month = (uint8_t)constrain((int)s.grow_start_month + delta, 1, 12);
      else                          s.grow_start_year  = (uint16_t)constrain((int)s.grow_start_year  + delta, 2024, 2069);
      settings_dirty = true; break;
    case PARAM_LED_START:  // combined HH:MM
      if (param_edit_sub_idx == 0) s.led_start_hour = (uint8_t)constrain((int)s.led_start_hour + delta, 0, 23);
      else                     s.led_start_min  = (uint8_t)constrain((int)s.led_start_min  + delta, 0, 59);
      settings_dirty = true; break;
    case PARAM_LED_END: {  // combined HH:MM — cannot equal the start time
      if (param_edit_sub_idx == 0) s.led_end_hour = (uint8_t)constrain((int)s.led_end_hour + delta, 0, 23);
      else                     s.led_end_min  = (uint8_t)constrain((int)s.led_end_min  + delta, 0, 59);
      // If start == end, nudge end one extra minute in the same direction
      if (s.led_end_hour == s.led_start_hour && s.led_end_min == s.led_start_min) {
        uint16_t end_min = minutes_since_midnight(s.led_end_hour, s.led_end_min);
        end_min = (uint16_t)((end_min + delta + MINUTES_PER_DAY) % MINUTES_PER_DAY);
        s.led_end_hour = (uint8_t)(end_min / 60);
        s.led_end_min  = (uint8_t)(end_min % 60);
      }
      settings_dirty = true; break;
    }
    case PARAM_SOIL:
      s.soil_threshold = (uint8_t)constrain(s.soil_threshold + delta, 0, 100);
      settings_dirty = true; break;
    case PARAM_TEMP_MIN:
      s.plant_temp_min = (int8_t)constrain(s.plant_temp_min + delta, -40, 60);
      settings_dirty = true; break;
    case PARAM_TEMP_MAX:
      s.plant_temp_max = (int8_t)constrain(s.plant_temp_max + delta, -40, 60);
      settings_dirty = true; break;
  }
}

// ─── Grow days calculation ────────────────────────────────────────────────────

static int32_t julian_day(int d, int m, int y) {
  return 367L*y - 7*(y+(m+9)/12)/4 + 275*m/9 + d + 1721013L;
}

static int32_t days_elapsed(int d1, int m1, int y1, int d2, int m2, int y2) {
  int32_t diff = julian_day(d2, m2, y2) - julian_day(d1, m1, y1);
  return diff < 0 ? 0 : diff;
}

// ─── Tab bar ──────────────────────────────────────────────────────────────────

static void render_tabbar() {
  ui_draw_menu_background();

  switch (current_tab) {
    case SCREEN_HOME:
      ui_draw_menu_icon_home_selected();
      ui_draw_menu_icon_details();
      ui_draw_menu_icon_settings();
      break;
    case SCREEN_DETAILS:
      ui_draw_menu_icon_home();
      ui_draw_menu_icon_details_selected();
      ui_draw_menu_icon_settings();
      break;
    case SCREEN_PARAMS:
      ui_draw_menu_icon_home();
      ui_draw_menu_icon_details();
      ui_draw_menu_icon_settings_selected();
      break;
  }
}

// ─── Plant state helper ───────────────────────────────────────────────────────
//
// Priority: THIRSTY > HOT > COLD > HAPPY
// Soil moisture is always available (ADC); temperature requires AHT20.
// When AHT20 is unavailable, only the thirsty condition is checked.

static PlantState plant_get_state(const SensorData &data, const Settings &settings) {
  if (data.soil_pct < settings.soil_threshold) return PLANT_THIRSTY;
  if (data.aht20_ready) {
    if (data.air_temp > (float)settings.plant_temp_max) return PLANT_HOT;
    if (data.air_temp < (float)settings.plant_temp_min) return PLANT_COLD;
  }
  return PLANT_HAPPY;
}

// ─── Screen: Home ─────────────────────────────────────────────────────────────
//
// Layout (320 × 200):
//   Y= 12 : owner name               (centered, size 1)
//   Y= 35 : thin separator line
//   Y= 65 : date  DD / MM            (centered, size 2)
//   Y= 95 : time  HH:MM              (centered, size 3)
//   Y=145 : year  YYYY               (centered, size 1)
//   X=PLANT_IMG_X, Y=PLANT_IMG_Y : plant status image (top-right, 64×64 px)
//   Y=168 : grow-days counter        (centered, size 1)

// Redraw only the four live widgets — no background, no owner name, no tabbar.
// Called every minute from the periodic timer when SCREEN_HOME is active.
static void render_home_update(const SensorData &data, const Settings &settings) {
  char buf[32];

  if (data.rtc_ready) {
    snprintf(buf, sizeof(buf), "%02d / %02d", data.day, data.month);
    ui_draw_home_date(buf);
    snprintf(buf, sizeof(buf), "%02d:%02d", data.hour, data.minute);
    ui_draw_home_time(buf);
  } else {
    ui_draw_home_date("--/--");
    ui_draw_home_time("--:--");
  }

  if (settings.grow_start_day > 0 && data.rtc_ready) {
    int32_t days = days_elapsed(settings.grow_start_day, settings.grow_start_month, settings.grow_start_year,
                                data.day, data.month, data.year);
    snprintf(buf, sizeof(buf), "J + %ld", days);
  } else {
    snprintf(buf, sizeof(buf), "J + --");
  }
  ui_draw_home_grow_counter(buf);

  ui_draw_home_plant_status(plant_get_state(data, settings));
}

static void render_home(const SensorData &data, const Settings &settings) {
  ui_draw_background();

  // Owner name (static — only drawn on full render)
  ui_draw_home_owner(settings.owner_name);

  render_home_update(data, settings);
}

// ─── Screen: Details ──────────────────────────────────────────────────────────
//
// Scrollable list — 6 items total, 3 visible at a time.
// Encoder UP/DOWN moves cursor; LONG_PRESS returns to tab navigation.
// Item backgrounds: details_item (normal) / details_item_selected (highlighted).

#define DETAILS_ITEM_COUNT  6
#define DETAILS_ITEMS_SHOWN 3
#define DETAILS_ITEM_Y0     62    // y of first visible item slot
#define DETAILS_ITEM_STEP   34    // vertical spacing between items (px)

static const char * const DETAIL_LABELS[DETAILS_ITEM_COUNT] = {
  "Temperature air",
  "Humidite air",
  "Humidite sol",
  "Pompe",
  "Eclairage LED",
  "Dernier log",
};

static void get_detail_value(int i, const SensorData &data, char *buf, size_t len) {
  switch (i) {
    case 0:
      if (data.aht20_ready) snprintf(buf, len, "%.1f C",   data.air_temp);
      else                  snprintf(buf, len, "-- C");
      break;
    case 1:
      if (data.aht20_ready) snprintf(buf, len, "%.0f %%",  data.air_humidity);
      else                  snprintf(buf, len, "-- %%");
      break;
    case 2: snprintf(buf, len, "%d %%", data.soil_pct); break;
    case 3: snprintf(buf, len, "%s", data.pump_on ? "ON" : "OFF"); break;
    case 4: snprintf(buf, len, "%s", data.led_on  ? "ON" : "OFF"); break;
    case 5:
      if (data.has_last_save)
        snprintf(buf, len, "%02d/%02d %02d:%02d",
                 data.last_save_day, data.last_save_month,
                 data.last_save_hour, data.last_save_min);
      else
        snprintf(buf, len, "--");
      break;
    default: snprintf(buf, len, ""); break;
  }
}

static void render_details_row(int item_idx, int slot, bool selected, const SensorData &data) {
  int y = DETAILS_ITEM_Y0 + slot * DETAILS_ITEM_STEP;
  if (selected) ui_draw_details_item_selected(y);
  else          ui_draw_details_item(y);

  char val[24];
  get_detail_value(item_idx, data, val, sizeof(val));

  int ty = y + 13;
  tft.setFreeFont(UI_FONT);
  tft.setTextSize(UI_FONT_SIZE);
  tft.setTextColor(UI_COLOR_TEXT);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(DETAIL_LABELS[item_idx], 50, ty);
  tft.setTextDatum(TR_DATUM);
  tft.drawString(val, 265, ty);
}

static void render_details(const SensorData &data) {
  ui_draw_background();
  ui_draw_details_background();
  ui_draw_details_title();

  for (int slot = 0; slot < DETAILS_ITEMS_SHOWN; slot++) {
    int item = details_scroll + slot;
    if (item >= DETAILS_ITEM_COUNT) break;
    render_details_row(item, slot, item == details_cursor, data);
  }
}

// ─── Screen: Settings ─────────────────────────────────────────────────────────
//
// Scrollable list — 10 items total, 3 visible at a time.
// Matches the details page layout: same images, same scroll logic.

#define PARAMS_ITEMS_SHOWN  3
#define PARAMS_ITEM_Y0      62
#define PARAMS_ITEM_STEP    34

static void render_param_row(int item_idx, int slot, bool selected, bool editing, const Settings &s) {
  int y = PARAMS_ITEM_Y0 + slot * PARAMS_ITEM_STEP;
  if (selected) ui_draw_params_item_selected(y);
  else          ui_draw_params_item(y);

  char val[12];
  char label[28];
  strncpy(label, PARAM_LABELS[item_idx], sizeof(label) - 1);
  label[sizeof(label) - 1] = '\0';

  if (combined_sub_count(item_idx) > 0) {
    if (item_idx == PARAM_DATE || item_idx == PARAM_GROW_START) {
      // DD/MM/YYYY — source depends on which row we're rendering
      uint8_t  d  = (item_idx == PARAM_DATE) ? stg_day          : s.grow_start_day;
      uint8_t  mo = (item_idx == PARAM_DATE) ? stg_month        : s.grow_start_month;
      uint16_t y  = (item_idx == PARAM_DATE) ? stg_year         : s.grow_start_year;
      snprintf(val, sizeof(val), "%02d/%02d/%04d", d, mo, y);
      if (editing) {
        const char *sub_names[] = { "d", "m", "y" };
        snprintf(label, sizeof(label), "%s (%s)", PARAM_LABELS[item_idx], sub_names[param_edit_sub_idx]);
      }
    } else {
      // PARAM_TIME, PARAM_LED_START, PARAM_LED_END: HH:MM
      int h, m;
      if      (item_idx == PARAM_TIME)      { h = stg_hour;          m = stg_min;          }
      else if (item_idx == PARAM_LED_START) { h = s.led_start_hour;  m = s.led_start_min;  }
      else                                  { h = s.led_end_hour;     m = s.led_end_min;    }
      snprintf(val, sizeof(val), "%02d:%02d", h, m);
      if (editing) {
        snprintf(label, sizeof(label), "%s (%s)", PARAM_LABELS[item_idx],
                 param_edit_sub_idx == 0 ? "h" : "m");
      }
    }
  } else {
    int v = get_param_val(item_idx, s);
    snprintf(val, sizeof(val), "%d", v);
  }

  int ty = y + 13;
  tft.setFreeFont(UI_FONT);
  tft.setTextSize(UI_FONT_SIZE);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(UI_COLOR_TEXT);
  tft.drawString(label, 50, ty);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(editing ? (uint16_t)UI_COLOR_EDIT : (uint16_t)UI_COLOR_TEXT);
  tft.drawString(val, 265, ty);
}

static void render_params(const Settings &s) {
  ui_draw_background();
  ui_draw_params_background();
  ui_draw_params_title(stg_rtc_dirty);

  for (int slot = 0; slot < PARAMS_ITEMS_SHOWN; slot++) {
    int item = params_scroll + slot;
    if (item >= PARAM_COUNT) break;
    bool selected = (item == param_cursor) && (ui_mode != MODE_TAB);
    bool editing  = selected && (ui_mode == MODE_PARAM_EDIT);
    render_param_row(item, slot, selected, editing, s);
  }
}

// ─── Hardware error screens ───────────────────────────────────────────────────
//
// render_hw_error() draws the common base (white background + error icon + red banner).
// Each device error calls render_hw_error() then appends its own detail message.

static void render_hw_error(const char *title) {
  tft.fillScreen(TFT_WHITE);
  ui_draw_icon_error();
  tft.fillRect(0, 0, SCREEN_W, 52, 0xC000);
  tft.setTextColor(TFT_WHITE, 0xC000);
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(UI_FONT_SIZE);
  tft.drawString(title, SCREEN_W / 2, 26);
}

static void render_hw_error_footer(const char *msg) {
  tft.setTextSize(UI_FONT_SIZE);
  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.drawString(msg, SCREEN_W / 2, 172);
  tft.setTextColor(0x8410, TFT_WHITE);
  tft.drawString("Appuyer pour continuer", SCREEN_W / 2, 200);
}

static void render_sd_error() {
  render_hw_error("ERREUR CARTE SD");
  tft.setTextSize(UI_FONT_SIZE);
  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.drawString("Carte SD absente ou inaccessible.", SCREEN_W / 2, 180);
}

static void render_aht20_error() {
  render_hw_error("ERREUR AHT20");
  render_hw_error_footer("Capteur temp./humidite absent.");
}

static void render_rtc_error() {
  render_hw_error("ERREUR HORLOGE RTC");
  render_hw_error_footer("Module DS3231M absent ou inaccessible.");
}

// ─── Public ───────────────────────────────────────────────────────────────────

// ST7789 DISPON command — sent after the frame buffer is filled black.
// The DISPON inside ST7789_Init.h has been patched out so begin() never
// enables the display; we control it here for a noise-free startup.
#define ST7789_DISPON   0x29

void display_init() {
  // 1. Backlight off for the entire init sequence.
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, LOW);

  // 2. Init the TFT controller. The patched ST7789_Init.h no longer sends
  //    DISPON or enables the backlight, so the panel stays dark.
  tft.begin();

  // 3. Configure rotation and pixel format, then fill the frame buffer black.
  tft.setRotation(3);
  tft.setSwapBytes(false);
  tft.fillScreen(TFT_BLACK);

  // 4. Enable panel output now that the frame buffer is clean black.
  tft.startWrite();
  tft.writecommand(ST7789_DISPON);
  tft.endWrite();

  // 5. Turn backlight on — panel already shows black, no noise flash.
  digitalWrite(TFT_BL, HIGH);

  // 6. Hand the shared TFT instance to the UI rendering layer.
  ui_init(tft);

  tft.setFreeFont(UI_FONT);
}

bool display_update(SensorData &data, Settings &settings, EncEvent ev) {
  settings_dirty = false;

  // ── SD error overlay — highest priority, blocks all normal interaction ────
  if (data.sd_error) {
    if (!sd_error_shown) {
      render_sd_error();
      sd_error_shown   = true;
      need_full_redraw = true;   // force a full redraw when the card comes back
    }
    return false;
  }
  if (sd_error_shown) {
    // SD card just came back — restore normal UI
    sd_error_shown = false;
    tft.fillScreen(TFT_BLACK);
    render_tabbar();
  }

  // ── Hardware diagnostics — re-triggered each time an error (re)appears ───
  // Reset the "shown" flag when the error clears so a reconnected sensor
  // will produce a new diagnostic screen if it fails again.
  if (prev_aht20_error && !data.aht20_error) hw_error_aht20_displayed = false;
  if (prev_rtc_error   && !data.rtc_error)   hw_error_rtc_displayed   = false;
  prev_aht20_error = data.aht20_error;
  prev_rtc_error   = data.rtc_error;

  if (!hw_error_aht20_displayed && data.aht20_error) {
    if (diag_until_ms == 0) {
      render_aht20_error();
      diag_until_ms = millis() + DIAG_DURATION_MS;
    }
    if (millis() < diag_until_ms && ev == ENC_NONE) return false;
    hw_error_aht20_displayed = true;
    diag_until_ms    = 0;
    need_full_redraw = true;
  }
  if (!hw_error_rtc_displayed && data.rtc_error) {
    if (diag_until_ms == 0) {
      render_rtc_error();
      diag_until_ms = millis() + DIAG_DURATION_MS;
    }
    if (millis() < diag_until_ms && ev == ENC_NONE) return false;
    hw_error_rtc_displayed   = true;
    diag_until_ms    = 0;
    need_full_redraw = true;
  }

  bool do_render = false;

  // ── First-time full redraw ────────────────────────────────────────────────
  if (need_full_redraw) {
    tft.fillScreen(TFT_BLACK);
    need_full_redraw = false;
    do_render        = true;
  }

  // ── Periodic refresh for live data ───────────────────────────────────────
  // Home screen: partial update (widgets only — no background or tabbar redraw).
  // Details screen: full redraw (all rows are dynamic data).
  if ((ui_mode == MODE_TAB || ui_mode == MODE_DETAILS_SCROLL) &&
      current_tab != SCREEN_PARAMS &&
      millis() - last_refresh_ms >= 60000UL) {
    last_refresh_ms = millis();
    if (current_tab == SCREEN_HOME) {
      render_home_update(data, settings);
    } else {
      do_render = true;
    }
  }

  // ── Encoder event handling ────────────────────────────────────────────────
  if (ev != ENC_NONE) {
    switch (ui_mode) {

      // ── Tab navigation ────────────────────────────────────────────────────
      case MODE_TAB:
        if (ev == ENC_UP || ev == ENC_DOWN) {
          int dir = (ev == ENC_UP) ? 1 : -1;
          current_tab = (Screen)(((int)current_tab + dir + 3) % 3);
          if (current_tab == SCREEN_PARAMS) init_staged(data);
          do_render = true;
        } else if (ev == ENC_PRESS && current_tab == SCREEN_DETAILS) {
          details_cursor = 0;
          details_scroll = 0;
          ui_mode        = MODE_DETAILS_SCROLL;
          do_render      = true;
        } else if (ev == ENC_PRESS && current_tab == SCREEN_PARAMS) {
          init_staged(data);
          param_cursor  = 0;
          params_scroll = 0;
          ui_mode       = MODE_PARAM_SELECT;
          do_render     = true;
        }
        break;

      // ── Details scroll ────────────────────────────────────────────────────
      // Partial update when cursor moves within the visible window (no scroll):
      // redraw only the old and new row.  Full redraw when the window shifts.
      case MODE_DETAILS_SCROLL: {
        int prev_cursor = details_cursor;
        int prev_scroll = details_scroll;
        if (ev == ENC_UP && details_cursor > 0) {
          details_cursor--;
          if (details_cursor < details_scroll) details_scroll--;
        } else if (ev == ENC_DOWN && details_cursor < DETAILS_ITEM_COUNT - 1) {
          details_cursor++;
          if (details_cursor >= details_scroll + DETAILS_ITEMS_SHOWN) details_scroll++;
        } else if (ev == ENC_LONG_PRESS) {
          ui_mode   = MODE_TAB;
          do_render = true;
          break;
        }
        if (details_cursor != prev_cursor) {
          if (details_scroll != prev_scroll) {
            do_render = true;   // window shifted — full redraw
          } else {
            render_details_row(prev_cursor, prev_cursor - details_scroll, false, data);
            render_details_row(details_cursor, details_cursor - details_scroll, true, data);
          }
        }
        break;
      }

      // ── Parameter selection ───────────────────────────────────────────────
      // Scroll logic mirrors details page: linear cursor, no wrap-around.
      case MODE_PARAM_SELECT: {
        int prev_cursor = param_cursor;
        int prev_scroll = params_scroll;
        if (ev == ENC_UP && param_cursor > 0) {
          param_cursor--;
          if (param_cursor < params_scroll) params_scroll--;
        } else if (ev == ENC_DOWN && param_cursor < PARAM_COUNT - 1) {
          param_cursor++;
          if (param_cursor >= params_scroll + PARAMS_ITEMS_SHOWN) params_scroll++;
        } else if (ev == ENC_PRESS) {
          param_edit_sub_idx = 0;
          ui_mode = MODE_PARAM_EDIT;
          render_param_row(param_cursor, param_cursor - params_scroll, true, true, settings);
        } else if (ev == ENC_LONG_PRESS) {
          commit_staged_rtc();
          ui_mode   = MODE_TAB;
          do_render = true;
        }
        if (param_cursor != prev_cursor) {
          if (params_scroll != prev_scroll) {
            // Scroll window shifted — redraw all visible slots only (no background/title)
            for (int slot = 0; slot < PARAMS_ITEMS_SHOWN; slot++) {
              int item = params_scroll + slot;
              if (item >= PARAM_COUNT) break;
              render_param_row(item, slot, item == param_cursor, false, settings);
            }
          } else {
            render_param_row(prev_cursor, prev_cursor - params_scroll, false, false, settings);
            render_param_row(param_cursor, param_cursor - params_scroll, true, false, settings);
          }
        }
        break;
      }

      // ── Value editing ─────────────────────────────────────────────────────
      case MODE_PARAM_EDIT:
        if (ev == ENC_UP) {
          apply_delta(param_cursor, +1, settings);
          render_param_row(param_cursor, param_cursor - params_scroll, true, true, settings);
        } else if (ev == ENC_DOWN) {
          apply_delta(param_cursor, -1, settings);
          render_param_row(param_cursor, param_cursor - params_scroll, true, true, settings);
        } else if (ev == ENC_PRESS && combined_sub_count(param_cursor) > 0) {
          param_edit_sub_idx = (param_edit_sub_idx + 1) % combined_sub_count(param_cursor);
          render_param_row(param_cursor, param_cursor - params_scroll, true, true, settings);
        } else if (ev == ENC_LONG_PRESS) {
          commit_staged_rtc();
          param_edit_sub_idx = 0;
          ui_mode = MODE_PARAM_SELECT;
          render_param_row(param_cursor, param_cursor - params_scroll, true, false, settings);
        }
        break;
    }
  }

  // ── Full content render ───────────────────────────────────────────────────
  // render_tabbar() is called AFTER content so ui_draw_background() (320×240)
  // cannot overwrite the tab bar.
  if (do_render) {
    last_refresh_ms = millis();
    switch (current_tab) {
      case SCREEN_HOME:    render_home(data, settings);  break;
      case SCREEN_DETAILS: render_details(data);         break;
      case SCREEN_PARAMS:  render_params(settings); break;
    }
    render_tabbar();
  }

  return settings_dirty;
}
