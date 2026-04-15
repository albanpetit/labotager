#include <Arduino.h>
#include "hardware/gpio.h"
#include <TFT_eSPI.h>
#include <PNGdec.h>
#include <Wire.h>
#include <SPI.h>
#include <SdFat.h>
#include <DFRobot_DS3231M.h>
#include "interface.h"
#include "interface_2.h"
#include "interface_3.h"

#define TEST_ECRAN
#define TEST_LED
#define TEST_MOTOR
//#define TEST_RTC
//#define TEST_ENCODEUR
//#define TEST_I2C_SCAN
#define TEST_SD
//#define TEST_AHT20
//#define TEST_SOIL_MOISTURE

#define GPIO_MOTOR   15
#define GPIO_LED     14
#define GPIO_ENC_A    8
#define GPIO_ENC_B    9
#define GPIO_ENC_SW  28
#define GPIO_SD_DAT0 12   // MISO  — SPI1 RX  (pin 16)
#define GPIO_SD_CS   13   // CS    — SPI1 CS  (pin 17)
#define GPIO_SD_CLK  10   // SCK   — SPI1 SCK (pin 14)
#define GPIO_SD_CMD  11   // MOSI  — SPI1 TX  (pin 15)

#define GPIO_SOIL_MOISTURE 27
#define SOIL_DRY_VAL  3500   // valeur ADC capteur dans l'air (sec)
#define SOIL_WET_VAL   800   // valeur ADC capteur dans l'eau (mouille)

#define DS3231_ADDR  0x68
#define AHT20_ADDR   0x38

MbedI2C rtcWire(0, 1);          // SDA=GP0, SCL=GP1
DFRobot_DS3231M rtc(&rtcWire);  // passe le bus I2C custom à la lib

PNG png;
TFT_eSPI tft = TFT_eSPI();

#define MAX_IMAGE_WIDTH 320
int16_t xpos = 0;
int16_t ypos = 0;

uint32_t lastToggleLed = 0;
uint32_t lastToggleMotor = 0;
bool outputStateLed = false;
bool outputStateMotor = false;

uint32_t lastRtcRead = 0;

bool     aht20Ready   = false;
uint32_t lastAht20Read = 0;

#ifdef TEST_SD
MbedSPI sdSPI(GPIO_SD_DAT0, GPIO_SD_CMD, GPIO_SD_CLK);  // MISO, MOSI, SCK → SPI1
SdFat32 sd;
bool sdReady = false;
uint32_t sdWriteCount = 0;
#endif

volatile int  encCount    = 0;
volatile int  encLastA    = HIGH;
int           encCountLast = 0;
volatile bool swPending   = false;
int           swLastState  = HIGH;
uint32_t      swLastChange = 0;

int pngDraw(PNGDRAW *pDraw) {
  uint16_t lineBuffer[MAX_IMAGE_WIDTH];
  png.getLineAsRGB565(pDraw, lineBuffer, PNG_RGB565_BIG_ENDIAN, 0xffffffff);
  tft.pushImage(xpos, ypos + pDraw->y, pDraw->iWidth, 1, lineBuffer);
  return 1;
}

static uint32_t lastEcranToggle = 0;
static bool     ecranShowImage  = true;

void test_ecran() {
  if (millis() - lastEcranToggle < 3000) return;
  lastEcranToggle = millis();

  if (ecranShowImage) {
    int16_t rc = png.openFLASH((uint8_t*)interface, sizeof(interface), pngDraw);
    if (rc != PNG_SUCCESS) {
      Serial.print("Ecran : echec ouverture PNG ("); Serial.print(rc); Serial.println(")");
      return;
    }
    tft.startWrite();
    uint32_t dt = millis();
    png.decode(NULL, 0);
    tft.endWrite();
    png.close();
    Serial.print("Ecran : image  ("); Serial.print(millis() - dt); Serial.println("ms)");
  } else {
    tft.fillScreen(TFT_WHITE);
    Serial.println("Ecran : blanc");
  }

  ecranShowImage = !ecranShowImage;
}

void test_led() {
  if (millis() - lastToggleLed >= 1000) {
    lastToggleLed = millis();
    outputStateLed = !outputStateLed;
    digitalWrite(GPIO_LED, outputStateLed);
    Serial.print("LED : "); Serial.println(outputStateLed ? "ON" : "OFF");
  }
}

void test_motor() {
  if (millis() - lastToggleMotor >= 1000) {
    lastToggleMotor = millis();
    outputStateMotor = !outputStateMotor;
    digitalWrite(GPIO_MOTOR, outputStateMotor);
    Serial.print("MOTOR : "); Serial.println(outputStateMotor ? "ON" : "OFF");
  }
}

static void encodeur_irq(uint gpio, uint32_t events) {
  if (gpio == GPIO_ENC_A) {
    int a = (int)gpio_get(GPIO_ENC_A);
    int b = (int)gpio_get(GPIO_ENC_B);
    if (a != encLastA) {
      encCount += (b != a) ? +1 : -1;
      encLastA = a;
    }
  } else if (gpio == GPIO_ENC_SW) {
    swPending = true;
  }
}

void test_encodeur() {
  // Rotation : report des changements de position
  int count = encCount;
  if (count != encCountLast) {
    Serial.print("ENCODEUR : position="); Serial.print(count);
    Serial.println(count > encCountLast ? "  [->]" : "  [<-]");
    encCountLast = count;
  }

  // Switch : anti-rebond 50ms dans le loop
  if (swPending && (millis() - swLastChange) > 50) {
    swPending = false;
    swLastChange = millis();
    int state = digitalRead(GPIO_ENC_SW);
    if (state != swLastState) {
      swLastState = state;
      if (state == LOW) Serial.println("ENCODEUR : switch appuye");
      else              Serial.println("ENCODEUR : switch relache");
    }
  }
}

void test_i2c_scan() {
  Serial.println("I2C scan (GP0=SDA, GP1=SCL)...");
  uint8_t found = 0;
  for (uint8_t addr = 0x01; addr < 0x7F; addr++) {
    rtcWire.beginTransmission(addr);
    if (rtcWire.endTransmission() == 0) {
      char buf[40];
      sprintf(buf, "  -> peripherique : 0x%02X", addr);
      Serial.println(buf);
      found++;
    }
  }
  if (found == 0) Serial.println("  -> aucun peripherique detecte");
  else { char buf[32]; sprintf(buf, "  -> %d peripherique(s) trouve(s)", found); Serial.println(buf); }
  Serial.println("I2C scan termine.");
}

#ifdef TEST_SD
void setup_sd() {
  SdSpiConfig sdConfig(GPIO_SD_CS, DEDICATED_SPI, SD_SCK_MHZ(10), &sdSPI);
  if (!sd.begin(sdConfig)) {
    Serial.println("SD : echec initialisation (carte presente ?)");
    return;
  }
  Serial.println("SD : initialise");
  sdReady = true;
}

void test_sd() {
  if (!sdReady) return;

  sdWriteCount++;
  File32 f = sd.open("counter.txt", O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) {
    Serial.println("SD : echec ouverture fichier");
    return;
  }
  f.println(sdWriteCount);
  f.close();
  Serial.print("SD : mise a jour -> "); Serial.println(sdWriteCount);
  delay(100);
}
#endif

// AHT20 : implémentation directe via rtcWire (pas de lib), adresse 0x38
// Protocol : trigger [0xAC, 0x33, 0x00] → 80ms → lire 6 octets
void setup_aht20() {
  // Vérification présence
  rtcWire.beginTransmission(AHT20_ADDR);
  if (rtcWire.endTransmission() != 0) {
    Serial.println("AHT20 : non detecte (0x38 absent sur le bus)");
    return;
  }

  // Lecture statut brut — bypass available() (bug potentiel MbedI2C)
  rtcWire.requestFrom((uint8_t)AHT20_ADDR, (uint8_t)1);
  int rawStatus = rtcWire.read();
  uint8_t status = (rawStatus >= 0) ? (uint8_t)rawStatus : 0x00;
  Serial.print("AHT20 dbg setup : avail="); Serial.print(rtcWire.available());
  Serial.print(" raw="); Serial.print(rawStatus);
  Serial.print(" status=0x"); Serial.println(status, HEX);

  // Initialisation si le bit calibration (bit3) n'est pas positionné
  if (!(status & 0x08)) {
    rtcWire.beginTransmission(AHT20_ADDR);
    rtcWire.write(0xBE); rtcWire.write(0x08); rtcWire.write(0x00);
    rtcWire.endTransmission();
    delay(10);

    rtcWire.requestFrom((uint8_t)AHT20_ADDR, (uint8_t)1);
    rawStatus = rtcWire.read();
    status = (rawStatus >= 0) ? (uint8_t)rawStatus : 0x00;
    if (!(status & 0x08)) {
      Serial.println("AHT20 : echec calibration");
      return;
    }
  }

  Serial.println("AHT20 : detecte");
  aht20Ready = true;
}

void test_aht20() {
  if (!aht20Ready) return;
  if (millis() - lastAht20Read < 2000) return;
  lastAht20Read = millis();

  // Déclenchement mesure
  rtcWire.beginTransmission(AHT20_ADDR);
  rtcWire.write(0xAC); rtcWire.write(0x33); rtcWire.write(0x00);
  uint8_t txErr = (uint8_t)rtcWire.endTransmission();
  if (txErr != 0) {
    Serial.print("AHT20 : echec commande (err="); Serial.print(txErr); Serial.println(")");
    return;
  }
  delay(80);

  // Lecture brute — bypass available(), lit les octets directement
  uint8_t got = rtcWire.requestFrom((uint8_t)AHT20_ADDR, (uint8_t)6);
  uint8_t data[6];
  for (int i = 0; i < 6; i++) {
    int b = rtcWire.read();
    data[i] = (b >= 0) ? (uint8_t)b : 0xFF;
  }

  // Diagnostic systématique
  Serial.print("AHT20 dbg : got="); Serial.print(got);
  Serial.print(" raw=");
  for (int i = 0; i < 6; i++) { Serial.print(data[i], HEX); Serial.print(' '); }
  Serial.println();

  if (data[0] == 0xFF && data[1] == 0xFF && data[2] == 0xFF) {
    Serial.println("AHT20 : echec lecture I2C (bus/NACK)");
    return;
  }
  // Vérif bit busy (bit7 du premier octet)
  if (data[0] & 0x80) {
    Serial.println("AHT20 : capteur toujours occupe apres 80ms");
    return;
  }

  // Décodage humidité et température
  uint32_t rawHum  = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | (data[3] >> 4);
  uint32_t rawTemp = (((uint32_t)data[3] & 0x0F) << 16) | ((uint32_t)data[4] << 8) | data[5];
  float humidity = (float)rawHum  * 100.0f / 1048576.0f;
  float temp     = (float)rawTemp * 200.0f / 1048576.0f - 50.0f;

  char buf[60];
  sprintf(buf, "AHT20 : temp=%.2f C  hum=%.1f %%RH", temp, humidity);
  Serial.println(buf);
}

void test_rtc() {
  if (millis() - lastRtcRead < 1000) return;
  lastRtcRead = millis();

  rtc.getNowTime();

  char buf[40];
  sprintf(buf, "RTC : %02d/%02d/%04d  %02d:%02d:%02d",
    rtc.day(), rtc.month(), rtc.year(),
    rtc.hour(), rtc.minute(), rtc.second());
  Serial.print(buf);

  if (rtc.lostPower()) Serial.print("  [WARN: oscillateur arrete - relancer adjust()]");
  Serial.println();

  Serial.print("RTC temp : ");
  Serial.print(rtc.getTemperatureC());
  Serial.println(" C");
}

void setup() {
  Serial.begin(115200);
  delay(3000);

  tft.begin();
  tft.fillScreen(TFT_BLACK);
  tft.setRotation(3);
  tft.setSwapBytes(false);

  #ifdef TEST_SD
    setup_sd();
  #endif

  pinMode(GPIO_MOTOR, OUTPUT);
  pinMode(GPIO_LED, OUTPUT);

  #ifdef TEST_SOIL_MOISTURE
    analogReadResolution(12);  // résolution max RP2040 : 12 bits (0-4095)
  #endif

  Serial.println("Initialisation done.");

  #if defined(TEST_I2C_SCAN) || defined(TEST_RTC) || defined(TEST_AHT20)
    rtcWire.begin();
  #endif

  #ifdef TEST_I2C_SCAN
    test_i2c_scan();
  #endif

  #ifdef TEST_RTC
    if (!rtc.begin()) {
      Serial.println("RTC : DS3231M non detecte");
    } else {
      Serial.println("RTC : DS3231M detecte");
      if (rtc.lostPower()) {
        Serial.println("RTC : oscillateur arrete, remise a l'heure de compilation");
        rtc.dateTime();
        rtc.adjust();
      }
    }
  #endif

  #ifdef TEST_ENCODEUR
    gpio_pull_up(GPIO_ENC_A);
    gpio_pull_up(GPIO_ENC_B);
    gpio_pull_up(GPIO_ENC_SW);
    encLastA    = (int)gpio_get(GPIO_ENC_A);
    swLastState = (int)gpio_get(GPIO_ENC_SW);
    gpio_set_irq_enabled_with_callback(GPIO_ENC_A,  GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true, encodeur_irq);
    gpio_set_irq_enabled(GPIO_ENC_SW, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);
  #endif

  #ifdef TEST_AHT20
    setup_aht20();
  #endif
}

void loop() {
  #ifdef TEST_ECRAN
    test_ecran();
  #endif

  #ifdef TEST_LED
    test_led();
  #endif

  #ifdef TEST_SOIL_MOISTURE
    int soilRaw = analogRead(GPIO_SOIL_MOISTURE);
    int soilPct = constrain(map(soilRaw, SOIL_DRY_VAL, SOIL_WET_VAL, 0, 100), 0, 100);
    Serial.print("Soil moisture : raw="); Serial.print(soilRaw);
    Serial.print("  pct="); Serial.print(soilPct); Serial.println("%");
  #endif

  #ifdef TEST_MOTOR
    test_motor();
  #endif

  #ifdef TEST_ENCODEUR
    test_encodeur();
  #endif

  #ifdef TEST_RTC
    test_rtc();
  #endif

  #ifdef TEST_SD
    test_sd();
  #endif

  #ifdef TEST_AHT20
    test_aht20();
  #endif
}
