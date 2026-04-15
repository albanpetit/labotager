#pragma once
#include "state.h"

// Initialise la SD, charge /config.txt dans settings.
// Retourne false si la carte SD est absente ou inaccessible.
bool logger_init(Settings &settings);

// Ecrit une ligne CSV si l'intervalle est écoulé
void logger_update(SensorData &data, const Settings &settings);

// Persiste les settings dans /config.txt
void logger_save_settings(const Settings &settings);
