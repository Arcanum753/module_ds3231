#ifndef _MODULE_DS3231_TYPES_h
#define _MODULE_DS3231_TYPES_h

// ============================================================
// module_ds3231_types.h — типы, структуры и define'ы модуля DS3231.
// Реализация: module_ds3231.cpp (шаблон) и module_ds3231_engine.cpp
// (I2C, будильники, SQW/GPIO).
// ============================================================

#include <Arduino.h>
#include <stdint.h>

#define CONFIG_FILE_DS3231       "/config_ds3231.json"
#define DS3231_SCAN_RETRIES      2
#define DS3231_DEFAULT_ADDR      0x68
#define DS3231_ADDR_NONE         0

// Режимы чтения вывода SQW/INT# — настраиваются на странице модуля
#define DS3231_SQW_MODE_POLLING     0   // опрос через EERTOS timer
#define DS3231_SQW_MODE_INTERRUPT   1   // прерывание (attachInterrupt)

// Пин для вывода SQW/INT# DS3231 (только ESP32). По умолчанию D13 (DevKit v1)
#if defined(ESP32)
#ifndef DS3231_SQW_PIN
#define DS3231_SQW_PIN 13
#endif
#endif

typedef struct {
    uint8_t  addr;
    bool     autoPoll;
    uint16_t pollInterval;
    // === SQW / GPIO (только ESP32) ===
    bool     sqwEnabled;     // вкл/выкл GPIO мониторинг
    uint8_t  sqwMode;        // DS3231_SQW_MODE_POLLING / _INTERRUPT
    bool     sqwLevelActive; // активный уровень (false=LOW, true=HIGH)
    // === биты Control (0x0E) ===
    bool     ctrlBbsqw;      // BBSQW: 0=INT#, 1=меандр на выходе SQW
    uint8_t  ctrlRs;         // RS[1:0]: 0=1Гц,1=1.024кГц,2=4.096кГц,3=8.192кГц
    bool     ctrlIntcn;      // INTCN: 1=INT по будильникам, 0=выход SQW
    bool     ctrlA1ie;       // A1IE — разрешить прерывание Alarm 1
    bool     ctrlA2ie;       // A2IE — разрешить прерывание Alarm 2
} strDs3231Config;

#endif // _MODULE_DS3231_TYPES_h
