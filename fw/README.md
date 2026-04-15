# Labotager — Firmware

Système de culture automatisé basé sur un Raspberry Pi Pico (RP2040).
Le firmware gère l'arrosage automatique, l'éclairage horticole, le logging des données et une interface utilisateur TFT avec encodeur rotatif.

---

## Composants matériels

| Composant | Rôle |
|---|---|
| Raspberry Pi Pico (RP2040) | Microcontrôleur principal |
| Écran TFT ST7789 320×240 | Interface utilisateur |
| Encodeur rotatif | Navigation menu |
| AHT20 (I2C 0x38) | Capteur température + humidité ambiante |
| DS3231M (I2C 0x68) | RTC temps réel |
| Capteur humidité sol (ADC) | Mesure humidité substrat |
| Carte micro-SD (SPI1) | Logging CSV + persistance configuration |
| Pompe (relais/driver) | Arrosage automatique |
| LEDs horticoles | Éclairage planifié |

---

## Brochage GPIO

| GPIO | Fonction |
|---|---|
| GP0 | I2C SDA (AHT20 + DS3231M) |
| GP1 | I2C SCL (AHT20 + DS3231M) |
| GP2 | TFT SCK (SPI0) |
| GP3 | TFT MOSI (SPI0) |
| GP4 | TFT MISO / RST (SPI0) |
| GP5 | TFT DC |
| GP6 | TFT CS |
| GP7 | TFT Backlight |
| GP8 | Encodeur A |
| GP9 | Encodeur B |
| GP10 | SD SCK (SPI1 — physique pin 14) |
| GP11 | SD MOSI (SPI1 — physique pin 15) |
| GP12 | SD MISO (SPI1 — physique pin 16) |
| GP13 | SD CS (SPI1 — physique pin 17) |
| GP14 | LEDs horticoles |
| GP15 | Pompe (arrosage) |
| GP27 | Capteur humidité sol (ADC) |
| GP28 | Encodeur switch |

---

## Architecture logicielle

```
src/
  main.cpp        — setup() + loop() : orchestration pure
  config.h        — GPIO, constantes, valeurs par défaut
  state.h         — structs SensorData et Settings (partagées)
  encoder.h/.cpp  — ISR Pico SDK + file d'événements (NONE/UP/DOWN/PRESS/LONG_PRESS)
  sensors.h/.cpp  — AHT20 non-bloquant + lecture ADC humidité sol
  rtc_mgr.h/.cpp  — DS3231M : init, lecture périodique, gestion OSF
  pump.h/.cpp     — Machine à états arrosage (IDLE / PUMPING)
  lighting.h/.cpp — Scheduling LED horticole basé sur RTC
  logger.h/.cpp   — SD init, /config.txt, /logs/log.csv
  display.h/.cpp  — TFT + PNG + menu navigation encodeur
  interface.h     — Image PNG composant home (PROGMEM)
  interface_2.h   — Image PNG composant menu (PROGMEM)
  interface_3.h   — Image PNG composant boutons (PROGMEM)
```

### Structs partagées (`state.h`)

**`SensorData`** — données mises à jour chaque cycle :
- `air_temp`, `air_humidity` — AHT20
- `soil_raw`, `soil_pct` — capteur sol (ADC 12 bits, 0–4095)
- `hour/minute/second/day/month/year` — RTC
- `pump_on`, `led_on` — état actionneurs
- `last_log_millis` — timer logging

**`Settings`** — paramètres utilisateur persistés :
- `soil_threshold` — seuil déclenchement pompe (%)
- `watering_check_s` — intervalle re-vérification arrosage (s)
- `led_start_hour/min` — heure début éclairage
- `led_duration_hours` — durée éclairage
- `log_interval_s` — intervalle log CSV (s)

---

## Logique arrosage

```
       soil_pct < threshold
IDLE ─────────────────────────► PUMPING  (pompe ON)
  ▲                                │
  │   soil_pct >= threshold        │ toutes les watering_check_s secondes
  └────────────────────────────────┘                    re-vérification
```

La vérification se fait sur la valeur de `soil_pct` déjà calculée dans `sensors_update()`, sans délai bloquant.

---

## Logique éclairage

Calcul en minutes depuis minuit :
```
now_min   = hour * 60 + minute
start_min = led_start_hour * 60 + led_start_min
end_min   = start_min + led_duration_hours * 60

Si end_min <= 1440 : LED ON si start_min <= now_min < end_min
Si end_min >  1440 : LED ON si now_min >= start_min OU now_min < (end_min - 1440)
```

Supporte les plages qui franchissent minuit (ex. 20h + 16h = jusqu'à 12h le lendemain).

---

## Fichier de configuration `/config.txt`

Format key=value, un paramètre par ligne. Créé automatiquement avec les valeurs par défaut si absent.

```
soil_threshold=30
watering_check_s=60
led_start_hour=8
led_start_min=0
led_duration_hours=16
log_interval_s=300
```

Les valeurs sont rechargées au démarrage. Toute modification via le menu est sauvegardée immédiatement.

---

## Format du log CSV `/logs/log.csv`

```
datetime,air_temp_c,air_hum_pct,soil_pct,pump,led
2025-06-15T08:05:00,23.45,58.2,42,0,1
2025-06-15T08:10:00,23.51,57.8,35,1,1
```

- `pump` et `led` : 0 = OFF, 1 = ON
- Une ligne toutes les `log_interval_s` secondes (défaut : 5 minutes)

---

## Interface utilisateur

### Navigation encodeur

| Événement | Action |
|---|---|
| Rotation | Déplacer curseur / modifier valeur (mode édition) |
| Appui court | Entrer menu / valider / basculer mode édition |
| Appui long (> 500 ms) | Retour au menu précédent / home |

### Structure des menus

```
SCREEN_HOME (données temps réel, rafraîchi toutes les 2s)
  └─ [PRESS] ──► MENU_MAIN
                  ├─ Arrosage  ──► MENU_ARROSAGE
                  │               ├─ Seuil sol %
                  │               ├─ Intervalle s
                  │               └─ Retour
                  ├─ Eclairage ──► MENU_ECLAIRAGE
                  │               ├─ Heure debut
                  │               ├─ Minute debut
                  │               ├─ Duree h
                  │               └─ Retour
                  ├─ Logging   ──► MENU_LOGGING
                  │               ├─ Intervalle s
                  │               └─ Retour
                  └─ Retour
```

---

## Build PlatformIO

```bash
# Compiler
pio run

# Compiler + flasher
pio run --target upload

# Moniteur série
pio device monitor --baud 115200
```

Environnement : `pico` (Raspberry Pi Pico, framework arduino-mbed).

---

## Notes techniques

### AHT20 — bypass `available()`
`MbedI2C::available()` retourne 0 même quand des octets sont reçus (bug connu arduino-mbed).
Solution : appeler `requestFrom()` puis `read()` directement, en ignorant `available()`.

### AHT20 non-bloquant
La mesure AHT20 nécessite 80 ms de traitement. Le firmware utilise une machine à états (IDLE → TRIGGERED → lecture) pour ne pas bloquer le loop pendant ce délai.

### DS3231M — patch bibliothèque
La bibliothèque DFRobot_DS3231M utilisait `Wire.beginTransmission()` au lieu de `_pWire->beginTransmission()`.
Le fichier `lib/DS3231M/DS3231M.cpp` a été corrigé pour supporter le bus I2C custom (`MbedI2C`).

### SPI0 (TFT) et SPI1 (SD) — séparation matérielle
L'écran TFT utilise SPI0 (GP2/3/4/5/6) et la carte SD utilise SPI1 (GP10/11/12/13).
`SPI_DRIVER_SELECT=0` dans `platformio.ini` indique à SdFat d'utiliser l'API SPI native mbed.

### Calibration capteur sol
Le capteur capacitif retourne une valeur ADC haute quand le substrat est sec et basse quand il est humide.
`map(raw, SOIL_DRY_VAL, SOIL_WET_VAL, 0, 100)` gère automatiquement cette inversion.
Valeurs par défaut : `SOIL_DRY_VAL=3500`, `SOIL_WET_VAL=800`.
À recalibrer avec votre capteur si nécessaire (modifier `config.h`).

### PNG — images UI
Les images PNG sont stockées en flash (PROGMEM) dans les fichiers `interface*.h`.
Rendu via `openFLASH()` + `decode()` + `close()` à chaque affichage.
`openFLASH()` et `openRAM()` sont les seules API valides dans PNGdec (pas de `open()` générique).
