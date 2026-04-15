#pragma once
#include "state.h"

void rtc_init();
void rtc_update(SensorData &data);

// Writes date and time directly to DS3231M registers over I2C (BCD encoding)
void rtc_set_datetime(uint16_t year, uint8_t month, uint8_t day,
                      uint8_t hour, uint8_t minute, uint8_t second);
