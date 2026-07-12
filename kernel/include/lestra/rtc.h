#ifndef LESTRA_RTC_H
#define LESTRA_RTC_H
#include <lestra/types.h>
void rtc_init(void);
void rtc_get_time(uint8_t* hour, uint8_t* min, uint8_t* sec);
void rtc_get_date(uint16_t* year, uint8_t* month, uint8_t* day);
uint64_t rtc_get_unix_time(void);
#endif
