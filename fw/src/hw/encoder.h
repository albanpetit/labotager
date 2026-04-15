#pragma once
#include <stdint.h>

enum EncEvent {
  ENC_NONE,
  ENC_UP,
  ENC_DOWN,
  ENC_PRESS,
  ENC_LONG_PRESS   // press held for more than 500 ms
};

void     encoder_init();
EncEvent encoder_poll();
