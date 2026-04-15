#pragma once
#include "state.h"
#include "hw/encoder.h"

// Les 3 onglets de la barre de navigation
enum Screen {
  SCREEN_HOME = 0,
  SCREEN_DETAILS,
  SCREEN_PARAMS,
};

void display_init();

// Appelé chaque loop.
// Gère la navigation et le rendu selon l'écran courant.
// Retourne true si des Settings ont été modifiés (pour déclencher la sauvegarde).
bool display_update(SensorData &data, Settings &settings, EncEvent ev);
