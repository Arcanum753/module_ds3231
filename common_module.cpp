
#include "common_module.h"

#include "common/TimeLib.h"

// ============================================================
// Вспомогательные функции модуля DS3231
// ============================================================

namespace ns_module_ds3231 {

// BCD — статические вспомогательные функции

uint8_t _dec2bcd(uint8_t dec) {
    return ((dec / 10) << 4) | (dec % 10);
}

uint8_t _bcd2dec(uint8_t bcd) {
    return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

// Будильники — декодирование/кодирование режимов

void _decodeAlarm1Mode(uint8_t *buf, uint8_t &mode, uint8_t &dayOrDate, bool &isDayOfWeek) {
    bool a1m1 = (buf[0] >> 7) & 1;
    bool a1m2 = (buf[1] >> 7) & 1;
    bool a1m3 = (buf[2] >> 7) & 1;
    bool a1m4 = (buf[3] >> 7) & 1;
    isDayOfWeek = (buf[3] >> 6) & 1;

    if (a1m1 && a1m2 && a1m3 && a1m4)              { mode = 0; }
    else if (!a1m1 && a1m2 && a1m3 && a1m4)        { mode = 1; }
    else if (!a1m1 && !a1m2 && a1m3 && a1m4)        { mode = 2; }
    else if (!a1m1 && !a1m2 && !a1m3 && a1m4)        { mode = 3; }
    else                                             { mode = 4; }

    dayOrDate = _bcd2dec(buf[3] & 0x3F);
}

void _encodeAlarm1Mode(uint8_t *buf, uint8_t sec, uint8_t min, uint8_t hour, uint8_t mode, uint8_t dayOrDate, bool isDayOfWeek) {
    buf[0] = _dec2bcd(sec);
    buf[1] = _dec2bcd(min);
    buf[2] = _dec2bcd(hour);
    buf[3] = _dec2bcd(dayOrDate);
    if (isDayOfWeek) { buf[3] |= 0x40; }

    switch (mode) {
        case 0: buf[0] |= 0x80; buf[1] |= 0x80; buf[2] |= 0x80; buf[3] |= 0x80; break;
        case 1:                     buf[1] |= 0x80; buf[2] |= 0x80; buf[3] |= 0x80; break;
        case 2:                                         buf[2] |= 0x80; buf[3] |= 0x80; break;
        case 3:                                                             buf[3] |= 0x80; break;
        case 4: break;
    }
}

void _decodeAlarm2Mode(uint8_t *buf, uint8_t &mode, uint8_t &dayOrDate, bool &isDayOfWeek) {
    bool a2m2 = (buf[0] >> 7) & 1;
    bool a2m3 = (buf[1] >> 7) & 1;
    bool a2m4 = (buf[2] >> 7) & 1;
    isDayOfWeek = (buf[2] >> 6) & 1;

    if (a2m2 && a2m3 && a2m4)              { mode = 0; }
    else if (!a2m2 && a2m3 && a2m4)         { mode = 1; }
    else if (!a2m2 && !a2m3 && a2m4)         { mode = 2; }
    else                                      { mode = 3; }

    dayOrDate = _bcd2dec(buf[2] & 0x3F);
}

void _encodeAlarm2Mode(uint8_t *buf, uint8_t min, uint8_t hour, uint8_t mode, uint8_t dayOrDate, bool isDayOfWeek) {
    buf[0] = _dec2bcd(min);
    buf[1] = _dec2bcd(hour);
    buf[2] = _dec2bcd(dayOrDate);
    if (isDayOfWeek) { buf[2] |= 0x40; }

    switch (mode) {
        case 0: buf[0] |= 0x80; buf[1] |= 0x80; buf[2] |= 0x80; break;
        case 1:                     buf[1] |= 0x80; buf[2] |= 0x80; break;
        case 2:                                         buf[2] |= 0x80; break;
        case 3: break;
    }
}

#if defined(ESP32)
// Форматирование времени срабатывания: YYYY-MM-DD HH:MM:SS. Если время
// недостоверно (t==0, например OSF), возвращаем пометку "time invalid".
void _formatAlarmTime(time_t t, String &out) {
    if (t == 0) { out = "(time invalid)"; return; }
    String dt = "";
    dt += String(year(t)) + "-";
    if (month(t) < 10) { dt += "0"; }
    dt += String(month(t)) + "-";
    if (day(t) < 10) { dt += "0"; }
    dt += String(day(t)) + " ";
    if (hour(t) < 10) { dt += "0"; }
    dt += String(hour(t)) + ":";
    if (minute(t) < 10) { dt += "0"; }
    dt += String(minute(t)) + ":";
    if (second(t) < 10) { dt += "0"; }
    dt += String(second(t));
    out = dt;
}

// Форматирование штампа времени срабатывания для отображения.
// Если времени нет (t==0 — не срабатывал), возвращаем "--".
void _formatAlarmStamp(time_t t, String &out) {
    if (t == 0) { out = "--"; return; }
    _formatAlarmTime(t, out);
}
#endif

} // namespace ns_module_ds3231
