#include "core_web/FSWebServerLib.h"

#include "module_ds3231.h"
#include "common_module.h"
#include "module_ds3231_version.h"

#include "core_json/core_json.h"
#include "core_ntp/NtpClientLib.h"
#include "core_sys/core_sys.h"
#include "core_terminal/core_terminal.h"
#include "core_terminal/ErriezSerialTerminal.h"

#include <Wire.h>
#include "common/TimeLib.h"

// Терминальные команды модуля (реализация — в module_ds3231_engine.cpp)
void ds3231CmdAlarm();
void ds3231CmdSqw();
void ds3231CmdSqr();

CLASS_MODULE_DS3231 module_ds3231;
CLASS_MODULE_DS3231::CLASS_MODULE_DS3231() {
    _lastError = 0; _wireStarted = false;
#if defined(ESP32)
    _sqwIrqFlag = false;
    _sqwLastLowEdge = false;
    _sqwCareActive = false;
    _alarm1Fired = false;
    _alarm2Fired = false;
    _lastAlarm1At = 0;
    _lastAlarm2At = 0;
    _sqwInterrupting = false;
#endif
}

// ============================================================
// Time Source Provider API — колбэки для core_sys
// ============================================================

static bool ds3231GetTime(time_t& out) {
    if (!module_ds3231.isConnected()) { return false; }
    time_t t = (time_t)module_ds3231.getTime();   // одна I2C-операция
    if (t < CORE_SYS_TIME_MIN_VALID) { return false; }
    out = t;
    return true;
}

static bool ds3231SetTime(time_t in) {
    return module_ds3231.setTime((time_t)in);
}

static const char* ds3231Status() {
    if (!module_ds3231.isConnected()) { return "not connected"; }
    if (module_ds3231.getStatusReg() & 0x80) { return "osf (battery low?)"; }
    return "";
}

#if defined(ESP32)
void CLASS_MODULE_DS3231::setFs(fs::LittleFSFS* fs)
#elif defined(ESP8266)
void CLASS_MODULE_DS3231::setFs(FS* fs)
#endif
{
    _fs = fs;
}

// ============================================================
// begin()
// ============================================================

void CLASS_MODULE_DS3231::begin() {
    DEBUGDS3231("%s\r\n", __FUNCTION__);

    defaultConfig();
    if (loadConfig() == false) { saveConfig(); }

    if (!_wireStarted) { _wireStarted = true; Wire.begin(); }

    if (_config.addr != DS3231_ADDR_NONE) {
        bool found = false;
        for (uint8_t retry = 0; retry < DS3231_SCAN_RETRIES; retry++) {
            if (_detectDS3231(_config.addr)) { found = true; break; }
            delay(100);
        }
        if (!found) {
            DEBUGDS3231("DS3231:  0x%02X  didn't response. Rescan...\r\n", _config.addr);
            _config.addr = _scanForDS3231();
            saveConfig();
        }
    } else {
        _config.addr = _scanForDS3231();
        saveConfig();
    }

    if (_config.addr != DS3231_ADDR_NONE) {
        DEBUGDS3231("DS3231: inited, adr: 0x%02X\r\n", _config.addr);
    } else {
        DEBUGDS3231("DS3231: Not found!\r\n");
    }

    TerminalRegisterModule(ds3231TerminalRegister);

#if defined(ESP32)
    // Применяем биты Control (0x0E), отвечающие за режим выхода SQW
    _applyCtrlBits();
    // Инициализируем GPIO для мониторинга вывода SQW/INT#
    sqwGpioInit();
    // Считываем флаги будильников в начальное состояние
    checkAlarmFlags();
#endif
}

void CLASS_MODULE_DS3231::begin(ModContext& ctx) {
    _fs = ctx.fs;
    begin();
}

// ============================================================
// web_Init()
// ============================================================

void CLASS_MODULE_DS3231::web_Init() {
    DEBUGDS3231("%s\r\n", __FUNCTION__);

    ESPHTTPServer.on("/ds3231/read", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!ESPHTTPServer.checkAuth(request)) { return request->requestAuthentication(); }
        this->handleRead(request);
    });

    ESPHTTPServer.on("/ds3231/poll", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!ESPHTTPServer.checkAuth(request)) { return request->requestAuthentication(); }
        this->handlePoll(request);
    });

    ESPHTTPServer.on("/ds3231/set_time", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (!ESPHTTPServer.checkAuth(request)) { return request->requestAuthentication(); }
        this->handleSetTime(request);
    });

    ESPHTTPServer.on("/ds3231/set_alarm1", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (!ESPHTTPServer.checkAuth(request)) { return request->requestAuthentication(); }
        this->handleSetAlarm1(request);
    });

    ESPHTTPServer.on("/ds3231/set_alarm2", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (!ESPHTTPServer.checkAuth(request)) { return request->requestAuthentication(); }
        this->handleSetAlarm2(request);
    });

    ESPHTTPServer.on("/ds3231/set_reg", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (!ESPHTTPServer.checkAuth(request)) { return request->requestAuthentication(); }
        this->handleSetReg(request);
    });

    ESPHTTPServer.on("/ds3231/save", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (!ESPHTTPServer.checkAuth(request)) { return request->requestAuthentication(); }
        this->handleSaveConfig(request);
    });

    ESPHTTPServer.on("/ds3231/info", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!ESPHTTPServer.checkAuth(request)) { return request->requestAuthentication(); }
        this->handleInfo(request);
    });

    ESPHTTPServer.on("/ds3231/ver", HTTP_GET, [this](AsyncWebServerRequest *request) {
        this->html_ver_get(request);
    });
}

// ============================================================
// Источник времени для core_sys (Time Source Provider API)
// ============================================================

void CLASS_MODULE_DS3231::registerTimeSource() {
    core_sys.addTimeSource("ds3231", 50, ds3231GetTime, ds3231SetTime, ds3231Status);
}

// ============================================================
// Веб-обработчики
// ============================================================

void CLASS_MODULE_DS3231::handleRead(AsyncWebServerRequest *request) {
    DEBUGDS3231("%s\r\n", __FUNCTION__);
    String values = "";

    if (_config.addr == DS3231_ADDR_NONE) {
        values += "ds_state|disconnected|div\n";
        values += "ds_temp|--|div\n";
        values += "ds_ctrl_reg|00|div\n";
        values += "ds_stat_reg|00|div\n";
        values += "ds_aging_offset|0|input\n";
        values += "ds_alarm1_hour|0|input\n";
        values += "ds_alarm1_min|0|input\n";
        values += "ds_alarm1_sec|0|input\n";
        values += "ds_alarm1_mode|0|input\n";
        values += "ds_alarm1_dayOrDate|1|input\n";
        values += "ds_alarm1_isDayOfWeek||chk\n";
        values += "ds_alarm2_hour|0|input\n";
        values += "ds_alarm2_min|0|input\n";
        values += "ds_alarm2_mode|0|input\n";
        values += "ds_alarm2_dayOrDate|1|input\n";
        values += "ds_alarm2_isDayOfWeek||chk\n";
        char buf[16];
        snprintf(buf, sizeof(buf), "0x%02X", _config.addr);
        values += "ds_addr|" + String(buf) + "|div\n";
        request->send(200, "text/plain", values);
        return;
    }

    values += "ds_state|connected|div\n";
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "0x%02X", _config.addr);
        values += "ds_addr|" + String(buf) + "|div\n";
    }

    // Время
    {
        time_t t = _readTime();
        if (t > 0) {
            String dt = "";
            if (year(t) > 2000) {
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
            }
            values += "ds_time|" + dt + "|div\n";
        } else {
            values += "ds_time|Read error|div\n";
        }
    }

    // Температура
    {
        float temp;
        if (getTemperature(temp)) {
            char tbuf[16];
            snprintf(tbuf, sizeof(tbuf), "%.2f", temp);
            values += "ds_temp|" + String(tbuf) + "|div\n";
        } else {
            values += "ds_temp|--|div\n";
        }
    }

    // Регистры управления
    {
        uint8_t ctrl = _readReg(0x0E);
        uint8_t stat = _readReg(0x0F);
        uint8_t aging = _readReg(0x10);
        char rbuf[16];
        snprintf(rbuf, sizeof(rbuf), "0x%02X", ctrl);
        values += "ds_ctrl_reg|" + String(rbuf) + "|div\n";
        snprintf(rbuf, sizeof(rbuf), "0x%02X", stat);
        values += "ds_stat_reg|" + String(rbuf) + "|div\n";
        values += "ds_aging_offset|" + String(aging) + "|input\n";

        // Биты управления
        values += "ds_ctrl_eosc|"   + String((ctrl & 0x80) ? "1" : "0") + "|div\n";
        values += "ds_ctrl_bbsqw|"  + String((ctrl & 0x40) ? "1" : "0") + "|div\n";
        values += "ds_ctrl_conv|"   + String((ctrl & 0x20) ? "1" : "0") + "|div\n";
        values += "ds_ctrl_rs|"     + String((ctrl >> 3) & 3)           + "|div\n";
        values += "ds_ctrl_intcn|"  + String((ctrl & 0x04) ? "1" : "0") + "|div\n";
        values += "ds_ctrl_a2ie|"   + String((ctrl & 0x02) ? "1" : "0") + "|div\n";
        values += "ds_ctrl_a1ie|"   + String((ctrl & 0x01) ? "1" : "0") + "|div\n";

        // Биты статуса
        values += "ds_stat_osf|"    + String((stat & 0x80) ? "1" : "0") + "|div\n";
        values += "ds_stat_en32khz|"+ String((stat & 0x08) ? "1" : "0") + "|div\n";
        values += "ds_stat_bsy|"    + String((stat & 0x04) ? "1" : "0") + "|div\n";
        values += "ds_stat_a2f|"    + String((stat & 0x02) ? "1" : "0") + "|div\n";
        values += "ds_stat_a1f|"    + String((stat & 0x01) ? "1" : "0") + "|div\n";
    }

    // Будильники
    {
        uint8_t h, m, s, mode, dayOrDate;
        bool isDOW;
        if (getAlarm1(h, m, s, mode, dayOrDate, isDOW)) {
            values += "ds_alarm1_hour|"        + String(h)        + "|input\n";
            values += "ds_alarm1_min|"         + String(m)        + "|input\n";
            values += "ds_alarm1_sec|"         + String(s)        + "|input\n";
            values += "ds_alarm1_mode|"        + String(mode)     + "|select\n";
            values += "ds_alarm1_dayOrDate|"   + String(dayOrDate)+ "|input\n";
            values += "ds_alarm1_isDayOfWeek|" + String(isDOW ? "checked" : "") + "|chk\n";
        }
        if (getAlarm2(h, m, mode, dayOrDate, isDOW)) {
            values += "ds_alarm2_hour|"        + String(h)        + "|input\n";
            values += "ds_alarm2_min|"         + String(m)        + "|input\n";
            values += "ds_alarm2_mode|"        + String(mode)     + "|select\n";
            values += "ds_alarm2_dayOrDate|"   + String(dayOrDate)+ "|input\n";
            values += "ds_alarm2_isDayOfWeek|" + String(isDOW ? "checked" : "") + "|chk\n";
        }
    }

    // SQW / будильники — проверяем флаги и выводим поля конфигурации
#if defined(ESP32)
    checkAlarmFlags();
    emitSqwFields(values);
#endif

    request->send(200, "text/plain", values);
}

void CLASS_MODULE_DS3231::handlePoll(AsyncWebServerRequest *request) {
    DEBUGDS3231("%s\r\n", __FUNCTION__);
    String values = "";
    // Автоопрос (autoPoll/pollInterval) — клиентский: интервал задаёт JS на
    // веб-странице, сервер только отдаёт текущее состояние по запросу.
    // Поэтому поля конфига интервала здесь не применяются.

    if (_config.addr == DS3231_ADDR_NONE) {
        values += "ds_state|disconnected|div\n";
        request->send(200, "text/plain", values);
        return;
    }

    values += "ds_state|connected|div\n";
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "0x%02X", _config.addr);
        values += "ds_addr|" + String(buf) + "|div\n";
    }

    {
        time_t t = _readTime();
        if (t > 0) {
            String dt = "";
            if (year(t) > 2000) {
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
            }
            values += "ds_time|" + dt + "|div\n";
        } else {
            values += "ds_time|Read error|div\n";
        }
    }

    {
        float temp;
        if (getTemperature(temp)) {
            char tbuf[16];
            snprintf(tbuf, sizeof(tbuf), "%.2f", temp);
            values += "ds_temp|" + String(tbuf) + "|div\n";
        } else {
            values += "ds_temp|--|div\n";
        }
    }

    {
        uint8_t ctrl = _readReg(0x0E);
        uint8_t stat = _readReg(0x0F);
        char rbuf[16];
        snprintf(rbuf, sizeof(rbuf), "0x%02X", ctrl);
        values += "ds_ctrl_reg|" + String(rbuf) + "|div\n";
        snprintf(rbuf, sizeof(rbuf), "0x%02X", stat);
        values += "ds_stat_reg|" + String(rbuf) + "|div\n";

        values += "ds_ctrl_eosc|"   + String((ctrl & 0x80) ? "1" : "0") + "|div\n";
        values += "ds_ctrl_bbsqw|"  + String((ctrl & 0x40) ? "1" : "0") + "|div\n";
        values += "ds_ctrl_conv|"   + String((ctrl & 0x20) ? "1" : "0") + "|div\n";
        values += "ds_ctrl_rs|"     + String((ctrl >> 3) & 3)           + "|div\n";
        values += "ds_ctrl_intcn|"  + String((ctrl & 0x04) ? "1" : "0") + "|div\n";
        values += "ds_ctrl_a2ie|"   + String((ctrl & 0x02) ? "1" : "0") + "|div\n";
        values += "ds_ctrl_a1ie|"   + String((ctrl & 0x01) ? "1" : "0") + "|div\n";

        values += "ds_stat_osf|"    + String((stat & 0x80) ? "1" : "0") + "|div\n";
        values += "ds_stat_en32khz|"+ String((stat & 0x08) ? "1" : "0") + "|div\n";
        values += "ds_stat_bsy|"    + String((stat & 0x04) ? "1" : "0") + "|div\n";
        values += "ds_stat_a2f|"    + String((stat & 0x02) ? "1" : "0") + "|div\n";
        values += "ds_stat_a1f|"    + String((stat & 0x01) ? "1" : "0") + "|div\n";
    }

    // SQW / будильники — проверяем флаги и выводим поля конфигурации
#if defined(ESP32)
    checkAlarmFlags();
    emitSqwFields(values);
#endif

    request->send(200, "text/plain", values);
}

void CLASS_MODULE_DS3231::handleSetTime(AsyncWebServerRequest *request) {
    DEBUGDS3231("%s\r\n", __FUNCTION__);
    if (_config.addr == DS3231_ADDR_NONE) { request->send(200, "text/plain", "DS3231 not connected"); return; }

    if (request->hasArg("source") && request->arg("source") == "ntp") {
        if (NTP.getLastNTPSync() > 0) {
            time_t nowT = now();
            if (_writeTime(nowT)) {
                request->send(200, "text/plain", "OK");
            } else {
                request->send(200, "text/plain", "Write error");
            }
        } else {
            request->send(200, "text/plain", "NTP not synced");
        }
        return;
    }

    int yr    = request->hasArg("year")   ? request->arg("year").toInt()   : -1;
    int mon   = request->hasArg("month")  ? request->arg("month").toInt()  : -1;
    int day   = request->hasArg("day")    ? request->arg("day").toInt()    : -1;
    int hr    = request->hasArg("hour")   ? request->arg("hour").toInt()   : -1;
    int min   = request->hasArg("minute") ? request->arg("minute").toInt() : -1;
    int sec   = request->hasArg("second") ? request->arg("second").toInt() : 0;

    if (yr < 0 || mon < 0 || day < 0 || hr < 0 || min < 0) {
        request->send(200, "text/plain", "Missing parameters");
        return;
    }

    if (setTime(yr, mon, day, hr, min, sec)) {
        request->send(200, "text/plain", "OK");
    } else {
        request->send(200, "text/plain", "Error");
    }
}

void CLASS_MODULE_DS3231::handleSetAlarm1(AsyncWebServerRequest *request) {
    DEBUGDS3231("%s\r\n", __FUNCTION__);
    if (_config.addr == DS3231_ADDR_NONE) { request->send(200, "text/plain", "DS3231 not connected"); return; }

    uint8_t hour  = request->hasArg("hour")   ? (uint8_t)request->arg("hour").toInt()   : 0;
    uint8_t min   = request->hasArg("min")    ? (uint8_t)request->arg("min").toInt()    : 0;
    uint8_t sec   = request->hasArg("sec")    ? (uint8_t)request->arg("sec").toInt()    : 0;
    uint8_t mode  = request->hasArg("mode")   ? (uint8_t)request->arg("mode").toInt()   : 4;
    uint8_t dayOrDate = request->hasArg("dayOrDate") ? (uint8_t)request->arg("dayOrDate").toInt() : 1;
    bool isDOW    = request->hasArg("isDayOfWeek") && request->arg("isDayOfWeek") == "true";

    if (setAlarm1(hour, min, sec, mode, dayOrDate, isDOW)) {
        request->send(200, "text/plain", "OK");
    } else {
        request->send(200, "text/plain", "Error");
    }
}

void CLASS_MODULE_DS3231::handleSetAlarm2(AsyncWebServerRequest *request) {
    DEBUGDS3231("%s\r\n", __FUNCTION__);
    if (_config.addr == DS3231_ADDR_NONE) { request->send(200, "text/plain", "DS3231 not connected"); return; }

    uint8_t hour  = request->hasArg("hour")   ? (uint8_t)request->arg("hour").toInt()   : 0;
    uint8_t min   = request->hasArg("min")    ? (uint8_t)request->arg("min").toInt()    : 0;
    uint8_t mode  = request->hasArg("mode")   ? (uint8_t)request->arg("mode").toInt()   : 3;
    uint8_t dayOrDate = request->hasArg("dayOrDate") ? (uint8_t)request->arg("dayOrDate").toInt() : 1;
    bool isDOW    = request->hasArg("isDayOfWeek") && request->arg("isDayOfWeek") == "true";

    if (setAlarm2(hour, min, mode, dayOrDate, isDOW)) {
        request->send(200, "text/plain", "OK");
    } else {
        request->send(200, "text/plain", "Error");
    }
}

void CLASS_MODULE_DS3231::handleSetReg(AsyncWebServerRequest *request) {
    DEBUGDS3231("%s\r\n", __FUNCTION__);
    if (_config.addr == DS3231_ADDR_NONE) { request->send(200, "text/plain", "DS3231 not connected"); return; }

    if (request->hasArg("ctrl")) {
        uint8_t val = (uint8_t)strtol(request->arg("ctrl").c_str(), NULL, 16);
        _writeReg(0x0E, val);
    } else if (request->hasArg("bbsqw") || request->hasArg("rs") ||
               request->hasArg("intcn") || request->hasArg("a1ie") || request->hasArg("a2ie")) {
        // Поразрядное управление битами Control (0x0E) — режим выхода SQW/INT
#if defined(ESP32)
        if (request->hasArg("bbsqw")) {
            _config.ctrlBbsqw = (request->arg("bbsqw") == "true" || request->arg("bbsqw") == "1");
        }
        if (request->hasArg("rs")) {
            uint8_t rs = (uint8_t)request->arg("rs").toInt();
            if (rs > 3) { rs = 3; }
            _config.ctrlRs = rs;
        }
        if (request->hasArg("intcn")) {
            _config.ctrlIntcn = (request->arg("intcn") == "true" || request->arg("intcn") == "1");
        }
        if (request->hasArg("a1ie")) {
            _config.ctrlA1ie = (request->arg("a1ie") == "true" || request->arg("a1ie") == "1");
        }
        if (request->hasArg("a2ie")) {
            _config.ctrlA2ie = (request->arg("a2ie") == "true" || request->arg("a2ie") == "1");
        }
        _applyCtrlBits();
        saveConfig();
#endif
    }
    if (request->hasArg("stat")) {
        uint8_t val = (uint8_t)strtol(request->arg("stat").c_str(), NULL, 16);
        _writeReg(0x0F, val);
        // После сброса флагов Status выводим сообщение о свободном состоянии
        DEBUGDS3231("DS3231: Status register written 0x%02X\r\n", val);
    }
#if defined(ESP32)
    checkAlarmFlags();
#endif
    request->send(200, "text/plain", "OK");
}

void CLASS_MODULE_DS3231::handleSaveConfig(AsyncWebServerRequest *request) {
    DEBUGDS3231("%s\r\n", __FUNCTION__);
    if (request->hasArg("autoPoll")) {
        _config.autoPoll = (request->arg("autoPoll") == "true");
    }
    if (request->hasArg("pollInterval")) {
        uint16_t val = (uint16_t)request->arg("pollInterval").toInt();
        if (val < 1)  { val = 1; }
        if (val > 3600) { val = 3600; }
        _config.pollInterval = val;
    }
#if defined(ESP32)
    if (request->hasArg("sqwEnabled")) {
        _config.sqwEnabled = (request->arg("sqwEnabled") == "true");
    }
    if (request->hasArg("sqwMode")) {
        uint8_t m = (uint8_t)request->arg("sqwMode").toInt();
        if (m != DS3231_SQW_MODE_POLLING && m != DS3231_SQW_MODE_INTERRUPT) { m = DS3231_SQW_MODE_POLLING; }
        _config.sqwMode = m;
    }
    if (request->hasArg("sqwLevel")) {
        _config.sqwLevelActive = (request->arg("sqwLevel") == "1" || request->arg("sqwLevel") == "true");
    }
    // Бит SQW — отдельная кнопка /set_reg, здесь не пишем.
#endif
    saveConfig();
#if defined(ESP32)
    // Переинициализация GPIO согласно новому конфигу
    sqwGpioReinit();
#endif
    request->send(200, "text/plain", "OK");
}

void CLASS_MODULE_DS3231::handleInfo(AsyncWebServerRequest *request) {
    DEBUGDS3231("%s\r\n", __FUNCTION__);
    String values = "";
    values += "ds_autoPoll|"      + String(_config.autoPoll ? "checked" : "") + "|chk\n";
    values += "ds_pollInterval|"  + String(_config.pollInterval)               + "|input\n";
    values += "ds_scanRetries|"   + String(DS3231_SCAN_RETRIES)               + "|div\n";
#if defined(ESP32)
    values += "ds_sqw_enabled|"   + String(_config.sqwEnabled ? "checked" : "") + "|chk\n";
    values += "ds_sqw_mode|"      + String(_config.sqwMode)                     + "|input\n";
    values += "ds_sqw_level|"     + String(_config.sqwLevelActive ? "1" : "0")  + "|input\n";
    values += "ds_sqw_pin|"       + String(DS3231_SQW_PIN)                      + "|div\n";
    values += "ds_ctrl_bbsqw|"    + String(_config.ctrlBbsqw ? "1" : "0")       + "|input\n";
    values += "ds_ctrl_rs|"       + String(_config.ctrlRs)                      + "|input\n";
    values += "ds_ctrl_intcn|"    + String(_config.ctrlIntcn ? "1" : "0")       + "|input\n";
    values += "ds_ctrl_a1ie|"     + String(_config.ctrlA1ie ? "checked" : "")   + "|chk\n";
    values += "ds_ctrl_a2ie|"     + String(_config.ctrlA2ie ? "checked" : "")   + "|chk\n";
#endif
    request->send(200, "text/plain", values);
}

#if defined(ESP32)
// Поля конфигурации SQW/GPIO для страницы. Вызывается handleRead/handlePoll
void CLASS_MODULE_DS3231::emitSqwFields(String &values) {
    values += "ds_sqw_enabled|" + String(_config.sqwEnabled ? "checked" : "") + "|chk\n";
    values += "ds_sqw_mode|"    + String(_config.sqwMode)                       + "|input\n";
    values += "ds_sqw_level|"   + String(_config.sqwLevelActive ? "1" : "0")    + "|input\n";
    values += "ds_sqw_level_read|" + String(getSqwLevel() ? "1" : "0")          + "|div\n";
    values += "ds_sqw_pin|"     + String(DS3231_SQW_PIN)                        + "|div\n";

    // Управляемые биты Control (0x0E)
    values += "ds_ctrl_bbsqw|"  + String(_config.ctrlBbsqw ? "1" : "0") + "|input\n";
    values += "ds_ctrl_rs|"     + String(_config.ctrlRs)               + "|input\n";
    values += "ds_ctrl_intcn|"  + String(_config.ctrlIntcn ? "1" : "0") + "|input\n";
    values += "ds_ctrl_a1ie|"   + String(_config.ctrlA1ie ? "checked" : "") + "|chk\n";
    values += "ds_ctrl_a2ie|"   + String(_config.ctrlA2ie ? "checked" : "") + "|chk\n";

    emitAlarmState(values);
}

// Статус сработавших будильников на основе сохранённого времени последнего срабатывания.
void CLASS_MODULE_DS3231::emitAlarmState(String &values) {
    bool has1 = (_lastAlarm1At != 0);
    bool has2 = (_lastAlarm2At != 0);
    String t1, t2;
    ns_module_ds3231::_formatAlarmStamp(_lastAlarm1At, t1);
    ns_module_ds3231::_formatAlarmStamp(_lastAlarm2At, t2);

    String st = "";
    if (has1 && has2) { st = "Alarm 1: " + t1 + " | Alarm 2: " + t2; }
    else if (has1)    { st = "Alarm 1 сработал: " + t1; }
    else if (has2)    { st = "Alarm 2 сработал: " + t2; }
    else              { st = "--"; }
    values += "ds_alarm_state|" + st + "|div\n";
    values += "ds_alarm_any|"   + String(((has1 || has2) ? "1" : "0")) + "|div\n";
}
#endif // ESP32

// ============================================================
// Конфиг
// ============================================================

void CLASS_MODULE_DS3231::defaultConfig() {
    _config.addr         = DS3231_ADDR_NONE;
    _config.autoPoll     = false;
    _config.pollInterval = 5;
#if defined(ESP32)
    _config.sqwEnabled     = true;          // мониторинг SQW/INT# включён по умолчанию
    _config.sqwMode        = DS3231_SQW_MODE_POLLING;
    _config.sqwLevelActive = false;         // INT# активен по низкому уровню
    // Бит Control (0x0E): выход INT# по будильникам, меандр выключен
    _config.ctrlBbsqw = false;
    _config.ctrlRs     = 0;                 // 1Гц (при BBSQW=1)
    _config.ctrlIntcn  = true;              // INT# по будильникам
    _config.ctrlA1ie   = true;
    _config.ctrlA2ie   = true;
#endif
}

bool CLASS_MODULE_DS3231::loadConfig() {
    DEBUGDS3231("%s\r\n", __FUNCTION__);
    JsonDocument doc;
    if (core_json.jsonFileLoadDoc(CONFIG_FILE_DS3231, doc) == false) { return false; }

    _config.addr         = doc["addr"].as<uint8_t>();
    _config.autoPoll     = doc["autoPoll"].as<bool>();
    _config.pollInterval = doc["pollInterval"].as<uint16_t>();
#if defined(ESP32)
    _config.sqwEnabled     = doc["sqwEnabled"].as<bool>();
    _config.sqwMode        = doc["sqwMode"].as<uint8_t>();
    _config.sqwLevelActive = doc["sqwLevelActive"].as<bool>();
    _config.ctrlBbsqw      = doc["ctrlBbsqw"].as<bool>();
    _config.ctrlRs         = doc["ctrlRs"].as<uint8_t>();
    _config.ctrlIntcn      = doc["ctrlIntcn"].as<bool>();
    _config.ctrlA1ie       = doc["ctrlA1ie"].as<bool>();
    _config.ctrlA2ie       = doc["ctrlA2ie"].as<bool>();
    if (_config.sqwMode != DS3231_SQW_MODE_POLLING && _config.sqwMode != DS3231_SQW_MODE_INTERRUPT) {
        _config.sqwMode = DS3231_SQW_MODE_POLLING;
    }
#endif

    DEBUGDS3231("addr: 0x%02X, autoPoll: %d, pollInterval: %d\r\n", _config.addr, _config.autoPoll, _config.pollInterval);
    return true;
}

bool CLASS_MODULE_DS3231::saveConfig() {
    DEBUGDS3231("%s\r\n", __FUNCTION__);
    JsonDocument doc;
    core_json.jsonFileLoadDoc(CONFIG_FILE_DS3231, doc);
    doc["addr"]         = _config.addr;
    doc["autoPoll"]     = _config.autoPoll;
    doc["pollInterval"] = _config.pollInterval;
#if defined(ESP32)
    doc["sqwEnabled"]     = _config.sqwEnabled;
    doc["sqwMode"]        = _config.sqwMode;
    doc["sqwLevelActive"] = _config.sqwLevelActive;
    doc["ctrlBbsqw"]      = _config.ctrlBbsqw;
    doc["ctrlRs"]         = _config.ctrlRs;
    doc["ctrlIntcn"]      = _config.ctrlIntcn;
    doc["ctrlA1ie"]       = _config.ctrlA1ie;
    doc["ctrlA2ie"]       = _config.ctrlA2ie;
#endif
    return core_json.jsonFileSaveDoc(CONFIG_FILE_DS3231, doc);
}

// ============================================================
// Версионные методы
// ============================================================

String CLASS_MODULE_DS3231::getVersionStr() {
    return String(MODULE_DS3231_VERSION);
}

String CLASS_MODULE_DS3231::getGeneratedTime() {
    return String(MODULE_DS3231_GENERATED_TIME);
}

String CLASS_MODULE_DS3231::getCommitDateStr() {
    return String(MODULE_DS3231_COMMIT_DATE_STR);
}

void CLASS_MODULE_DS3231::html_ver_get(AsyncWebServerRequest *request) {
    DEBUGDS3231("%s\r\n", __FUNCTION__);
    String values = "";
    values += "ds3231version|" + getVersionStr()    + "|div\n";
    values += "ds3231gentime|" + getGeneratedTime() + "|div\n";
    values += "ds3231gendate|" + getCommitDateStr() + "|div\n";
    request->send(200, "text/plain", values);
}

// ============================================================
// Терминальные команды
// ============================================================

void ds3231TerminalRegister() {
    term.addCommand("ds-alarm", ds3231CmdAlarm);
    term.addCommand("ds-sqw",   ds3231CmdSqw);
    term.addCommand("ds-sqr",   ds3231CmdSqr);
}
