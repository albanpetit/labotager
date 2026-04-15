#pragma once

// ─── GPIO ─────────────────────────────────────────────────────────────────────
#define GPIO_MOTOR    15   // Watering pump (relay / driver)
#define GPIO_LED      14   // Horticultural LEDs
#define GPIO_ENC_A     8   // Rotary encoder channel A
#define GPIO_ENC_B     9   // Rotary encoder channel B
#define GPIO_ENC_SW   28   // Rotary encoder push-switch

// SD card — SPI1 (physical pins 14-17)
#define GPIO_SD_MISO  12   // SPI1 RX  (physical pin 16)
#define GPIO_SD_CS    13   // SPI1 CS  (physical pin 17)
#define GPIO_SD_SCK   10   // SPI1 SCK (physical pin 14)
#define GPIO_SD_MOSI  11   // SPI1 TX  (physical pin 15)

// Soil moisture sensor (ADC)
#define GPIO_SOIL     27

// ─── I2C ──────────────────────────────────────────────────────────────────────
#define I2C_SDA       0
#define I2C_SCL       1
#define AHT20_ADDR    0x38
#define DS3231_ADDR   0x68

// ─── ADC ──────────────────────────────────────────────────────────────────────
#define SOIL_DRY_VAL  3500   // ADC reading in open air (dry)
#define SOIL_WET_VAL   800   // ADC reading submerged in water (wet)

// ─── SD ───────────────────────────────────────────────────────────────────────
#define SD_SPEED_MHZ  10
#define CONFIG_FILE   "/config.txt"
#define LOG_DIR       "/logs"
#define LOG_FILE      "/logs/log.csv"

// ─── Default parameter values ─────────────────────────────────────────────────
#define DEFAULT_SOIL_THRESHOLD     5    // soil moisture threshold to trigger pump (%)
#define DEFAULT_WATERING_CHECK_S   10   // seconds between pump re-checks
#define DEFAULT_LED_START_HOUR      8   // grow light on  (08:00)
#define DEFAULT_LED_START_MIN       0
#define DEFAULT_LED_END_HOUR       22   // grow light off (22:00)
#define DEFAULT_LED_END_MIN         0
#define DEFAULT_LOG_INTERVAL_S    3600  // seconds between CSV log entries
#define DEFAULT_GROWTH_DAYS         0   // elapsed grow-days counter
#define DEFAULT_OWNER_NAME    "La Machinerie"

// ─── Plant status thresholds (defaults — overridden at runtime via Settings) ──
#define DEFAULT_PLANT_TEMP_MIN   15   // below → PLANT_COLD  (°C)
#define DEFAULT_PLANT_TEMP_MAX   30   // above → PLANT_HOT   (°C)

// ─── Screen layout (landscape 320×240) ────────────────────────────────────────
#define TABBAR_Y      200   // Y origin of the tab bar
#define TABBAR_H       40   // tab bar height (px)
#define CONTENT_H     200   // content area height (Y: 0..199)
#define TAB0_X          0   // Home tab    : x 0..106
#define TAB1_X        107   // Details tab : x 107..213
#define TAB2_X        214   // Settings tab: x 214..319
#define SCREEN_W      320
#define SCREEN_H      240

// ─── Miscellaneous ────────────────────────────────────────────────────────────
#define MAX_IMAGE_WIDTH   320
#define SERIAL_BAUD       115200

// ─── Watchdog ─────────────────────────────────────────────────────────────────
// Hardware watchdog timeout in milliseconds (RP2040 max ≈ 8388 ms).
// The main loop must call Watchdog::kick() within this window or the board resets.
// Set to 8 s to tolerate slow SD operations while catching genuine freezes.
#define WATCHDOG_MS       8000
