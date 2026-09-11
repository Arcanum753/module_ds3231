
#ifndef _MODULE_DS3231_COMMON_MODULE_h
#define _MODULE_DS3231_COMMON_MODULE_h

#include <stdint.h>
#include <Arduino.h>

// Вспомогательные функции модуля DS3231 (чистые, без состояния).
namespace ns_module_ds3231 {

// BCD <-> DEC
uint8_t _dec2bcd(uint8_t dec);
uint8_t _bcd2dec(uint8_t bcd);

// Будильники — декодирование/кодирование режимов
// Alarm 1: 4 байта с 0x07 (sec, min, hour, day/date)
// Alarm 2: 3 байта с 0x0B (min, hour, day/date)
// mode: 0=once, 1=match_sec/min, 2=match_min_sec/min_min, 3=match_hr_min_sec/hr_min, 4=match_day_date
void _decodeAlarm1Mode(uint8_t *buf, uint8_t &mode, uint8_t &dayOrDate, bool &isDayOfWeek);
void _encodeAlarm1Mode(uint8_t *buf, uint8_t sec, uint8_t min, uint8_t hour, uint8_t mode, uint8_t dayOrDate, bool isDayOfWeek);
void _decodeAlarm2Mode(uint8_t *buf, uint8_t &mode, uint8_t &dayOrDate, bool &isDayOfWeek);
void _encodeAlarm2Mode(uint8_t *buf, uint8_t min, uint8_t hour, uint8_t mode, uint8_t dayOrDate, bool isDayOfWeek);

#if defined(ESP32)
// Форматирование времени срабатывания будильников
void _formatAlarmTime(time_t t, String &out);
void _formatAlarmStamp(time_t t, String &out);
#endif

} // namespace ns_module_ds3231

#endif // _MODULE_DS3231_COMMON_MODULE_h
