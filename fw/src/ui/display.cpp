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
static bool     need_full_redraw        = true;
static uint32_t last_refresh_ms         = 0;
static uint32_t last_details_refresh_ms = 0;
static bool     sd_error_shown          = false;

// Hardware diagnostic screens — re-triggered each time an error (re)appears.
// The _displayed flags prevent re-showing while the error is still active.
// The prev_* flags detect error-clear→re-error transitions so a sensor that
// disappears after boot and comes back will produce a fresh diagnostic screen.
static bool     hw_error_aht20_displayed  = false;
static bool     hw_error_rtc_displayed    = false;
static bool     prev_aht20_error          = false;
static bool     prev_rtc_error            = false;
static uint32_t diag_until_ms     = 0;
static const uint32_t DIAG_DURATION_MS    = 3000;
static const uint32_t LIVE_REFRESH_MS     = 60000;   // Home partial widget refresh (ms)
static const uint32_t DETAILS_REFRESH_MS  =  1000;   // Details full row refresh (ms)
static const int      TAB_COUNT           =     3;   // number of top-level screens (Home, Details, Settings)

// ─── List row layout (Details and Settings share the same item dimensions) ────
#define LIST_TEXT_X_LEFT    50   // x of the left-aligned label inside a list item
#define LIST_TEXT_X_RIGHT  265   // x of the right-aligned value inside a list item
#define LIST_TEXT_Y_OFFSET  13   // vertical offset from item top to text baseline
#define LIST_ITEM_Y0        62   // y of the first visible item slot
#define LIST_ITEM_STEP      34   // vertical spacing between items (px)
#define LIST_ITEMS_SHOWN     3   // number of visible rows at a time

// ─── Hardware error screen layout ─────────────────────────────────────────────
#define HW_ERROR_MSG_Y     172   // y of the device-specific message on error screens
#define HW_ERROR_HINT_Y    200   // y of the "press to continue" hint

// ─── Parameter edit bounds and buffer sizes ───────────────────────────────────
#define PARAM_YEAR_MIN        2024
#define PARAM_YEAR_MAX        2069
#define PARAM_VAL_BUF_LEN       12   // fits "31/12/2069\0" (10 chars + NUL, padded to 12)
#define PARAM_LABEL_BUF_LEN     28   // fits "Debut eclairage (h)\0" (19 chars, padded to 28)

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
  PARAM_TIME              = 0,   // combined HH:MM        (stg_hour, stg_min)
  PARAM_DATE              = 1,   // combined DD/MM/YYYY   (stg_day, stg_month, stg_year)
  PARAM_GROW_START        = 2,   // combined DD/MM/YYYY   (grow_start_day/month/year)
  PARAM_LED_START         = 3,   // combined HH:MM        (led_start_hour, led_start_min)
  PARAM_LED_END           = 4,   // combined HH:MM        (led_end_hour, led_end_min)
  PARAM_SOIL              = 5,   // soil moisture threshold %
  PARAM_WATERING_DURATION = 6,   // max pump on time per cycle (s)
  PARAM_WATERING_COOLDOWN = 7,   // min time between watering cycles (min)
  PARAM_TEMP_MIN          = 8,   // minimum temperature °C
  PARAM_TEMP_MAX          = 9,   // maximum temperature °C
  PARAM_COUNT             = 10,
};

static const char * const PARAM_LABELS[PARAM_COUNT] = {
  "Heure",             // PARAM_TIME
  "Date",              // PARAM_DATE
  "Debut pousse",      // PARAM_GROW_START
  "Debut eclairage",   // PARAM_LED_START
  "Fin eclairage",     // PARAM_LED_END
  "Seuil humidite %",  // PARAM_SOIL
  "Duree arrosage s",  // PARAM_WATERING_DURATION
  "Temporisation arrosage min",// PARAM_WATERING_COOLDOWN
  "Temp min C",        // PARAM_TEMP_MIN
  "Temp max C",        // PARAM_TEMP_MAX
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
    case PARAM_SOIL:              return s.soil_threshold;
    case PARAM_WATERING_DURATION: return s.watering_duration_s;
    case PARAM_WATERING_COOLDOWN: return s.watering_cooldown_min;
    case PARAM_TEMP_MIN:          return s.plant_temp_min;
    case PARAM_TEMP_MAX:          return s.plant_temp_max;
    default:                      return 0;
  }
}

static void apply_time_delta(int sub, uint8_t &h, uint8_t &m, int delta) {
  if (sub == 0) h = (uint8_t)constrain((int)h + delta, 0, 23);
  else          m = (uint8_t)constrain((int)m + delta, 0, 59);
}

static void apply_date_delta(int sub, uint8_t &d, uint8_t &mo, uint16_t &yr, int delta) {
  if      (sub == 0) d  = (uint8_t) constrain((int)d  + delta, 1, 31);
  else if (sub == 1) mo = (uint8_t) constrain((int)mo + delta, 1, 12);
  else               yr = (uint16_t)constrain((int)yr + delta, PARAM_YEAR_MIN, PARAM_YEAR_MAX);
}

static void apply_delta(int i, int delta, Settings &s) {
  switch (i) {
    case PARAM_TIME:  // combined HH:MM
      apply_time_delta(param_edit_sub_idx, stg_hour, stg_min, delta);
      stg_rtc_dirty = true; break;
    case PARAM_DATE:  // combined DD/MM/YYYY
      apply_date_delta(param_edit_sub_idx, stg_day, stg_month, stg_year, delta);
      stg_rtc_dirty = true; break;
    case PARAM_GROW_START:  // combined DD/MM/YYYY
      apply_date_delta(param_edit_sub_idx, s.grow_start_day, s.grow_start_month, s.grow_start_year, delta);
      settings_dirty = true; break;
    case PARAM_LED_START:  // combined HH:MM
      apply_time_delta(param_edit_sub_idx, s.led_start_hour, s.led_start_min, delta);
      settings_dirty = true; break;
    case PARAM_LED_END: {  // combined HH:MM — cannot equal the start time
      apply_time_delta(param_edit_sub_idx, s.led_end_hour, s.led_end_min, delta);
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
    case PARAM_WATERING_DURATION:
      s.watering_duration_s = (uint16_t)constrain(s.watering_duration_s + delta, 1, 300);
      settings_dirty = true; break;
    case PARAM_WATERING_COOLDOWN:
      s.watering_cooldown_min = (uint16_t)constrain(s.watering_cooldown_min + delta, 1, 1440);
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

// Julian Day Number — Fliegel & Van Flandern integer formula (CACM vol.11, 1968).
static int32_t julian_day(int d, int m, int y) {
  return 367L*y - 7*(y+(m+9)/12)/4 + 275*m/9 + d + 1721013L;
}

static int32_t days_elapsed(int d1, int m1, int y1, int d2, int m2, int y2) {
  int32_t diff = julian_day(d2, m2, y2) - julian_day(d1, m1, y1);
  return diff < 0 ? 0 : diff;
}

// ─── Scrollable list helper ───────────────────────────────────────────────────

enum ScrollResult { SCROLL_UNCHANGED, SCROLL_SAME_WINDOW, SCROLL_WINDOW_SHIFTED };

// Move cursor up/down and keep the scroll window in sync.
// Returns SCROLL_UNCHANGED when ev is not ENC_UP/ENC_DOWN or the cursor is
// already at a boundary; SCROLL_SAME_WINDOW when only the cursor moved;
// SCROLL_WINDOW_SHIFTED when the visible window had to scroll.
static ScrollResult scroll_update(int &cursor, int &scroll,
                                  EncEvent ev, int max_count, int items_shown) {
  if (ev != ENC_UP && ev != ENC_DOWN) return SCROLL_UNCHANGED;
  int prev_scroll = scroll;
  if (ev == ENC_UP && cursor > 0) {
    cursor--;
    if (cursor < scroll) scroll--;
  } else if (ev == ENC_DOWN && cursor < max_count - 1) {
    cursor++;
    if (cursor >= scroll + items_shown) scroll++;
  } else {
    return SCROLL_UNCHANGED;
  }
  return (scroll != prev_scroll) ? SCROLL_WINDOW_SHIFTED : SCROLL_SAME_WINDOW;
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
// Called from render_home() on full render, and directly by the periodic timer
// (LIVE_REFRESH_MS) for partial updates without a full background repaint.
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

#define DETAILS_ITEM_COUNT    6
#define DETAIL_VAL_BUF_LEN   24   // fits longest formatted value ("31/12 23:59\0" = 12 chars, padded to 24)

enum DetailIndex {
  DETAIL_TEMP     = 0,
  DETAIL_HUM      = 1,
  DETAIL_SOIL     = 2,
  DETAIL_PUMP     = 3,
  DETAIL_LED      = 4,
  DETAIL_LAST_LOG = 5,
};

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
    case DETAIL_TEMP:
      if (data.aht20_ready) snprintf(buf, len, "%.1f C",  data.air_temp);
      else                  snprintf(buf, len, "-- C");
      break;
    case DETAIL_HUM:
      if (data.aht20_ready) snprintf(buf, len, "%.0f %%", data.air_humidity);
      else                  snprintf(buf, len, "-- %%");
      break;
    case DETAIL_SOIL:     snprintf(buf, len, "%d %%", data.soil_pct); break;
    case DETAIL_PUMP:     snprintf(buf, len, "%s", data.pump_on ? "ON" : "OFF"); break;
    case DETAIL_LED:      snprintf(buf, len, "%s", data.led_on  ? "ON" : "OFF"); break;
    case DETAIL_LAST_LOG:
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

// Shared text renderer for both Details and Settings list rows.
static void render_row_text(int y, const char *label, const char *val, uint16_t val_color) {
  int ty = y + LIST_TEXT_Y_OFFSET;
  tft.setFreeFont(UI_FONT);
  tft.setTextSize(UI_FONT_SIZE);
  tft.setTextColor(UI_COLOR_TEXT);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(label, LIST_TEXT_X_LEFT, ty);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(val_color);
  tft.drawString(val, LIST_TEXT_X_RIGHT, ty);
}

static void render_details_row(int item_idx, int slot, bool selected, const SensorData &data) {
  int y = LIST_ITEM_Y0 + slot * LIST_ITEM_STEP;
  if (selected) ui_draw_details_item_selected(y);
  else          ui_draw_details_item(y);

  char val[DETAIL_VAL_BUF_LEN];
  get_detail_value(item_idx, data, val, sizeof(val));
  render_row_text(y, DETAIL_LABELS[item_idx], val, UI_COLOR_TEXT);
}

static void render_details(const SensorData &data) {
  ui_draw_background();
  ui_draw_details_background();
  ui_draw_details_title();

  for (int slot = 0; slot < LIST_ITEMS_SHOWN; slot++) {
    int item = details_scroll + slot;
    if (item >= DETAILS_ITEM_COUNT) break;
    render_details_row(item, slot, item == details_cursor, data);
  }
}

// ─── Screen: Settings ─────────────────────────────────────────────────────────
//
// Scrollable list — 8 items total, 3 visible at a time.
// Matches the details page layout: same images, same scroll logic.


static void render_param_row(int item_idx, int slot, bool selected, bool editing, const Settings &s) {
  int y = LIST_ITEM_Y0 + slot * LIST_ITEM_STEP;
  if (selected) ui_draw_params_item_selected(y);
  else          ui_draw_params_item(y);

  char val[PARAM_VAL_BUF_LEN];
  char label[PARAM_LABEL_BUF_LEN];
  strncpy(label, PARAM_LABELS[item_idx], sizeof(label) - 1);
  label[sizeof(label) - 1] = '\0';

  if (combined_sub_count(item_idx) > 0) {
    if (item_idx == PARAM_DATE || item_idx == PARAM_GROW_START) {
      // DD/MM/YYYY — source depends on which row we're rendering
      uint8_t  d  = (item_idx == PARAM_DATE) ? stg_day          : s.grow_start_day;
      uint8_t  mo = (item_idx == PARAM_DATE) ? stg_month        : s.grow_start_month;
      uint16_t yr = (item_idx == PARAM_DATE) ? stg_year         : s.grow_start_year;
      snprintf(val, sizeof(val), "%02d/%02d/%04d", d, mo, yr);
      if (editing) {
        const char *sub_names[] = { "d", "m", "y" };
        snprintf(label, sizeof(label), "%s (%s)", PARAM_LABELS[item_idx], sub_names[param_edit_sub_idx]);
      }
    } else {
      // PARAM_TIME, PARAM_LED_START, PARAM_LED_END: HH:MM
      int h = 0, m = 0;
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

  render_row_text(y, label, val, editing ? (uint16_t)UI_COLOR_EDIT : (uint16_t)UI_COLOR_TEXT);
}

static void render_params(const Settings &s) {
  ui_draw_background();
  ui_draw_params_background();
  ui_draw_params_title(stg_rtc_dirty);

  for (int slot = 0; slot < LIST_ITEMS_SHOWN; slot++) {
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
  tft.fillRect(0, 0, SCREEN_W, UI_ERROR_BANNER_H, UI_COLOR_ERROR_BG);
  tft.setTextColor(TFT_WHITE, UI_COLOR_ERROR_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(UI_FONT_SIZE);
  tft.drawString(title, SCREEN_W / 2, UI_ERROR_BANNER_H / 2);
}

// Draw the device-specific message line, explicitly fixing the text datum so
// callers do not rely on MC_DATUM being left set by render_hw_error().
static void render_hw_error_msg(const char *msg) {
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(UI_FONT_SIZE);
  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.drawString(msg, SCREEN_W / 2, HW_ERROR_MSG_Y);
}

static void render_hw_error_footer(const char *msg) {
  render_hw_error_msg(msg);
  tft.setTextColor(UI_COLOR_HINT, TFT_WHITE);
  tft.drawString("Appuyer pour continuer", SCREEN_W / 2, HW_ERROR_HINT_Y);
}

static void render_sd_error() {
  render_hw_error("ERREUR CARTE SD");
  render_hw_error_msg("Carte SD absente ou inaccessible.");
}

static void render_aht20_error() {
  render_hw_error("ERREUR AHT20");
  render_hw_error_footer("Capteur temp./humidite absent.");
}

static void render_rtc_error() {
  render_hw_error("ERREUR HORLOGE RTC");
  render_hw_error_footer("Module DS3231M absent ou inaccessible.");
}

// Show a hardware diagnostic screen and block rendering until dismissed.
// Returns false while the screen is still showing (caller should return early).
// Sets need_full_redraw when the diagnostic is dismissed so the normal UI
// is fully repainted on the next call.
static bool handle_hw_diag(bool &displayed, bool error_active,
                            void (*render_fn)(), EncEvent ev) {
  if (displayed || !error_active) return true;
  if (diag_until_ms == 0) {
    render_fn();
    diag_until_ms = millis() + DIAG_DURATION_MS;
  }
  if (millis() < diag_until_ms && ev == ENC_NONE) return false;
  displayed        = true;
  diag_until_ms    = 0;
  need_full_redraw = true;
  return true;
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

  if (!handle_hw_diag(hw_error_aht20_displayed, data.aht20_error, render_aht20_error, ev)) return false;
  if (!handle_hw_diag(hw_error_rtc_displayed,   data.rtc_error,   render_rtc_error,   ev)) return false;

  bool do_render = false;

  // ── First-time full redraw ────────────────────────────────────────────────
  if (need_full_redraw) {
    tft.fillScreen(TFT_BLACK);
    need_full_redraw = false;
    do_render        = true;
  }

  // ── Periodic refresh for live data ───────────────────────────────────────
  // Home: partial widget update only (no background repaint), every 60 s.
  if (ui_mode == MODE_TAB && current_tab == SCREEN_HOME &&
      millis() - last_refresh_ms >= LIVE_REFRESH_MS) {
    last_refresh_ms = millis();
    render_home_update(data, settings);
  }
  // Details: redraw each visible row at 1 Hz so sensor values and actuator
  // states stay current. Only the item background + text are redrawn —
  // the global background and title are left untouched to avoid flicker.
  if ((ui_mode == MODE_TAB || ui_mode == MODE_DETAILS_SCROLL) &&
      current_tab == SCREEN_DETAILS &&
      millis() - last_details_refresh_ms >= DETAILS_REFRESH_MS) {
    last_details_refresh_ms = millis();
    for (int slot = 0; slot < LIST_ITEMS_SHOWN; slot++) {
      int item = details_scroll + slot;
      if (item >= DETAILS_ITEM_COUNT) break;
      render_details_row(item, slot, item == details_cursor, data);
    }
  }

  // ── Encoder event handling ────────────────────────────────────────────────
  if (ev != ENC_NONE) {
    switch (ui_mode) {

      // ── Tab navigation ────────────────────────────────────────────────────
      case MODE_TAB:
        if (ev == ENC_UP || ev == ENC_DOWN) {
          int dir = (ev == ENC_UP) ? 1 : -1;
          current_tab = (Screen)(((int)current_tab + dir + TAB_COUNT) % TAB_COUNT);
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
        if (ev == ENC_LONG_PRESS) { ui_mode = MODE_TAB; do_render = true; break; }
        int prev_cursor = details_cursor;
        ScrollResult sr = scroll_update(details_cursor, details_scroll,
                                        ev, DETAILS_ITEM_COUNT, LIST_ITEMS_SHOWN);
        if (sr == SCROLL_WINDOW_SHIFTED) {
          do_render = true;
        } else if (sr == SCROLL_SAME_WINDOW) {
          render_details_row(prev_cursor,      prev_cursor      - details_scroll, false, data);
          render_details_row(details_cursor,   details_cursor   - details_scroll, true,  data);
        }
        break;
      }

      // ── Parameter selection ───────────────────────────────────────────────
      case MODE_PARAM_SELECT: {
        if (ev == ENC_PRESS) {
          param_edit_sub_idx = 0;
          ui_mode = MODE_PARAM_EDIT;
          render_param_row(param_cursor, param_cursor - params_scroll, true, true, settings);
          break;
        }
        if (ev == ENC_LONG_PRESS) {
          commit_staged_rtc();
          ui_mode = MODE_TAB; do_render = true;
          break;
        }
        int prev_cursor = param_cursor;
        ScrollResult sr = scroll_update(param_cursor, params_scroll,
                                        ev, PARAM_COUNT, LIST_ITEMS_SHOWN);
        if (sr == SCROLL_WINDOW_SHIFTED) {
          // Redraw visible slots only — no background or title repaint
          for (int slot = 0; slot < LIST_ITEMS_SHOWN; slot++) {
            int item = params_scroll + slot;
            if (item >= PARAM_COUNT) break;
            render_param_row(item, slot, item == param_cursor, false, settings);
          }
        } else if (sr == SCROLL_SAME_WINDOW) {
          render_param_row(prev_cursor,  prev_cursor  - params_scroll, false, false, settings);
          render_param_row(param_cursor, param_cursor - params_scroll, true,  false, settings);
        }
        break;
      }

      // ── Value editing ─────────────────────────────────────────────────────
      case MODE_PARAM_EDIT:
        if (ev == ENC_UP || ev == ENC_DOWN) {
          apply_delta(param_cursor, (ev == ENC_UP) ? +1 : -1, settings);
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
    last_refresh_ms         = millis();
    last_details_refresh_ms = millis();
    switch (current_tab) {
      case SCREEN_HOME:    render_home(data, settings);  break;
      case SCREEN_DETAILS: render_details(data);         break;
      case SCREEN_PARAMS:  render_params(settings); break;
    }
    render_tabbar();
  }

  return settings_dirty;
}
