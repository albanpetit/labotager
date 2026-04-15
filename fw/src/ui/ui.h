#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

// ─── UI font ──────────────────────────────────────────────────────────────────
#include "components/custom_font.h"
#define UI_FONT       (&custom_font)
#define UI_FONT_SIZE  1
#define UI_COLOR_TEXT 0x2948   // #2e2847 en RGB565
#define UI_COLOR_EDIT 0x2B08   // #2b6043 en RGB565 — valeur en cours d'édition


// Must be called once from display_init() before any draw call.
// Provides the shared TFT instance to the UI rendering layer.
void ui_init(TFT_eSPI &tft);

// Draw any PROGMEM PNG byte array at screen position (x, y).
// opaque=true forces pushImageDMA for every pixel (no alpha mask), useful for
// background images where transparent areas must never reveal stale content.
void ui_draw_png(const uint8_t *data, size_t len, int16_t x, int16_t y, bool opaque = false);

// ─── Component draw functions ─────────────────────────────────────────────────
// One function per header in components/. Add entries here as new PNG assets
// are converted and placed in src/ui/components/.

// Tab bar background — drawn at (0, 201)
void ui_draw_menu_background();

// Tab icons — unselected state
void ui_draw_menu_icon_home();
void ui_draw_menu_icon_details();
void ui_draw_menu_icon_settings();

// Tab icons — selected state
void ui_draw_menu_icon_home_selected();
void ui_draw_menu_icon_details_selected();
void ui_draw_menu_icon_settings_selected();

// Full-screen content background — 320×200 px, drawn at (0, 0)
void ui_draw_background();

// Error icon — drawn at (20, 30)
void ui_draw_icon_error();

void ui_draw_home_date(const char *date);
void ui_draw_home_time(const char *time);
void ui_draw_home_grow_counter(const char *days);
void ui_draw_home_owner(const char *owner);

// ─── Plant status illustration ────────────────────────────────────────────────
// Add new states here and handle them in ui_draw_plant_status() / display.cpp.
enum PlantState {
  PLANT_HAPPY,     // all parameters within acceptable range
  PLANT_THIRSTY,   // soil_pct < settings.soil_threshold
  PLANT_HOT,       // air_temp > PLANT_TEMP_MAX_C
  PLANT_COLD,      // air_temp < PLANT_TEMP_MIN_C
};

// Draw the plant status image at (PLANT_IMG_X, PLANT_IMG_Y).
// Replace the stub headers in components/ with real PNG data to show images.
void ui_draw_home_plant_status(PlantState state);

void ui_draw_details_background();
void ui_draw_details_title();

// Details list items — y is the vertical position of the item row
void ui_draw_details_item(int16_t y);
void ui_draw_details_item_selected(int16_t y);

void ui_draw_params_background();
void ui_draw_params_title();

// Params list items — y is the vertical position of the item row
void ui_draw_params_item(int16_t y);
void ui_draw_params_item_selected(int16_t y);
