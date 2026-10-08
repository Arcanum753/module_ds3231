#include "core_web/FSWebServerLib.h"

#include "module_ds3231.h"
#include "common_module.h"
#include "core_sys/eertos.h"

#include "core_terminal/core_terminal.h"
#include "core_terminal/ErriezSerialTerminal.h"

#include <Wire.h>
#include "common/TimeLib.h"

#if defined(ESP32)
#include <esp_task_wdt.h>
#include <esp_attr.h>
#endif

// ============================================================
// Свободные функции
// ============================================================

#if defined(ESP32)
void IRAM_ATTR ds3231SqwIsr() {
    module_ds3231.sqwSetIrqFlag();
}

void ds3231SqwPollTask() {
    module_ds3231.sqwPollStep();
}
#endif

// ============================================================
// Публичное API
// ============================================================

time_t CLASS_MODULE_DS3231::getTime() {
    if (_config.addr == DS3231_ADDR_NONE) { return 0; }
    // Бит 7 регистра 0x00 — CH (Clock Halt). Если осциллятор был остановлен,
    // снимаем стоп-бит, чтобы продолжить счёт времени.
    uint8_t secReg = _readReg(0x00);
    if (secReg & 0x80) {
        _writeReg(0x00, secReg & 0x7F);
    }
    // OSF (0x0F bit7) — осциллятор останавливался, время недостоверно.
    // Возвращаем только надёжное время; флаг не сбрасываем.
    uint8_t stat = _readReg(0x0F);
    if (stat & 0x80) { return 0; }
    return _readTime();
}

bool CLASS_MODULE_DS3231::setTime(time_t t) {
    return _writeTime(t);
}

bool CLASS_MODULE_DS3231::setTime(int yr, int mon, int day, int hr, int min, int sec) {
    if (yr < 2000) { yr += 2000; }
    if (yr < 2000 || yr > 2099) { _lastError = -10; return false; }
    if (mon < 1 || mon > 12)    { _lastError = -11; return false; }
    if (day < 1 || hr < 0 || min < 0 || sec < 0 ||
        hr > 23 || min > 59 || sec > 59) { _lastError = -12; return false; }
    // Проверяем фактическое число дней в месяце (с учётом високосного года).
    static const uint8_t daysInMonth[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    uint8_t maxDays = daysInMonth[mon - 1];
    if (mon == 2) {
        bool leap = ((yr % 4 == 0) && (yr % 100 != 0)) || (yr % 400 == 0);
        if (leap) { maxDays = 29; }
    }
    if (day > maxDays) { _lastError = -12; return false; }
    tmElements_t tm;
    tm.Year   = yr - 1970;
    tm.Month  = mon;
    tm.Day    = day;
    tm.Hour   = hr;
    tm.Minute = min;
    tm.Second = sec;
    time_t t = makeTime(tm);
    return _writeTime(t);
}

bool CLASS_MODULE_DS3231::getTemperature(float &temp) {
    uint8_t buf[2];
    if (!_readBlock(0x11, buf, 2)) { return false; }
    // Проверяем целый градус в допустимом диапазоне — иначе данные невалидны
    // (например, чтение плавающего вывода без подтяжки).
    int8_t tempUpper = (int8_t)buf[0];
    if (tempUpper < -40 || tempUpper > 85) { return false; }
    int16_t raw = ((int16_t)tempUpper << 8) | buf[1];
    temp = raw / 256.0f;
    return true;
}

bool CLASS_MODULE_DS3231::isConnected() {
    return (_config.addr != DS3231_ADDR_NONE);
}

uint8_t CLASS_MODULE_DS3231::getAddr() {
    return _config.addr;
}

int CLASS_MODULE_DS3231::getLastError() {
    return _lastError;
}

uint8_t CLASS_MODULE_DS3231::getStatusReg() {
    return _readReg(0x0F);
}

#if defined(ESP32)
bool CLASS_MODULE_DS3231::getSqwLevel() {
    return (digitalRead(DS3231_SQW_PIN) == HIGH);
}

bool CLASS_MODULE_DS3231::getAlarmFired1() { return _alarm1Fired; }
bool CLASS_MODULE_DS3231::getAlarmFired2() { return _alarm2Fired; }
time_t CLASS_MODULE_DS3231::getLastAlarm1Time() { return _lastAlarm1At; }
time_t CLASS_MODULE_DS3231::getLastAlarm2Time() { return _lastAlarm2At; }
#endif

// ============================================================
// I2C — низкоуровневые операции
// ============================================================

uint8_t CLASS_MODULE_DS3231::_readReg(uint8_t reg) {
    if (_config.addr == DS3231_ADDR_NONE) { _lastError = -1; return 0; }
    if (!_wireStarted) { _wireStarted = true; Wire.begin(); }
    Wire.beginTransmission(_config.addr);
    Wire.write(reg);
    _lastError = Wire.endTransmission();
    if (_lastError != 0) { return 0; }
    Wire.requestFrom((int)_config.addr, 1);
    if (Wire.available()) { return Wire.read(); }
    _lastError = -2;
    return 0;
}

bool CLASS_MODULE_DS3231::_writeReg(uint8_t reg, uint8_t val) {
    if (_config.addr == DS3231_ADDR_NONE) { _lastError = -1; return false; }
    if (!_wireStarted) { _wireStarted = true; Wire.begin(); }
    Wire.beginTransmission(_config.addr);
    Wire.write(reg);
    Wire.write(val);
    _lastError = Wire.endTransmission();
    return (_lastError == 0);
}

bool CLASS_MODULE_DS3231::_readBlock(uint8_t reg, uint8_t *buf, uint8_t len) {
    if (_config.addr == DS3231_ADDR_NONE) { _lastError = -1; return false; }
    if (!_wireStarted) { _wireStarted = true; Wire.begin(); }
    Wire.beginTransmission(_config.addr);
    Wire.write(reg);
    _lastError = Wire.endTransmission();
    if (_lastError != 0) { return false; }
    Wire.requestFrom((int)_config.addr, (int)len);
    for (uint8_t i = 0; i < len; i++) {
        if (Wire.available()) { buf[i] = Wire.read(); }
        else { _lastError = -2; return false; }
    }
    return true;
}

bool CLASS_MODULE_DS3231::_writeBlock(uint8_t reg, uint8_t *buf, uint8_t len) {
    if (_config.addr == DS3231_ADDR_NONE) { _lastError = -1; return false; }
    if (!_wireStarted) { _wireStarted = true; Wire.begin(); }
    Wire.beginTransmission(_config.addr);
    Wire.write(reg);
    for (uint8_t i = 0; i < len; i++) { Wire.write(buf[i]); }
    _lastError = Wire.endTransmission();
    return (_lastError == 0);
}

// ============================================================
// Детекция DS3231
// ============================================================

bool CLASS_MODULE_DS3231::_detectDS3231(uint8_t addr) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    if (err != 0) { return false; }

    Wire.beginTransmission(addr);
    Wire.write(0x0F);
    err = Wire.endTransmission();
    if (err != 0) { return false; }
    Wire.requestFrom((int)addr, 1);
    if (!Wire.available()) { return false; }
    uint8_t stat = Wire.read();

    Wire.beginTransmission(addr);
    Wire.write(0x0E);
    err = Wire.endTransmission();
    if (err != 0) { return false; }
    Wire.requestFrom((int)addr, 1);
    if (!Wire.available()) { return false; }
    uint8_t ctrl = Wire.read();

    if (ctrl == 0xFF || stat == 0xFF) { return false; }

    Wire.beginTransmission(addr);
    Wire.write(0x11);
    err = Wire.endTransmission();
    if (err != 0) { return false; }
    Wire.requestFrom((int)addr, 2);
    if (Wire.available() < 2) { return false; }
    int8_t tempUpper = (int8_t)Wire.read();
    if (tempUpper < -40 || tempUpper > 85) { return false; }

    return true;
}

uint8_t CLASS_MODULE_DS3231::_scanForDS3231() {
    DEBUGDS3231("DS3231: сканирование шины I2C...\r\n");
    for (uint8_t addr = 0x01; addr < 0x7F; addr++) {
#if defined(ESP32)
        esp_task_wdt_reset();
#elif defined(ESP8266)
        ESP.wdtFeed();
#endif
        if (_detectDS3231(addr)) {
            DEBUGDS3231("DS3231: 0x%02X\r\n", addr);
            return addr;
        }
    }
    DEBUGDS3231("DS3231: устройство НЕ найдено\r\n");
    return DS3231_ADDR_NONE;
}

// ============================================================
// Чтение / запись времени
// ============================================================

time_t CLASS_MODULE_DS3231::_readTime() {
    uint8_t buf[7];
    if (!_readBlock(0x00, buf, 7)) { return 0; }

    uint8_t sec   = ns_module_ds3231::_bcd2dec(buf[0] & 0x7F);
    uint8_t min   = ns_module_ds3231::_bcd2dec(buf[1]);
    uint8_t hour  = ns_module_ds3231::_bcd2dec(buf[2] & 0x3F);
    uint8_t day   = ns_module_ds3231::_bcd2dec(buf[4]);
    uint8_t mon   = ns_module_ds3231::_bcd2dec(buf[5] & 0x1F);
    uint16_t yr   = ns_module_ds3231::_bcd2dec(buf[6]) + 2000;

    tmElements_t tm;
    tm.Year   = yr - 1970;
    tm.Month  = mon;
    tm.Day    = day;
    tm.Hour   = hour;
    tm.Minute = min;
    tm.Second = sec;
    return makeTime(tm);
}

bool CLASS_MODULE_DS3231::_writeTime(time_t t) {
    tmElements_t tm;
    breakTime(t, tm);

    uint8_t buf[7];
    buf[0] = ns_module_ds3231::_dec2bcd(tm.Second) & 0x7F;
    buf[1] = ns_module_ds3231::_dec2bcd(tm.Minute);
    buf[2] = ns_module_ds3231::_dec2bcd(tm.Hour);
    buf[3] = ns_module_ds3231::_dec2bcd(weekday(t));
    buf[4] = ns_module_ds3231::_dec2bcd(tm.Day);
    buf[5] = ns_module_ds3231::_dec2bcd(tm.Month);
    buf[6] = ns_module_ds3231::_dec2bcd((tm.Year + 1970) - 2000);
    bool ok = _writeBlock(0x00, buf, 7);
    if (ok) {
        // Время установлено — сбрасываем OSF (0x0F bit7): осциллятор остановлен,
        // но время теперь достоверно. Без этого без батарейки RTC остаётся «osf» навсегда.
        uint8_t stat = _readReg(0x0F);
        if (stat & 0x80) { _writeReg(0x0F, stat & 0x7F); }
    }
    return ok;
}

// ============================================================
// Будильники
// ============================================================

bool CLASS_MODULE_DS3231::getAlarm1(uint8_t &hour, uint8_t &min, uint8_t &sec, uint8_t &mode, uint8_t &dayOrDate, bool &isDayOfWeek) {
    uint8_t buf[4];
    if (!_readBlock(0x07, buf, 4)) { return false; }
    sec  = ns_module_ds3231::_bcd2dec(buf[0] & 0x7F);
    min  = ns_module_ds3231::_bcd2dec(buf[1] & 0x7F);
    hour = ns_module_ds3231::_bcd2dec(buf[2] & 0x3F);
    ns_module_ds3231::_decodeAlarm1Mode(buf, mode, dayOrDate, isDayOfWeek);
    return true;
}

bool CLASS_MODULE_DS3231::setAlarm1(uint8_t hour, uint8_t min, uint8_t sec, uint8_t mode, uint8_t dayOrDate, bool isDayOfWeek) {
    if (mode > 4) { mode = 4; }
    uint8_t buf[4];
    ns_module_ds3231::_encodeAlarm1Mode(buf, sec, min, hour, mode, dayOrDate, isDayOfWeek);
    bool ok = _writeBlock(0x07, buf, 4);
    if (ok) {
        // Разрешаем прерывание Alarm 1 (A1IE) только когда выход настроен на
        // будильники (INTCN=1). При меандре (BBSQW=1) биты прерываний не влияют.
        if (_config.ctrlIntcn && !_config.ctrlBbsqw) {
            uint8_t ctrl = _readReg(0x0E);
            ctrl |= 0x01;
            _writeReg(0x0E, ctrl);
        }
        // Сбрасываем внутренний флаг, а также флаг A1F в Status, чтобы
        // предыдущее срабатывание не считалось новым.
#if defined(ESP32)
        _alarm1Fired = false;
#endif
        uint8_t stat = _readReg(0x0F);
        stat &= ~0x01;
        _writeReg(0x0F, stat);
    }
    return ok;
}

bool CLASS_MODULE_DS3231::getAlarm2(uint8_t &hour, uint8_t &min, uint8_t &mode, uint8_t &dayOrDate, bool &isDayOfWeek) {
    uint8_t buf[3];
    if (!_readBlock(0x0B, buf, 3)) { return false; }
    min  = ns_module_ds3231::_bcd2dec(buf[0] & 0x7F);
    hour = ns_module_ds3231::_bcd2dec(buf[1] & 0x3F);
    ns_module_ds3231::_decodeAlarm2Mode(buf, mode, dayOrDate, isDayOfWeek);
    return true;
}

bool CLASS_MODULE_DS3231::setAlarm2(uint8_t hour, uint8_t min, uint8_t mode, uint8_t dayOrDate, bool isDayOfWeek) {
    if (mode > 3) { mode = 3; }
    uint8_t buf[3];
    ns_module_ds3231::_encodeAlarm2Mode(buf, min, hour, mode, dayOrDate, isDayOfWeek);
    bool ok = _writeBlock(0x0B, buf, 3);
    if (ok) {
        // Разрешаем прерывание Alarm 2 (A2IE) только когда выход настроен на
        // будильники (INTCN=1). При меандре (BBSQW=1) биты прерываний не влияют.
        if (_config.ctrlIntcn && !_config.ctrlBbsqw) {
            uint8_t ctrl = _readReg(0x0E);
            ctrl |= 0x02;
            _writeReg(0x0E, ctrl);
        }
        // Сбрасываем внутренний флаг, а также флаг A2F в Status, чтобы
        // предыдущее срабатывание не считалось новым.
#if defined(ESP32)
        _alarm2Fired = false;
#endif
        uint8_t stat = _readReg(0x0F);
        stat &= ~0x02;
        _writeReg(0x0F, stat);
    }
    return ok;
}

// ============================================================
// SQW / GPIO — реализация (только ESP32)
// ============================================================
#if defined(ESP32)

// Собрать байт Control (0x0E) из полей конфига
bool CLASS_MODULE_DS3231::_ctrlBitsToReg() {
    // EOSC (bit7)=0 — осциллятор включён
    uint8_t ctrl = 0x00;
    if (_config.ctrlBbsqw)  { ctrl |= 0x40; }
    if ((_config.ctrlRs & 0x03) != 0) { ctrl |= ((_config.ctrlRs & 0x03) << 3); }
    if (_config.ctrlIntcn)  { ctrl |= 0x04; }
    if (_config.ctrlA2ie)   { ctrl |= 0x02; }
    if (_config.ctrlA1ie)   { ctrl |= 0x01; }
    return _writeReg(0x0E, ctrl);
}

// Записать биты Control (0x0E) из конфига
void CLASS_MODULE_DS3231::_applyCtrlBits() {
    if (_config.addr == DS3231_ADDR_NONE) { return; }
    _ctrlBitsToReg();
}

// Инициализация GPIO вывода SQW/INT#
void CLASS_MODULE_DS3231::sqwGpioInit() {
    DEBUGDS3231("%s\r\n", __FUNCTION__);
    _sqwIrqFlag = false;
    _sqwCareActive = false;
    _alarm1Fired = false;
    _alarm2Fired = false;
    _sqwInterrupting = false;

    if (_config.addr == DS3231_ADDR_NONE) {
        DEBUGDS3231("DS3231: cannot init SQW, DS3231 not connected\r\n");
        return;
    }

    // Всегда планируем периодический детектор флагов будильников (раз в секунду),
    // независимо от способа чтения GPIO, чтобы сообщения выводились вовремя.
    DelTimerTask(ds3231SqwPollTask);
    SetTimerTask(ds3231SqwPollTask, 1000);
    DEBUGDS3231("DS3231: alarm detector scheduled (1s)\r\n");

    if (!_config.sqwEnabled) {
        DEBUGDS3231("DS3231: monitoring SQW disabled\r\n");
        return;
    }

    pinMode(DS3231_SQW_PIN, INPUT_PULLUP);
    _sqwLastLowEdge = (digitalRead(DS3231_SQW_PIN) == (uint8_t)_config.sqwLevelActive);
    DEBUGDS3231("DS3231: SQW pin D%d mode=%s activeLevel=%d\r\n",
                DS3231_SQW_PIN,
                (_config.sqwMode == DS3231_SQW_MODE_INTERRUPT) ? "interrupt" : "polling",
                _config.sqwLevelActive ? 1 : 0);

    if (_config.sqwMode == DS3231_SQW_MODE_INTERRUPT) {
        sqwEnableInterrupt();
    }
}

// Остановка мониторинга GPIO SQW (детектор флагов остаётся активным)
void CLASS_MODULE_DS3231::sqwGpioStop() {
    DEBUGDS3231("%s\r\n", __FUNCTION__);
#if defined(ESP32)
    sqwDisableInterrupt();
#endif
}

// Переинициализация после смены конфига
void CLASS_MODULE_DS3231::sqwGpioReinit() {
    DEBUGDS3231("%s\r\n", __FUNCTION__);
    sqwGpioStop();
    sqwGpioInit();
}

// Подключение прерывания
void CLASS_MODULE_DS3231::sqwEnableInterrupt() {
    DEBUGDS3231("%s\r\n", __FUNCTION__);
    if (_sqwInterrupting) { return; }
    // При активном высоком уровне отслеживаем RISING, при низком — FALLING
    int intMode = _config.sqwLevelActive ? RISING : FALLING;
    attachInterrupt(digitalPinToInterrupt(DS3231_SQW_PIN), ds3231SqwIsr, intMode);
    _sqwInterrupting = true;
    DEBUGDS3231("DS3231: SQW interrupt attached, mode=%s\r\n", (intMode == RISING) ? "RISING" : "FALLING");
}

// Отключение прерывания
void CLASS_MODULE_DS3231::sqwDisableInterrupt() {
    DEBUGDS3231("%s\r\n", __FUNCTION__);
    if (!_sqwInterrupting) { return; }
    detachInterrupt(digitalPinToInterrupt(DS3231_SQW_PIN));
    _sqwInterrupting = false;
}

// Вызвается из ISR — только ставим флаг, тяжёлую работу делаем в main-loop
void CLASS_MODULE_DS3231::sqwSetIrqFlag() {
    _sqwIrqFlag = true;
}

// Обработка флага прерывания/спайка в основном контексте
void CLASS_MODULE_DS3231::checkSqw() {
    bool level = (digitalRead(DS3231_SQW_PIN) == (uint8_t)_config.sqwLevelActive);
    // Ловим фронт «наступление активного сигнала»
    if (level && !_sqwLastLowEdge) {
        if (!_sqwCareActive) {
            _sqwCareActive = true;
            DEBUGDS3231("DS3231: INT# / SQW стал активным (будильник сработал)\r\n");
        }
    }
    _sqwLastLowEdge = level;
    _sqwIrqFlag = false;
}

// Шаг опроса (периодическая задача)
void CLASS_MODULE_DS3231::sqwPollStep() {
    checkAlarmFlags();
    // Перепланируем задачу на следующий интервал
    SetTimerTask(ds3231SqwPollTask, 1000);
}

// Обработка срабатывания одного будильника: вывод сообщения с временем в терминал
// и автовзвод (перезапись) для периодических режимов mode=1..3.
// mode=0 (раз в секунду) — только краткое сообщение, перезапись не требуется.
// mode=4 (по дню/дате) — полное сообщение, но повторно в течение суток не взводим.
void CLASS_MODULE_DS3231::_handleAlarmFired(uint8_t alarmNum, uint8_t mode,
                                            uint8_t hour, uint8_t min, uint8_t sec,
                                            uint8_t dayOrDate, bool isDayOfWeek) {
    time_t t = _readTime();
    String when;
    ns_module_ds3231::_formatAlarmTime(t, when);

    // Сохраняем время последнего срабатывания (для всех режимов, включая mode=0).
    // Не сбрасывается при setAlarm1/setAlarm2 — обновляется только фактической сработкой.
    if (alarmNum == 1) { _lastAlarm1At = t; } else { _lastAlarm2At = t; }

    if (mode == 0) {
        // Раз в секунду — краткое сообщение без полной даты.
        DEBUGDS3231("DS3231: Alarm %d fired\r\n", alarmNum);
        return;
    }

    // Полная дата и время срабатывания для остальных режимов.
    DEBUGDS3231("DS3231: Alarm %d fired: %s\r\n", alarmNum, when.c_str());

    // Автовзвод только для периодических режимов mode=1..3 (не mode=0 и не mode=4).
    if (mode >= 1 && mode <= 3) {
        if (alarmNum == 1) {
            setAlarm1(hour, min, sec, mode, dayOrDate, isDayOfWeek);
        } else {
            setAlarm2(hour, min, mode, dayOrDate, isDayOfWeek);
        }
    }
}

// Проверка флагов регистра Status (0x0F): A1F(bit0), A2F(bit1)
void CLASS_MODULE_DS3231::checkAlarmFlags() {
    if (_config.addr == DS3231_ADDR_NONE) { return; }

    // Обработка сигнала от GPIO (если включён мониторинг)
    if (_config.sqwEnabled && _sqwIrqFlag) {
        checkSqw();
    }

    uint8_t stat = _readReg(0x0F);
    uint8_t a1f = (stat & 0x01) ? 1 : 0;
    uint8_t a2f = (stat & 0x02) ? 1 : 0;

    // Вывод сообщения по фронту (флаг перешёл из 0 в 1) — под флагом DEBUG_DS3231.
    // После срабатывания сбрасываем флаг, чтобы каждое новое срабатывание давало
    // сообщение. Для периодических режимов (mode=1..3) дополнительно перезаписываем
    // будильник (автовзвод), чтобы гарантировать новый фронт при следующем совпадении.
    if (a1f && !_alarm1Fired) {
        _alarm1Fired = true;
        stat &= ~0x01;
        _writeReg(0x0F, stat);

        uint8_t hour, min, sec, mode, dayOrDate;
        bool isDOW;
        if (getAlarm1(hour, min, sec, mode, dayOrDate, isDOW)) {
            _handleAlarmFired(1, mode, hour, min, sec, dayOrDate, isDOW);
        } else {
            DEBUGDS3231("DS3231: Alarm 1 fired (A1F=1)\r\n");
        }
    }
    if (a2f && !_alarm2Fired) {
        _alarm2Fired = true;
        stat &= ~0x02;
        _writeReg(0x0F, stat);

        uint8_t hour, min, mode, dayOrDate;
        bool isDOW;
        if (getAlarm2(hour, min, mode, dayOrDate, isDOW)) {
            _handleAlarmFired(2, mode, hour, min, 0, dayOrDate, isDOW);
        } else {
            DEBUGDS3231("DS3231: Alarm 2 fired (A2F=1)\r\n");
        }
    }
}

#endif // ESP32

// ============================================================
// Терминальные команды
// ============================================================

void ds3231CmdAlarm() {
    // Показать статус сработавших будильников (флаги A1F/A2F регистра Status)
    uint8_t stat = module_ds3231.getStatusReg();
    DEBUGDS3231("DS3231 Status=0x%02X A1F=%d A2F=%d\r\n",
                stat, (stat & 0x01) ? 1 : 0, (stat & 0x02) ? 1 : 0);
    if (stat & 0x01) { DEBUGDS3231("DS3231: Alarm 1 fired\r\n"); }
    if (stat & 0x02) { DEBUGDS3231("DS3231: Alarm 2 fired\r\n"); }
    if (!(stat & 0x03)) { DEBUGDS3231("DS3231: no alarm fired\r\n"); }
#if defined(ESP32)
    // Время последнего срабатывания (сохранённое в RAM).
    if (module_ds3231.getLastAlarm1Time() != 0) {
        String s; ns_module_ds3231::_formatAlarmStamp(module_ds3231.getLastAlarm1Time(), s);
        DEBUGDS3231("DS3231: Alarm 1 last fired: %s\r\n", s.c_str());
    }
    if (module_ds3231.getLastAlarm2Time() != 0) {
        String s; ns_module_ds3231::_formatAlarmStamp(module_ds3231.getLastAlarm2Time(), s);
        DEBUGDS3231("DS3231: Alarm 2 last fired: %s\r\n", s.c_str());
    }
    if (module_ds3231.getLastAlarm1Time() == 0 && module_ds3231.getLastAlarm2Time() == 0) {
        DEBUGDS3231("DS3231: no alarm fired since boot\r\n");
    }
#endif
}

void ds3231CmdSqw() {
    // Управление GPIO мониторинга SQW/INT#
#if defined(ESP32)
    String arg1 = term.getNext();
    DEBUGDS3231("ds-sqw arg=%s\r\n", arg1.c_str());
    if (arg1 == "on" || arg1 == "1") {
        module_ds3231.sqwGpioInit();
        module_ds3231._config.sqwEnabled = true;
        module_ds3231.saveConfig();
        DBG_MOD("[M_DS3231] ", "SQW monitoring ON\r\n");
        return;
    }
    if (arg1 == "off" || arg1 == "0") {
        module_ds3231._config.sqwEnabled = false;
        module_ds3231.sqwGpioStop();
        module_ds3231.saveConfig();
        DBG_MOD("[M_DS3231] ", "SQW monitoring OFF\r\n");
        return;
    }
    if (arg1 == "poll") {
        module_ds3231._config.sqwMode = DS3231_SQW_MODE_POLLING;
        module_ds3231.saveConfig();
        module_ds3231.sqwGpioReinit();
        DBG_MOD("[M_DS3231] ", "SQW mode: polling\r\n");
        return;
    }
    if (arg1 == "int") {
        module_ds3231._config.sqwMode = DS3231_SQW_MODE_INTERRUPT;
        module_ds3231.saveConfig();
        module_ds3231.sqwGpioReinit();
        DBG_MOD("[M_DS3231] ", "SQW mode: interrupt\r\n");
        return;
    }
    // По умолчанию печатаем состояние
    DBG_MOD("[M_DS3231] ", "SQW enabled=%d mode=%s level=%d pin D%d\r\n",
            module_ds3231._config.sqwEnabled ? 1 : 0,
            (module_ds3231._config.sqwMode == DS3231_SQW_MODE_INTERRUPT) ? "interrupt" : "polling",
            module_ds3231._config.sqwLevelActive ? 1 : 0,
            DS3231_SQW_PIN);
    DBG_MOD("[M_DS3231] ", "SQW raw=%d\r\n", digitalRead(DS3231_SQW_PIN));
#endif
#if defined(ESP8266)
    DBG_MOD("[M_DS3231] ", "SQW GPIO available only on ESP32\r\n");
#endif
}

void ds3231CmdSqr() {
    // Показать/записать биты Control (0x0E) — режим выхода SQW
#if defined(ESP32)
    String arg1 = term.getNext();
    if (arg1 == "bbsqw") {
        module_ds3231._config.ctrlBbsqw = true;
        module_ds3231._config.ctrlIntcn = false;
        module_ds3231.saveConfig();
        module_ds3231._applyCtrlBits();
        DBG_MOD("[M_DS3231] ", "SQW output: BBSQW=1 (меандр)\r\n");
        return;
    }
    if (arg1 == "intcn") {
        module_ds3231._config.ctrlIntcn = true;
        module_ds3231._config.ctrlBbsqw = false;
        module_ds3231.saveConfig();
        module_ds3231._applyCtrlBits();
        DBG_MOD("[M_DS3231] ", "SQW output: INTCN=1 (INT# по будильникам)\r\n");
        return;
    }
    uint8_t ctrl = module_ds3231._readReg(0x0E);
    DBG_MOD("[M_DS3231] ", "Control=0x%02X BBSQW=%d RS=%d INTCN=%d A1IE=%d A2IE=%d\r\n",
            ctrl,
            (ctrl & 0x40) ? 1 : 0,
            (ctrl >> 3) & 3,
            (ctrl & 0x04) ? 1 : 0,
            (ctrl & 0x01) ? 1 : 0,
            (ctrl & 0x02) ? 1 : 0);
#endif
#if defined(ESP8266)
    DBG_MOD("[M_DS3231] ", "SQW register control available only on ESP32\r\n");
#endif
}
