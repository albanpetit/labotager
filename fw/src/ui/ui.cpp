#include "ui.h"
#include "../config.h"
#include <TFT_eSPI.h>

// ─── Component image data (PROGMEM) ──────────────────────────────────────────
// Each header exports: name[], NAME_W, NAME_H, name_mask (array or nullptr).
// To add a new component:
//   1. Place the source PNG in tools/images/
//   2. Run: python3 tools/png2h.py tools/images/<file>.png --output-dir src/ui/components
//   3. Include the resulting header here and add a draw function below.

#include "components/menu_background.h"
#include "components/menu_icon_details_selected.h"
#include "components/menu_icon_home_selected.h"
#include "components/menu_icon_settings_selected.h"
#include "components/menu_icon_details.h"
#include "components/menu_icon_home.h"
#include "components/menu_icon_settings.h"
#include "components/background.h"
#include "components/icon_error.h"
#include "components/plant_happy.h"
#include "components/plant_unhappy.h"
#include "components/plant_icon_hot.h"
#include "components/plant_icon_cold.h"
#include "components/plant_thirsty.h"
#include "components/home_date.h"
#include "components/home_time.h"
#include "components/home_grow_counter.h"
#include "components/home_owner.h"
#include "components/details_background.h"
#include "components/details_item.h"
#include "components/details_item_selected.h"
#include "components/params_background.h"
#include "components/params_item.h"
#include "components/params_item_selected.h"

// ─── Private state ────────────────────────────────────────────────────────────

static TFT_eSPI *_tft = nullptr;

// Home widget sprites — allocated once in ui_init(), reused each frame.
// createSprite()/deleteSprite() on every draw() fragments the mbed heap;
// pointer allocation avoids fragmentation over long runtimes.
static TFT_eSprite *_spr_date = nullptr;
static TFT_eSprite *_spr_time = nullptr;
static TFT_eSprite *_spr_grow = nullptr;

// Transparent color key for sprite compositing.
// Pixels that are transparent in the mask keep this color so pushSprite(x,y,SPRITE_KEY)
// lets the underlying TFT content show through.
// Value chosen to be absent from warm-tone UI widget palettes.
#define SPRITE_KEY  ((uint16_t)0xF81F)   // pure magenta in RGB565

// ─── Private draw helpers ─────────────────────────────────────────────────────

// Dispatch to pushImage (opaque) or pushMaskedImage (alpha) based on mask pointer.
// Opaque headers export `name_mask = nullptr`; masked headers export a real array.
static void _ui_img(int16_t x, int16_t y, int16_t w, int16_t h,
                    const uint16_t *px, const uint8_t *mask) {
  if (mask)
    _tft->pushMaskedImage(x, y, w, h, (uint16_t*)px, (uint8_t*)mask);
  else
    _tft->pushImage(x, y, w, h, (uint16_t*)px);
}

// Copy a pre-decoded masked image into a sprite.
// Only opaque pixel runs are written; transparent pixels keep the pre-filled SPRITE_KEY.
static void _ui_img_to_sprite(TFT_eSprite &spr, int16_t w, int16_t h,
                               const uint16_t *px, const uint8_t *mask) {
  if (!mask) {
    spr.pushImage(0, 0, w, h, (uint16_t*)px);
    return;
  }
  int bytes_per_row = (w + 7) / 8;
  for (int y = 0; y < h; y++) {
    int run_start = -1;
    for (int x = 0; x <= w; x++) {
      bool opaque = (x < w) && (mask[y * bytes_per_row + (x >> 3)] & (0x80 >> (x & 7)));
      if (opaque && run_start < 0) {
        run_start = x;
      } else if (!opaque && run_start >= 0) {
        spr.pushImage(run_start, y, x - run_start, 1, px + y * w + run_start);
        run_start = -1;
      }
    }
  }
}

// Draw a component image to the TFT at (x, y).
// Automatically selects pushImage vs pushMaskedImage from name_mask.
// png2h.py emits lowercase `name_W`/`name_H` constexpr aliases for macro compatibility.
#define UI_IMG(name, x, y) \
  _ui_img((x), (y), name##_W, name##_H, (uint16_t*)(name), name##_mask)

// Copy a component image into a sprite (pre-fill with SPRITE_KEY first).
#define UI_IMG_SPRITE(spr, name) \
  _ui_img_to_sprite((spr), name##_W, name##_H, (uint16_t*)(name), name##_mask)

// ─── Public ───────────────────────────────────────────────────────────────────

// ST7789 DISPON command — sent after the frame buffer is filled black.
// The DISPON inside ST7789_Init.h has been patched out so begin() never
// enables the display; we control it here for a noise-free startup.
void ui_init(TFT_eSPI &tft) {
  _tft = &tft;

  _spr_date = new TFT_eSprite(_tft);
  _spr_date->createSprite(92, 29);
  _spr_date->setSwapBytes(false);

  _spr_time = new TFT_eSprite(_tft);
  _spr_time->createSprite(92, 29);
  _spr_time->setSwapBytes(false);

  _spr_grow = new TFT_eSprite(_tft);
  _spr_grow->createSprite(92, 29);
  _spr_grow->setSwapBytes(false);
}

// ─── Components ───────────────────────────────────────────────────────────────

void ui_draw_menu_background() {
  UI_IMG(menu_background, 0, 201);
}

void ui_draw_menu_icon_details_selected() {
  UI_IMG(menu_icon_details_selected, 95, 198);
}

void ui_draw_menu_icon_home_selected() {
  UI_IMG(menu_icon_home_selected, 134, 198);
}

void ui_draw_menu_icon_settings_selected() {
  UI_IMG(menu_icon_settings_selected, 173, 198);
}

void ui_draw_menu_icon_details() {
  UI_IMG(menu_icon_details, 95, 198);
}

void ui_draw_menu_icon_home() {
  UI_IMG(menu_icon_home, 134, 198);
}

void ui_draw_menu_icon_settings() {
  UI_IMG(menu_icon_settings, 173, 198);
}

void ui_draw_background() {
  UI_IMG(background, 0, 0);
}

void ui_draw_icon_error() {
  UI_IMG(icon_error, (320 - 45) / 2, 80);
}

void ui_draw_home_date(const char *date) {
  _spr_date->fillSprite(SPRITE_KEY);
  UI_IMG_SPRITE(*_spr_date, home_date);
  _spr_date->setFreeFont(UI_FONT);
  _spr_date->setTextColor(UI_COLOR_TEXT);
  _spr_date->setTextDatum(MC_DATUM);
  _spr_date->setTextSize(UI_FONT_SIZE);
  _spr_date->drawString(date, 53, 11);
  _spr_date->pushSprite(17, 29, SPRITE_KEY);
}

void ui_draw_home_time(const char *time) {
  _spr_time->fillSprite(SPRITE_KEY);
  UI_IMG_SPRITE(*_spr_time, home_time);
  _spr_time->setFreeFont(UI_FONT);
  _spr_time->setTextColor(UI_COLOR_TEXT);
  _spr_time->setTextDatum(MC_DATUM);
  _spr_time->setTextSize(UI_FONT_SIZE);
  _spr_time->drawString(time, 53, 11);
  _spr_time->pushSprite(17, 59, SPRITE_KEY);
}

void ui_draw_home_grow_counter(const char *days) {
  _spr_grow->fillSprite(SPRITE_KEY);
  UI_IMG_SPRITE(*_spr_grow, home_grow_counter);
  _spr_grow->setFreeFont(UI_FONT);
  _spr_grow->setTextColor(UI_COLOR_TEXT);
  _spr_grow->setTextDatum(MC_DATUM);
  _spr_grow->setTextSize(UI_FONT_SIZE);
  _spr_grow->drawString(days, 53, 11);
  _spr_grow->pushSprite(18, 89, SPRITE_KEY);
}

void ui_draw_home_owner(const char *owner_name) {
  UI_IMG(home_owner, 12, 135);
  _tft->setTextColor(UI_COLOR_TEXT);
  _tft->setTextDatum(MC_DATUM);
  _tft->setTextSize(UI_FONT_SIZE);
  _tft->drawString("Labotager de", 73, 152);
  _tft->drawString(owner_name, 73, 170);
}

void ui_draw_home_plant_status(PlantState state) {
  switch (state) {
    case PLANT_HAPPY:
      UI_IMG(plant_happy, 184, 49);
      break;
    case PLANT_THIRSTY:
      UI_IMG(plant_unhappy, 135, 17);
      UI_IMG(plant_thirsty, 160, 31);
      break;
    case PLANT_HOT:
      UI_IMG(plant_unhappy, 135, 17);
      UI_IMG(plant_icon_hot, 160, 31);
      break;
    case PLANT_COLD:
      UI_IMG(plant_unhappy, 135, 17);
      UI_IMG(plant_icon_cold, 160, 31);
      break;
  }
}

void ui_draw_details_background() {
  UI_IMG(details_background, 21, 23);
}

void ui_draw_details_title() {
  _tft->setFreeFont(UI_FONT);
  _tft->setTextColor(TFT_WHITE);
  _tft->setTextDatum(MC_DATUM);
  _tft->setTextSize(UI_FONT_SIZE);
  _tft->drawString("Details", 160, 45);
}

void ui_draw_details_item(int16_t y) {
  UI_IMG(details_item, 35, y);
}

void ui_draw_details_item_selected(int16_t y) {
  UI_IMG(details_item_selected, 35, y);
}

void ui_draw_params_background() {
  UI_IMG(params_background, 21, 23);
}

void ui_draw_params_title(bool dirty) {
  _tft->setFreeFont(UI_FONT);
  _tft->setTextColor(TFT_WHITE);
  _tft->setTextDatum(MC_DATUM);
  _tft->setTextSize(UI_FONT_SIZE);
  _tft->drawString(dirty ? "Parametres *" : "Parametres", 160, 45);
}

void ui_draw_params_item(int16_t y) {
  UI_IMG(params_item, 35, y);
}

void ui_draw_params_item_selected(int16_t y) {
  UI_IMG(params_item_selected, 35, y);
}
