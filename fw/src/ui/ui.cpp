#include "ui.h"
#include "../config.h"
#include <PNGdec.h>
#include <TFT_eSPI.h>

// ─── Component image data (PROGMEM) ──────────────────────────────────────────
// Include one header per converted PNG asset.
// To add a new component:
//   1. Convert the PNG with tools/convert_image.py (or png2h equivalent)
//   2. Place the resulting header in src/ui/components/
//   3. Include it here and add a draw function below.

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
#include "components/plant_icon_thirsty.h"
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

static TFT_eSPI    *_tft       = nullptr;
static TFT_eSprite *_spr       = nullptr;   // non-null during sprite decode

// Home widget sprites — allocated once in ui_init(), reused each frame.
// createSprite()/deleteSprite() on every draw() fragments the mbed heap;
// pointer allocation avoids fragmentation over long runtimes.
static TFT_eSprite *_spr_date = nullptr;
static TFT_eSprite *_spr_time = nullptr;
static TFT_eSprite *_spr_grow = nullptr;
static PNG          _png;
static int16_t      _png_x, _png_y;
static bool         _png_opaque = false;

// PNGdec line callback — pushes each decoded row to the active target.
// When _spr is set the row goes into the sprite buffer (x offset = 0).
// When _spr is null the row is pushed directly to the TFT at (_png_x, _png_y).
// _png_opaque=true  → pushImage for every pixel (background images).
// _png_opaque=false → pushMaskedImage when alpha present (icons on backgrounds).
// Transparent color key used when decoding a PNG into a sprite.
// Pixels below the alpha threshold keep this color so pushSprite(x,y,SPRITE_KEY)
// can skip them and let the underlying TFT content show through.
// Value chosen to be absent from warm-tone UI widget palettes.
#define SPRITE_KEY  ((uint16_t)0xF81F)   // pure magenta in RGB565

static int png_cb(PNGDRAW *p) {
  uint16_t buf[MAX_IMAGE_WIDTH];
  _png.getLineAsRGB565(p, buf, PNG_RGB565_BIG_ENDIAN, 0xffffffff);

  if (p->iHasAlpha && !_png_opaque) {
    uint8_t mask[1 + MAX_IMAGE_WIDTH / 8];
    _png.getAlphaMask(p, mask, 128);
    if (_spr) {
      // drawPixel on TFT_eSprite byte-swaps before storing → R and B are inverted.
      // pushImage does NOT byte-swap (setSwapBytes(false)) → correct colors.
      // Replicate pushMaskedImage behavior: find contiguous opaque runs and call
      // pushImage for each run.  Transparent pixels keep the pre-filled SPRITE_KEY.
      int run_start = -1;
      for (int x = 0; x <= p->iWidth; x++) {
        bool opaque = (x < p->iWidth) && (mask[x >> 3] & (0x80 >> (x & 7)));
        if (opaque && run_start < 0) {
          run_start = x;
        } else if (!opaque && run_start >= 0) {
          _spr->pushImage(run_start, p->y, x - run_start, 1, buf + run_start);
          run_start = -1;
        }
      }
    } else {
      _tft->pushMaskedImage(_png_x, _png_y + p->y, p->iWidth, 1, buf, mask);
    }
  } else {
    if (_spr)
      _spr->pushImage(0, p->y, p->iWidth, 1, buf);
    else
      _tft->pushImage(_png_x, _png_y + p->y, p->iWidth, 1, buf);
  }
  return 1;
}

// Decode a PROGMEM PNG into an already-created sprite.
// The sprite must be pre-filled with SPRITE_KEY before calling this function.
// Non-transparent pixels are written via drawPixel; transparent ones keep the key.
static void ui_png_to_sprite(TFT_eSprite &spr, const uint8_t *data, size_t len) {
  _spr        = &spr;
  _png_opaque = false;
  if (_png.openFLASH((uint8_t *)data, len, png_cb) == PNG_SUCCESS) {
    _png.decode(NULL, 0);
    _png.close();
  }
  _spr = nullptr;
}

// ─── Public ───────────────────────────────────────────────────────────────────

void ui_init(TFT_eSPI &tft) {
  _tft = &tft;

  // Create home widget sprites once — 92×29 px each.
  // Pixel buffers stay allocated for the whole session; only fill+push
  // happens on each draw call, avoiding repeated heap alloc/free.
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

void ui_draw_png(const uint8_t *data, size_t len, int16_t x, int16_t y, bool opaque) {
  if (!_tft) return;
  _png_x      = x;
  _png_y      = y;
  _png_opaque = opaque;
  if (_png.openFLASH((uint8_t *)data, len, png_cb) == PNG_SUCCESS) {
    _png.decode(NULL, 0);
    _png.close();
  }
}

// ─── Components ───────────────────────────────────────────────────────────────

void ui_draw_menu_background() {
  _tft->pushMaskedImage(0, 201, MENU_BACKGROUND_W, MENU_BACKGROUND_H,
                        (uint16_t *)menu_background, (uint8_t *)menu_background_mask);
}

void ui_draw_menu_icon_details_selected() {
  ui_draw_png(menu_icon_details_selected, sizeof(menu_icon_details_selected), 95, 198);
}

void ui_draw_menu_icon_home_selected() {
  ui_draw_png(menu_icon_home_selected, sizeof(menu_icon_home_selected), 134, 198);
}

void ui_draw_menu_icon_settings_selected() {
  ui_draw_png(menu_icon_settings_selected, sizeof(menu_icon_settings_selected), 173, 198);
}

void ui_draw_menu_icon_details() {
  ui_draw_png(menu_icon_details, sizeof(menu_icon_details), 116, 217);
}

void ui_draw_menu_icon_home() {
  ui_draw_png(menu_icon_home, sizeof(menu_icon_home), 153, 217);
}

void ui_draw_menu_icon_settings() {
  ui_draw_png(menu_icon_settings, sizeof(menu_icon_settings), 192, 217);
}

void ui_draw_background() {
  _tft->pushImage(0, 0, BACKGROUND_W, BACKGROUND_H, (uint16_t *)background);
}

void ui_draw_icon_error() {
  ui_draw_png(icon_error, sizeof(icon_error), (320-45)/2, 80);
}

void ui_draw_home_date(const char *date) {
  _spr_date->fillSprite(SPRITE_KEY);
  ui_png_to_sprite(*_spr_date, home_date, sizeof(home_date));
  _spr_date->setFreeFont(UI_FONT);
  _spr_date->setTextColor(UI_COLOR_TEXT);
  _spr_date->setTextDatum(MC_DATUM);
  _spr_date->setTextSize(UI_FONT_SIZE);
  _spr_date->drawString(date, 53, 11);
  _spr_date->pushSprite(17, 29, SPRITE_KEY);
}

void ui_draw_home_time(const char *time) {
  _spr_time->fillSprite(SPRITE_KEY);
  ui_png_to_sprite(*_spr_time, home_time, sizeof(home_time));
  _spr_time->setFreeFont(UI_FONT);
  _spr_time->setTextColor(UI_COLOR_TEXT);
  _spr_time->setTextDatum(MC_DATUM);
  _spr_time->setTextSize(UI_FONT_SIZE);
  _spr_time->drawString(time, 53, 11);
  _spr_time->pushSprite(17, 59, SPRITE_KEY);
}

void ui_draw_home_grow_counter(const char *days) {
  _spr_grow->fillSprite(SPRITE_KEY);
  ui_png_to_sprite(*_spr_grow, home_grow_counter, sizeof(home_grow_counter));
  _spr_grow->setFreeFont(UI_FONT);
  _spr_grow->setTextColor(UI_COLOR_TEXT);
  _spr_grow->setTextDatum(MC_DATUM);
  _spr_grow->setTextSize(UI_FONT_SIZE);
  _spr_grow->drawString(days, 53, 11);
  _spr_grow->pushSprite(18, 89, SPRITE_KEY);
}

void ui_draw_home_owner(const char *owner_name) {
  _tft->pushMaskedImage(12, 135, HOME_OWNER_W, HOME_OWNER_H,
                    (uint16_t *)home_owner, (uint8_t *)home_owner_mask);
  _tft->setTextColor(UI_COLOR_TEXT);
  _tft->setTextDatum(MC_DATUM);
  _tft->setTextSize(UI_FONT_SIZE);
  _tft->drawString("Labotager de", 73, 152);
  _tft->drawString(owner_name, 73, 170);

}

void ui_draw_home_plant_status(PlantState state) {
  switch (state) {
    case PLANT_HAPPY:   ui_draw_png(plant_happy,   sizeof(plant_happy),   184, 49); break;
    case PLANT_THIRSTY: {
      ui_draw_png(plant_unhappy, sizeof(plant_unhappy), 135, 17); 
      ui_draw_png(plant_icon_thirsty, sizeof(plant_icon_thirsty), 160, 31); 
      break;
    }
    case PLANT_HOT: {
      ui_draw_png(plant_unhappy, sizeof(plant_unhappy), 135, 17); 
      ui_draw_png(plant_icon_hot, sizeof(plant_icon_hot), 160, 31); 
      break;
    }
    case PLANT_COLD: {
      ui_draw_png(plant_unhappy, sizeof(plant_unhappy), 135, 17); 
      ui_draw_png(plant_icon_cold, sizeof(plant_icon_cold), 160, 31); 
      break;
    }
  }
}

void ui_draw_details_background() {
  _tft->pushMaskedImage(21, 23, DETAILS_BACKGROUND_W, DETAILS_BACKGROUND_H,
                  (uint16_t *)details_background, (uint8_t *)details_background_mask);
}

void ui_draw_details_title() {
  _tft->setFreeFont(UI_FONT);
  _tft->setTextColor(TFT_WHITE);
  _tft->setTextDatum(MC_DATUM);
  _tft->setTextSize(UI_FONT_SIZE);
  _tft->drawString("Details", 160, 45);
}

void ui_draw_details_item(int16_t y) {
  _tft->pushMaskedImage(35, y, DETAILS_ITEM_W, DETAILS_ITEM_H,
                  (uint16_t *)details_item, (uint8_t *)details_item_mask);
}

void ui_draw_details_item_selected(int16_t y) {
  _tft->pushMaskedImage(35, y, DETAILS_ITEM_SELECTED_W, DETAILS_ITEM_SELECTED_H,
                (uint16_t *)details_item_selected, (uint8_t *)details_item_selected_mask);
}

void ui_draw_params_background() {
  _tft->pushMaskedImage(21, 23, PARAMS_BACKGROUND_W, PARAMS_BACKGROUND_H,
                  (uint16_t *)params_background, (uint8_t *)params_background_mask);
}

void ui_draw_params_title(bool dirty) {
  _tft->setFreeFont(UI_FONT);
  _tft->setTextColor(TFT_WHITE);
  _tft->setTextDatum(MC_DATUM);
  _tft->setTextSize(UI_FONT_SIZE);
  _tft->drawString(dirty ? "Parametres *" : "Parametres", 160, 45);
}

void ui_draw_params_item(int16_t y) {
  _tft->pushMaskedImage(35, y, PARAMS_ITEM_W, PARAMS_ITEM_H,
                  (uint16_t *)params_item, (uint8_t *)params_item_mask);
}

void ui_draw_params_item_selected(int16_t y) {
  _tft->pushMaskedImage(35, y, PARAMS_ITEM_SELECTED_W, PARAMS_ITEM_SELECTED_H,
                (uint16_t *)params_item_selected, (uint8_t *)params_item_selected_mask);
}