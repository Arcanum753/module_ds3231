#include "core_web/FSWebServerLib.h"

#include <Arduino.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "module_ds3231.h"
#include "common_module.h"

#include "core_state/core_state.h"
#include "common/TimeLib.h"

#include "core_terminal/core_terminal.h"
#include "core_terminal/common_module.h"

// Строковые литералы терминала: на ESP8266 держим их в flash, иначе
// команда не влезает в DRAM (базовый env занимает ~96% RAM).
#if defined(ESP8266)
#define DS_LOG(fmt, ...) Serial.printf_P(PSTR(fmt), ##__VA_ARGS__)
#define DS_LOG_LN(msg)   Serial.println(F(msg))
#else
#define DS_LOG(fmt, ...) Serial.printf(fmt, ##__VA_ARGS__)
#define DS_LOG_LN(msg)   Serial.println(msg)
#endif

// ============================================================
// Терминальные команды module_ds3231
//
// Одна команда верхнего уровня ds3231, старые имена ds-alarm/ds-sqw/ds-sqr
// сохранены скрытыми алиасами. Команды только применяют изменения к RTC,
// GPIO и памяти модуля; на FS пишет единственная подкоманда save.
// ============================================================

static const char* const kDs3231Commands[] = {
    "show", "time", "set", "temp", "alarm1", "alarm2", "alarms",
    "sqw", "sqr", "autocor", "save", nullptr
};

static const char kDs3231Help[] PROGMEM =
    "ds3231 - DS3231 RTC (changes are not saved automatically)\r\n"
    "  ds3231 show                      connection, RTC/system time, config\r\n"
    "  ds3231 time                      read RTC time\r\n"
    "  ds3231 set <epoch|YYYY-MM-DD HH:MM:SS>\r\n"
    "                                   set RTC only (system time unchanged)\r\n"
    "  ds3231 temp                      read temperature\r\n"
    "  ds3231 alarm1                    show Alarm 1\r\n"
    "  ds3231 alarm1 <hh:mm:ss> <mode 0..4> <n> [dow]\r\n"
    "                                   set Alarm 1; n = date 1..31 or dow 1..7\r\n"
    "  ds3231 alarm2                    show Alarm 2\r\n"
    "  ds3231 alarm2 <hh:mm> <mode 0..3> <n> [dow]\r\n"
    "                                   set Alarm 2; n = date 1..31 or dow 1..7\r\n"
    "  ds3231 alarms                    A1F/A2F flags and last fired\r\n"
    "  ds3231 sqw [on|off|poll|int]     SQW GPIO monitoring (ESP32 only)\r\n"
    "  ds3231 sqr [intcn|bbsqw [<0..3>]|rs <0..3>]\r\n"
    "                                   SQW output mode, Control 0x0E (ESP32 only)\r\n"
    "  ds3231 autocor on|off            auto-correct invalid RTC at boot\r\n"
    "  ds3231 save                      persist /config_ds3231.json\r\n"
    "  ds3231 help                      this help\r\n"
    "\r\n"
    "Alarm 1 modes: 0=every sec, 1=match sec, 2=match min:sec,\r\n"
    "               3=match hour:min:sec (daily), 4=day/date + time\r\n"
    "Alarm 2 modes: 0=every min, 1=match min, 2=match hour:min,\r\n"
    "               3=day/date + time\r\n";

static const char* onOffText(bool on) {
    return on ? "on" : "off";
}

#if defined(ESP32)
// Изменения SQW/Control применяются к конфигу и без RTC; предупредить,
// что железо при этом не тронуто.
static void noteConfigOnly() {
    if (!module_ds3231.isConnected()) {
        DS_LOG_LN("DS3231 not connected; config updated only.");
    }
}
#endif

#if defined(ESP32)
static const char* sqwModeText(uint8_t mode) {
    return (mode == DS3231_SQW_MODE_INTERRUPT) ? "interrupt" : "polling";
}
#endif

// Строгий разбор YYYY-MM-DD HH:MM:SS: вся строка должна быть потреблена.
static bool parseDateTime(const String& in, int& yr, int& mon, int& day,
                          int& hr, int& min, int& sec) {
    int pos = 0;
    int n = sscanf(in.c_str(), "%d-%d-%d %d:%d:%d%n",
                   &yr, &mon, &day, &hr, &min, &sec, &pos);
    if (n != 6 || pos != (int)in.length()) { return false; }
    return yr >= 2000 && yr <= 2099 && mon >= 1 && mon <= 12 &&
           day >= 1 && day <= 31 && hr >= 0 && hr <= 23 &&
           min >= 0 && min <= 59 && sec >= 0 && sec <= 59;
}

// Строгий разбор hh:mm[:ss]: вся строка должна быть потреблена.
static bool parseClock(const String& in, bool withSeconds,
                       uint8_t& hr, uint8_t& min, uint8_t& sec) {
    int h = 0, m = 0, s = 0;
    int pos = 0;
    int n = 0;
    if (withSeconds) {
        n = sscanf(in.c_str(), "%d:%d:%d%n", &h, &m, &s, &pos);
        if (n != 3) { return false; }
    } else {
        n = sscanf(in.c_str(), "%d:%d%n", &h, &m, &pos);
        if (n != 2) { return false; }
        s = 0;
    }
    if (pos != (int)in.length()) { return false; }
    if (h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59) { return false; }
    hr  = (uint8_t)h;
    min = (uint8_t)m;
    sec = (uint8_t)s;
    return true;
}

void CLASS_MODULE_DS3231::terminalRegister() {
    TerminalRegisterModule("module_ds3231", "ds3231", "DS3231 RTC",
                           &CLASS_MODULE_DS3231::s_terminalCmd,
                           kDs3231Commands, kDs3231Help, true);

    // Старые имена — скрытыми алиасами, слоты не занимают.
    TerminalRegisterAlias("ds-alarm", &CLASS_MODULE_DS3231::s_cmdAlarms);
    TerminalRegisterAlias("ds-sqw",   &CLASS_MODULE_DS3231::s_cmdSqw);
    TerminalRegisterAlias("ds-sqr",   &CLASS_MODULE_DS3231::s_cmdSqr);
}

void CLASS_MODULE_DS3231::s_terminalCmd() {
    char* sub = term.getNext();
    if (sub == nullptr || strcmp(sub, "help") == 0) {
        TerminalPrintText(kDs3231Help);
        return;
    }

    if (strcmp(sub, "show") == 0)   { s_cmdShow();   return; }
    if (strcmp(sub, "time") == 0)   { s_cmdTime();   return; }
    if (strcmp(sub, "set") == 0)    { s_cmdSet();    return; }
    if (strcmp(sub, "temp") == 0)   { s_cmdTemp();   return; }
    if (strcmp(sub, "alarm1") == 0) { s_cmdAlarm1(); return; }
    if (strcmp(sub, "alarm2") == 0) { s_cmdAlarm2(); return; }
    if (strcmp(sub, "alarms") == 0) { s_cmdAlarms(); return; }
    if (strcmp(sub, "sqw") == 0)    { s_cmdSqw();    return; }
    if (strcmp(sub, "sqr") == 0)    { s_cmdSqr();    return; }
    if (strcmp(sub, "autocor") == 0) { s_cmdAutocor(); return; }
    if (strcmp(sub, "save") == 0)   { s_cmdSave();   return; }

    DS_LOG("Unknown ds3231 subcommand '%s'. Type 'ds3231 help'.\r\n", sub);
}

// ============================================================
// show
// ============================================================

void CLASS_MODULE_DS3231::s_cmdShow() {
    if (term.getNext() != nullptr) {
        DS_LOG_LN("Usage: ds3231 show");
        return;
    }

    uint8_t addr = module_ds3231._config.addr;
    bool comm = false;
    uint8_t stat = 0;

    if (addr == DS3231_ADDR_NONE) {
        DS_LOG_LN("Connected: no (none)");
    } else {
        stat = module_ds3231.getStatusReg();
        comm = (module_ds3231.getLastError() == 0);
        DS_LOG("Connected: %s (0x%02X)\r\n",
                      comm ? "yes" : "no response", addr);
    }

    if (comm) {
        time_t rtc = module_ds3231.getTime();
        String rtcStr;
        ns_module_ds3231::_formatTime(rtc, rtcStr);
        DS_LOG("RTC time:  %s%s\r\n", rtcStr.c_str(),
                      (rtc == 0) ? " (invalid or OSF)" : "");

        String sysStr = core_state.getStr("time.now_str", "");
        String src = core_state.getStr("time.source", "");
        bool valid = core_state.getBool("time.valid", false);
        bool fallback = core_state.getBool("time.fallback", true);
        DS_LOG("Sys time:  %s (%s, %s)\r\n",
                      (sysStr.length() > 0) ? sysStr.c_str() : "--",
                      (src.length() > 0) ? src.c_str() : "internal",
                      valid ? "valid" : (fallback ? "fallback" : "invalid"));

        float temp = 0.0f;
        if (module_ds3231.getTemperature(temp)) {
            DS_LOG("Temperature: %.2f C\r\n", temp);
        } else {
            DS_LOG_LN("Temperature: unavailable");
        }

        uint8_t ctrl = module_ds3231._readReg(0x0E);
        stat = module_ds3231._readReg(0x0F);
        if (module_ds3231.getLastError() == 0) {
            DS_LOG("Control: 0x%02X, Status: 0x%02X"
                          " (OSF=%d BSY=%d A1F=%d A2F=%d)\r\n",
                          ctrl, stat,
                          (stat & 0x80) ? 1 : 0,
                          (stat & 0x04) ? 1 : 0,
                          (stat & 0x01) ? 1 : 0,
                          (stat & 0x02) ? 1 : 0);
        } else {
            DS_LOG("Registers: read failed (%d)\r\n",
                          module_ds3231.getLastError());
        }

        uint8_t h = 0, m = 0, s = 0, mode = 0, dayOrDate = 0;
        bool dow = false;
        if (module_ds3231.getAlarm1(h, m, s, mode, dayOrDate, dow)) {
            DS_LOG("Alarm1: %02u:%02u:%02u mode=%u %s=%u\r\n",
                          (unsigned)h, (unsigned)m, (unsigned)s,
                          (unsigned)mode, dow ? "dow" : "date",
                          (unsigned)dayOrDate);
        } else {
            DS_LOG_LN("Alarm1: read failed");
        }
        if (module_ds3231.getAlarm2(h, m, mode, dayOrDate, dow)) {
            DS_LOG("Alarm2: %02u:%02u mode=%u %s=%u\r\n",
                          (unsigned)h, (unsigned)m, (unsigned)mode,
                          dow ? "dow" : "date", (unsigned)dayOrDate);
        } else {
            DS_LOG_LN("Alarm2: read failed");
        }

#if defined(ESP32)
        DS_LOG("SQW:      level=%s pin=%d raw=%d\r\n",
               module_ds3231._config.sqwLevelActive ? "high" : "low",
               DS3231_SQW_PIN,
               digitalRead(DS3231_SQW_PIN));
#endif
    }

#if defined(ESP32)
    DS_LOG("SQW cfg:  enabled=%s mode=%s\r\n",
           onOffText(module_ds3231._config.sqwEnabled),
           sqwModeText(module_ds3231._config.sqwMode));
#endif
    DS_LOG("Config:   %s; autocor=%s, autoPoll=%s/%us, addr=0x%02X\r\n",
                  module_ds3231._configDirty ? "modified (use 'ds3231 save')" : "saved",
                  onOffText(module_ds3231._config.autoCorrectOnStart),
                  onOffText(module_ds3231._config.autoPoll),
                  (unsigned)module_ds3231._config.pollInterval,
                  addr);
}

// ============================================================
// time
// ============================================================

void CLASS_MODULE_DS3231::s_cmdTime() {
    if (term.getNext() != nullptr) {
        DS_LOG_LN("Usage: ds3231 time");
        return;
    }
    if (!module_ds3231.isConnected()) {
        DS_LOG_LN("DS3231 not connected");
        return;
    }

    time_t t = module_ds3231.getTime();
    String out;
    ns_module_ds3231::_formatTime(t, out);
    DS_LOG("RTC time: %s%s\r\n", out.c_str(),
                  (t == 0) ? " (invalid or OSF)" : "");
}

// ============================================================
// set — только RTC, системное время не меняется
// ============================================================

void CLASS_MODULE_DS3231::s_cmdSet() {
    String value = ns_core_terminal::trimCopy(TerminalGetArgLine());
    if (value.length() == 0) {
        DS_LOG_LN("Usage: ds3231 set <epoch|YYYY-MM-DD HH:MM:SS>");
        return;
    }
    if (!module_ds3231.isConnected()) {
        DS_LOG_LN("DS3231 not connected");
        return;
    }

    bool ok = false;
    time_t tSet = 0;
    bool dateForm = (value.indexOf('-') >= 0) || (value.indexOf(':') >= 0);
    if (dateForm) {
        int yr = 0, mon = 0, day = 0, hr = 0, min = 0, sec = 0;
        if (!parseDateTime(value, yr, mon, day, hr, min, sec)) {
            DS_LOG_LN("Invalid date/time. Use YYYY-MM-DD HH:MM:SS.");
            return;
        }
        tmElements_t tm;
        tm.Year   = (uint8_t)(yr - 1970);
        tm.Month  = (uint8_t)mon;
        tm.Day    = (uint8_t)day;
        tm.Hour   = (uint8_t)hr;
        tm.Minute = (uint8_t)min;
        tm.Second = (uint8_t)sec;
        tSet = makeTime(tm);
        ok = module_ds3231.setTime(yr, mon, day, hr, min, sec);
    } else {
        char* end = nullptr;
        long long epoch = strtoll(value.c_str(), &end, 10);
        if (end == value.c_str() || *end != '\0' || epoch <= 0) {
            DS_LOG_LN("Usage: ds3231 set <epoch|YYYY-MM-DD HH:MM:SS>");
            return;
        }
        time_t t = (time_t)epoch;
        if ((long long)t != epoch) {
            DS_LOG_LN("Epoch out of range.");
            return;
        }
        tmElements_t tm;
        breakTime(t, tm);
        tSet = t;
        ok = module_ds3231.setTime((int)tm.Year + 1970, tm.Month, tm.Day,
                                   tm.Hour, tm.Minute, tm.Second);
    }

    if (!ok) {
        DS_LOG("RTC write failed (error %d).\r\n", module_ds3231.getLastError());
        return;
    }

    String out;
    ns_module_ds3231::_formatTime(tSet, out);
    DS_LOG("RTC time set to %s.\r\n", out.c_str());
    DS_LOG_LN("System time is unchanged; core_sys may adopt or overwrite the RTC.");
}

// ============================================================
// temp
// ============================================================

void CLASS_MODULE_DS3231::s_cmdTemp() {
    if (term.getNext() != nullptr) {
        DS_LOG_LN("Usage: ds3231 temp");
        return;
    }
    if (!module_ds3231.isConnected()) {
        DS_LOG_LN("DS3231 not connected");
        return;
    }

    float temp = 0.0f;
    if (module_ds3231.getTemperature(temp)) {
        DS_LOG("Temperature: %.2f C\r\n", temp);
    } else {
        DS_LOG("Temperature read failed (error %d).\r\n",
                      module_ds3231.getLastError());
    }
}

// ============================================================
// alarm1 / alarm2
// ============================================================

void CLASS_MODULE_DS3231::s_cmdAlarm1() {
    char* timeArg = term.getNext();
    if (timeArg == nullptr) {
        if (!module_ds3231.isConnected()) {
            DS_LOG_LN("DS3231 not connected");
            return;
        }
        uint8_t h = 0, m = 0, s = 0, mode = 0, dayOrDate = 0;
        bool dow = false;
        if (!module_ds3231.getAlarm1(h, m, s, mode, dayOrDate, dow)) {
            DS_LOG("Alarm1 read failed (error %d).\r\n",
                          module_ds3231.getLastError());
            return;
        }
        DS_LOG("Alarm1: %02u:%02u:%02u mode=%u %s=%u\r\n",
                      (unsigned)h, (unsigned)m, (unsigned)s,
                      (unsigned)mode, dow ? "dow" : "date",
                      (unsigned)dayOrDate);
        return;
    }

    char* modeArg = term.getNext();
    char* dayArg = term.getNext();
    char* dowArg = term.getNext();
    if (modeArg == nullptr || dayArg == nullptr || term.getNext() != nullptr) {
        DS_LOG_LN("Usage: ds3231 alarm1 <hh:mm:ss> <mode 0..4> <n> [dow]");
        return;
    }
    if (!module_ds3231.isConnected()) {
        DS_LOG_LN("DS3231 not connected");
        return;
    }

    uint8_t h = 0, m = 0, s = 0;
    if (!parseClock(String(timeArg), true, h, m, s)) {
        DS_LOG_LN("Invalid time. Use hh:mm:ss.");
        return;
    }

    long mode = 0;
    if (!ns_core_terminal::parseIntStrict(String(modeArg), mode, 0, 4)) {
        DS_LOG_LN("Alarm1 mode: 0..4.");
        return;
    }

    bool isDow = false;
    long day = 0;
    if (dowArg != nullptr) {
        if (strcmp(dowArg, "dow") != 0) {
            DS_LOG_LN("Usage: ds3231 alarm1 <hh:mm:ss> <mode 0..4> <n> [dow]");
            return;
        }
        isDow = true;
        if (!ns_core_terminal::parseIntStrict(String(dayArg), day, 1, 7)) {
            DS_LOG_LN("Day of week: 1..7 (1 = Sunday).");
            return;
        }
    } else {
        if (!ns_core_terminal::parseIntStrict(String(dayArg), day, 1, 31)) {
            DS_LOG_LN("Date: 1..31.");
            return;
        }
    }

    if (!module_ds3231.setAlarm1(h, m, s, (uint8_t)mode, (uint8_t)day, isDow)) {
        DS_LOG("Alarm1 write failed (error %d).\r\n",
                      module_ds3231.getLastError());
        return;
    }

    DS_LOG("Alarm1 set: %02u:%02u:%02u mode=%ld %s=%ld (not saved; RTC register).\r\n",
                  (unsigned)h, (unsigned)m, (unsigned)s, mode,
                  isDow ? "dow" : "date", day);
}

void CLASS_MODULE_DS3231::s_cmdAlarm2() {
    char* timeArg = term.getNext();
    if (timeArg == nullptr) {
        if (!module_ds3231.isConnected()) {
            DS_LOG_LN("DS3231 not connected");
            return;
        }
        uint8_t h = 0, m = 0, mode = 0, dayOrDate = 0;
        bool dow = false;
        if (!module_ds3231.getAlarm2(h, m, mode, dayOrDate, dow)) {
            DS_LOG("Alarm2 read failed (error %d).\r\n",
                          module_ds3231.getLastError());
            return;
        }
        DS_LOG("Alarm2: %02u:%02u mode=%u %s=%u\r\n",
                      (unsigned)h, (unsigned)m, (unsigned)mode,
                      dow ? "dow" : "date", (unsigned)dayOrDate);
        return;
    }

    char* modeArg = term.getNext();
    char* dayArg = term.getNext();
    char* dowArg = term.getNext();
    if (modeArg == nullptr || dayArg == nullptr || term.getNext() != nullptr) {
        DS_LOG_LN("Usage: ds3231 alarm2 <hh:mm> <mode 0..3> <n> [dow]");
        return;
    }
    if (!module_ds3231.isConnected()) {
        DS_LOG_LN("DS3231 not connected");
        return;
    }

    uint8_t h = 0, m = 0, s = 0;
    if (!parseClock(String(timeArg), false, h, m, s)) {
        DS_LOG_LN("Invalid time. Use hh:mm.");
        return;
    }

    long mode = 0;
    if (!ns_core_terminal::parseIntStrict(String(modeArg), mode, 0, 3)) {
        DS_LOG_LN("Alarm2 mode: 0..3.");
        return;
    }

    bool isDow = false;
    long day = 0;
    if (dowArg != nullptr) {
        if (strcmp(dowArg, "dow") != 0) {
            DS_LOG_LN("Usage: ds3231 alarm2 <hh:mm> <mode 0..3> <n> [dow]");
            return;
        }
        isDow = true;
        if (!ns_core_terminal::parseIntStrict(String(dayArg), day, 1, 7)) {
            DS_LOG_LN("Day of week: 1..7 (1 = Sunday).");
            return;
        }
    } else {
        if (!ns_core_terminal::parseIntStrict(String(dayArg), day, 1, 31)) {
            DS_LOG_LN("Date: 1..31.");
            return;
        }
    }

    if (!module_ds3231.setAlarm2(h, m, (uint8_t)mode, (uint8_t)day, isDow)) {
        DS_LOG("Alarm2 write failed (error %d).\r\n",
                      module_ds3231.getLastError());
        return;
    }

    DS_LOG("Alarm2 set: %02u:%02u mode=%ld %s=%ld (not saved; RTC register).\r\n",
                  (unsigned)h, (unsigned)m, mode,
                  isDow ? "dow" : "date", day);
}

// ============================================================
// alarms — флаги и время последнего срабатывания
// ============================================================

void CLASS_MODULE_DS3231::s_cmdAlarms() {
    if (term.getNext() != nullptr) {
        DS_LOG_LN("Usage: ds3231 alarms");
        return;
    }
    if (!module_ds3231.isConnected()) {
        DS_LOG_LN("DS3231 not connected");
        return;
    }

    uint8_t stat = module_ds3231.getStatusReg();
    if (module_ds3231.getLastError() != 0) {
        DS_LOG("Status read failed (error %d).\r\n",
                      module_ds3231.getLastError());
        return;
    }

    DS_LOG("A1F=%d A2F=%d\r\n",
                  (stat & 0x01) ? 1 : 0, (stat & 0x02) ? 1 : 0);

#if defined(ESP32)
    time_t t1 = module_ds3231.getLastAlarm1Time();
    time_t t2 = module_ds3231.getLastAlarm2Time();
    if (t1 == 0 && t2 == 0) {
        DS_LOG_LN("No alarm fired since boot.");
        return;
    }
    String s;
    if (t1 != 0) {
        ns_module_ds3231::_formatAlarmStamp(t1, s);
        DS_LOG("Alarm 1 last fired: %s\r\n", s.c_str());
    }
    if (t2 != 0) {
        ns_module_ds3231::_formatAlarmStamp(t2, s);
        DS_LOG("Alarm 2 last fired: %s\r\n", s.c_str());
    }
#endif
}

// ============================================================
// sqw — мониторинг GPIO SQW/INT# (только ESP32)
// ============================================================

void CLASS_MODULE_DS3231::s_cmdSqw() {
#if defined(ESP8266)
    DS_LOG_LN("SQW/INT# control is available on ESP32 only.");
#endif
#if defined(ESP32)
    char* arg = term.getNext();
    if (arg != nullptr && term.getNext() != nullptr) {
        DS_LOG_LN("Usage: ds3231 sqw [on|off|poll|int]");
        return;
    }

    if (arg == nullptr) {
        DS_LOG("SQW: %s mode=%s level=%s pin=%d raw=%d\r\n",
                      onOffText(module_ds3231._config.sqwEnabled),
                      sqwModeText(module_ds3231._config.sqwMode),
                      module_ds3231._config.sqwLevelActive ? "high" : "low",
                      DS3231_SQW_PIN,
                      digitalRead(DS3231_SQW_PIN));
        return;
    }

    if (strcmp(arg, "on") == 0 || strcmp(arg, "1") == 0) {
        module_ds3231._config.sqwEnabled = true;
        module_ds3231._configDirty = true;
        module_ds3231.sqwGpioReinit();
        noteConfigOnly();
        DS_LOG_LN("SQW monitoring ON (not saved).");
        return;
    }
    if (strcmp(arg, "off") == 0 || strcmp(arg, "0") == 0) {
        module_ds3231._config.sqwEnabled = false;
        module_ds3231._configDirty = true;
        module_ds3231.sqwGpioReinit();
        noteConfigOnly();
        DS_LOG_LN("SQW monitoring OFF (not saved).");
        return;
    }
    if (strcmp(arg, "poll") == 0) {
        module_ds3231._config.sqwMode = DS3231_SQW_MODE_POLLING;
        module_ds3231._configDirty = true;
        module_ds3231.sqwGpioReinit();
        noteConfigOnly();
        DS_LOG_LN("SQW mode: polling (not saved).");
        return;
    }
    if (strcmp(arg, "int") == 0) {
        module_ds3231._config.sqwMode = DS3231_SQW_MODE_INTERRUPT;
        module_ds3231._configDirty = true;
        module_ds3231.sqwGpioReinit();
        noteConfigOnly();
        DS_LOG_LN("SQW mode: interrupt (not saved).");
        return;
    }

    DS_LOG("Unknown sqw argument '%s'. Use on|off|poll|int.\r\n", arg);
#endif
}

// ============================================================
// sqr — режим выхода SQW (Control 0x0E, только ESP32)
// ============================================================

void CLASS_MODULE_DS3231::s_cmdSqr() {
#if defined(ESP8266)
    DS_LOG_LN("SQW/INT# control is available on ESP32 only.");
#endif
#if defined(ESP32)
    char* arg = term.getNext();
    if (arg == nullptr) {
        if (term.getNext() != nullptr) {
            DS_LOG_LN("Usage: ds3231 sqr [intcn|bbsqw [<0..3>]|rs <0..3>]");
            return;
        }
        if (!module_ds3231.isConnected()) {
            DS_LOG_LN("DS3231 not connected");
            return;
        }
        uint8_t ctrl = module_ds3231._readReg(0x0E);
        if (module_ds3231.getLastError() != 0) {
            DS_LOG("Control read failed (error %d).\r\n",
                          module_ds3231.getLastError());
            return;
        }
        uint8_t cfg = 0x00;
        if (module_ds3231._config.ctrlBbsqw) { cfg |= 0x40; }
        cfg |= (uint8_t)((module_ds3231._config.ctrlRs & 0x03) << 3);
        if (module_ds3231._config.ctrlIntcn) { cfg |= 0x04; }
        if (module_ds3231._config.ctrlA2ie)  { cfg |= 0x02; }
        if (module_ds3231._config.ctrlA1ie)  { cfg |= 0x01; }

        DS_LOG("Control=0x%02X BBSQW=%d RS=%d INTCN=%d A1IE=%d A2IE=%d\r\n",
                      ctrl,
                      (ctrl & 0x40) ? 1 : 0,
                      (ctrl >> 3) & 3,
                      (ctrl & 0x04) ? 1 : 0,
                      (ctrl & 0x01) ? 1 : 0,
                      (ctrl & 0x02) ? 1 : 0);
        DS_LOG("Config=0x%02X  BBSQW=%d RS=%d INTCN=%d A1IE=%d A2IE=%d%s\r\n",
                      cfg,
                      module_ds3231._config.ctrlBbsqw ? 1 : 0,
                      module_ds3231._config.ctrlRs,
                      module_ds3231._config.ctrlIntcn ? 1 : 0,
                      module_ds3231._config.ctrlA1ie ? 1 : 0,
                      module_ds3231._config.ctrlA2ie ? 1 : 0,
                      (cfg == ctrl) ? "" : " (differs from RTC)");
        return;
    }

    if (strcmp(arg, "intcn") == 0) {
        if (term.getNext() != nullptr) {
            DS_LOG_LN("Usage: ds3231 sqr intcn");
            return;
        }
        module_ds3231._config.ctrlIntcn = true;
        module_ds3231._config.ctrlBbsqw = false;
        module_ds3231._configDirty = true;
        module_ds3231._applyCtrlBits();
        noteConfigOnly();
        DS_LOG_LN("SQW output: INTCN=1 (INT# on alarms; not saved).");
        return;
    }

    if (strcmp(arg, "bbsqw") == 0) {
        char* rsArg = term.getNext();
        if (term.getNext() != nullptr) {
            DS_LOG_LN("Usage: ds3231 sqr bbsqw [<0..3>]");
            return;
        }
        long rs = module_ds3231._config.ctrlRs;
        if (rsArg != nullptr &&
            !ns_core_terminal::parseIntStrict(String(rsArg), rs, 0, 3)) {
            DS_LOG_LN("RS: 0..3 (1 Hz, 1.024 kHz, 4.096 kHz, 8.192 kHz).");
            return;
        }
        module_ds3231._config.ctrlBbsqw = true;
        module_ds3231._config.ctrlIntcn = false;
        module_ds3231._config.ctrlRs = (uint8_t)rs;
        module_ds3231._configDirty = true;
        module_ds3231._applyCtrlBits();
        noteConfigOnly();
        DS_LOG("SQW output: BBSQW=1, RS=%ld (square wave; not saved).\r\n", rs);
        return;
    }

    if (strcmp(arg, "rs") == 0) {
        char* rsArg = term.getNext();
        long rs = 0;
        if (rsArg == nullptr || term.getNext() != nullptr ||
            !ns_core_terminal::parseIntStrict(String(rsArg), rs, 0, 3)) {
            DS_LOG_LN("Usage: ds3231 sqr rs <0..3>");
            return;
        }
        module_ds3231._config.ctrlRs = (uint8_t)rs;
        module_ds3231._configDirty = true;
        module_ds3231._applyCtrlBits();
        noteConfigOnly();
        DS_LOG("SQW RS=%ld (not saved).\r\n", rs);
        return;
    }

    DS_LOG("Unknown sqr argument '%s'. Use intcn|bbsqw|rs.\r\n", arg);
#endif
}

// ============================================================
// autocor — автокоррекция времени RTC при старте
// ============================================================

void CLASS_MODULE_DS3231::s_cmdAutocor() {
    char* arg = term.getNext();
    bool on = false;
    if (arg == nullptr || term.getNext() != nullptr ||
        !ns_core_terminal::parseOnOff(String(arg), on)) {
        DS_LOG_LN("Usage: ds3231 autocor on|off");
        return;
    }

    module_ds3231._config.autoCorrectOnStart = on;
    module_ds3231._configDirty = true;
    DS_LOG("Auto-correct on boot: %s (applies at next boot; not saved).\r\n",
                  onOffText(on));
}

// ============================================================
// save — единственная подкоманда, пишущая конфиг на FS
// ============================================================

void CLASS_MODULE_DS3231::s_cmdSave() {
    if (term.getNext() != nullptr) {
        DS_LOG_LN("Usage: ds3231 save");
        return;
    }

    bool ok = module_ds3231.saveConfig();
    Serial.println(ok ? "Saved." : "Save failed.");
}
