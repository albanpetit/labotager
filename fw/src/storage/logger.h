#pragma once
#include "state.h"

// Initialise la SD, charge /config.txt dans settings.
// Retourne false si la carte SD est absente ou inaccessible.
bool logger_init(Settings &settings);

// Ecrit une ligne CSV si l'intervalle est écoulé.
// kick_wdt (optionnel) est appelé juste avant l'écriture SD pour éviter
// un timeout watchdog sur une carte lente. Passer nullptr pour désactiver.
void logger_update(SensorData &data, const Settings &settings, void (*kick_wdt)() = nullptr);

// Persiste les settings dans /config.txt
void logger_save_settings(const Settings &settings, void (*kick_wdt)() = nullptr);
