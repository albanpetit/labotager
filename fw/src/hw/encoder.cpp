#include "encoder.h"
#include "config.h"
#include <Arduino.h>
#include "hardware/gpio.h"
#include <mbed.h>

// ─── Rotation — 4 kHz timer-based quadrature polling ─────────────────────────
//
// Edge interrupts are unreliable for slow rotation: contact bounce generates
// rapid edges that can coalesce inside the IRQ controller, causing the
// state machine to read intermediate states and produce delta=0 (ignored)
// instead of the real transition.
//
// Polling at 4 kHz (every 250 µs) solves this:
//   • Bounce shorter than ~250 µs is invisible — the pin is sampled only once
//     per period, always seeing the stable post-bounce level.
//   • At max comfortable rotation (3 rev/s × 24 detents × 4 transitions = 288
//     transitions/s) we still get ~14 samples per transition — more than enough.
//   • The quadrature state machine is identical; only the sampling mechanism
//     changes.
//
// PEC12R-4225F-S0024: detents at A=HIGH, B=HIGH (binary 11 = state 3).
//   CW  : 11 → 01 → 00 → 10 → 11  (+1 per transition, +4 per detent)
//   CCW : 11 → 10 → 00 → 01 → 11  (-1 per transition, -4 per detent)

static const int8_t ENC_DELTA[4][4] = {
//  next: 00  01  10  11
  {  0, -1, +1,  0 },   // prev: 00
  { +1,  0,  0, -1 },   // prev: 01
  { -1,  0,  0, +1 },   // prev: 10
  {  0, +1, -1,  0 },   // prev: 11
};

static volatile uint8_t enc_state = 3;   // A=HIGH, B=HIGH (detent)
static volatile int     enc_accum = 0;   // ±1 per transition; ±4 = one detent

static mbed::Ticker enc_ticker;

static void enc_poll_cb() {
  uint8_t state = (gpio_get(GPIO_ENC_A) ? 2 : 0) |
                  (gpio_get(GPIO_ENC_B) ? 1 : 0);
  if (state != enc_state) {
    int8_t delta = ENC_DELTA[enc_state][state];
    // Direction change mid-detent: reset accumulator so the first full detent
    // in the new direction fires immediately without having to unwind first.
    if (delta != 0 && enc_accum != 0 && ((delta > 0) != (enc_accum > 0)))
      enc_accum = 0;
    enc_accum += delta;
    enc_state  = state;
  }
}

// ─── Switch — polling state machine ───────────────────────────────────────────

static uint8_t  sw_state   = 0;
static uint32_t sw_ts      = 0;
static uint32_t sw_press_t = 0;

// ─── Public ───────────────────────────────────────────────────────────────────

void encoder_init() {
  pinMode(GPIO_ENC_A, INPUT_PULLUP);
  pinMode(GPIO_ENC_B, INPUT_PULLUP);

  // Start the quadrature polling timer via mbed Ticker.
  enc_ticker.attach_us(enc_poll_cb, ENC_POLL_US);

  // Switch — Arduino polling, no ISR needed
  pinMode(GPIO_ENC_SW, INPUT_PULLUP);
}

EncEvent encoder_poll() {
  // ── Rotation — read timer-callback accumulator atomically ─────────────────
  // Snapshot and reset in one critical section so no transitions are lost
  // between the read and the subtraction.
  __disable_irq();
  int accum = enc_accum;
  enc_accum = 0;
  __enable_irq();
  if      (accum >=  ENC_DETENT_TRANSITIONS) return ENC_DOWN;
  else if (accum <= -ENC_DETENT_TRANSITIONS) return ENC_UP;
  // Sub-detent move: restore remaining accumulator (keeps partial counts)
  __disable_irq();
  enc_accum += accum;
  __enable_irq();

  // ── Switch ────────────────────────────────────────────────────────────────
  bool pressed = (digitalRead(GPIO_ENC_SW) == LOW);
  uint32_t now = millis();

  switch (sw_state) {
    case 0:   // idle
      if (pressed) { sw_state = 1; sw_ts = now; }
      break;

    case 1:   // press debounce
      if (!pressed) { sw_state = 0; break; }
      if (now - sw_ts >= ENC_SW_DEBOUNCE_MS) { sw_state = 2; sw_press_t = now; }
      break;

    case 2:   // held — wait for release or long-press timeout
      if (!pressed) { sw_state = 3; sw_ts = now; break; }
      if (now - sw_press_t >= ENC_SW_LONG_MS) { sw_state = 4; return ENC_LONG_PRESS; }
      break;

    case 3:   // release debounce
      if (pressed) { sw_state = 2; break; }
      if (now - sw_ts >= ENC_SW_DEBOUNCE_MS) { sw_state = 0; return ENC_PRESS; }
      break;

    case 4:   // long-press already emitted — wait for physical release
      if (!pressed) sw_state = 0;
      break;
  }

  return ENC_NONE;
}
