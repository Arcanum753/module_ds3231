# module_ds3231 — часы реального времени DS3231

> **Опциональный модуль.** Подключается через `src_filter` + `build_flags`.
> **Работоспособен только в составе сборки, содержащей ядро** (см. `../../TRS.md` §2.1).

Драйвер RTC DS3231 по I2C: чтение и установка времени, будильники Alarm1/Alarm2,
температура, регистры управления/статуса, мониторинг вывода SQW/INT# (ESP32).

- **Репозиторий:** https://github.com/Arcanum753/module_ds3231
- **Папка:** `src/module_ds3231/`
- **Флаг активации:** `-D MODULE_DS3231` (в env встречается `-D MODULE_DS3231=1`)
- **Registry:** `object=module_ds3231`, `define=MODULE_DS3231`, `web=1`, `loop=0` (без `namespace`/`res`/`prio`)
- **Только для платформы:** обе (ESP8266/ESP32); SQW/прерывание — только ESP32
- **Зависит от модулей:** —
- **Зависит от ядра:** `core_web`, `core_sys`, `core_json`, `core_ntp` (источник времени), `core_led`

## Назначение

RTC DS3231 по I2C: чтение/установка времени, Alarm1/Alarm2, температура, регистры
управления/статуса, мониторинг SQW/INT# (ESP32).

## Функциональные требования

- FR-DS3231-1: Чтение/установка времени (`getTime`, `setTime`), проверка связи (`isConnected`, `getAddr`, `getLastError`).
- FR-DS3231-2: Будильники Alarm1/Alarm2 (`getAlarm1/setAlarm1`, `getAlarm2/setAlarm2`), фиксация срабатывания (`getAlarmFired1/2`, `getLastAlarm1/2Time`).
- FR-DS3231-3: Температура (`getTemperature`), регистр статуса (`getStatusReg`).
- FR-DS3231-4: SQW-вывод (ESP32): `sqwGpioInit/Reinit/Stop`, `sqwEnable/DisableInterrupt`, `getSqwLevel`; пин `DS3231_SQW_PIN=13`.
- FR-DS3231-5: Конфиг `/config_ds3231.json`: `addr`, `autoPoll`, `pollInterval`, `sqwEnabled`, `sqwMode`, `sqwLevelActive`, `ctrlBbsqw`, `ctrlRs`, `ctrlIntcn`, `ctrlA1ie`, `ctrlA2ie`.

## Аппаратные интерфейсы

| Интерфейс | Выводы по умолчанию | Примечание |
|-----------|---------------------|------------|
| I2C (DS3231) | ESP32 21/22, ESP8266 4/5 (SDA/SCL) | адрес задаётся в `config_ds3231.json` (`addr`) |
| DS3231 SQW/INT# | 13 (`DS3231_SQW_PIN`) | только ESP32; используется `device_clock-mech`/`device_mech-ring` |

## Веб-интерфейс

Маршруты: `GET /ds3231/read`, `/ds3231/poll`, `POST /ds3231/set_time`, `/ds3231/set_alarm1`,
`/ds3231/set_alarm2`, `/ds3231/set_reg`, `/ds3231/save`, `GET /ds3231/info`, `/ds3231/ver`.

## Конфигурация

`/config_ds3231.json` — поля: см. FR-DS3231-5. `addr` — I2C-адрес DS3231 (обычно `0x68`).

## Терминальные команды

| Команда | Назначение | Доступность |
|---------|-----------|-------------|
| `ds-alarm` / `ds-sqw` / `ds-sqr` | отладка DS3231 | только `MODULE_DS3231` |

## Слоистая структура

Из `../../LAYERS.md`: `module_ds3231` — RTC: время, будильники, SQW/GPIO, ISR; сейчас типы +
крупная логика в `.cpp`; выделить `_types.h`, `_engine.cpp`. Локальные stateless-хелперы
(BCD/alarm) — в `common_module.*`, namespace `ns_module_ds3231`.

## Тестирование

- HIL-стенд (уровень 5): чтение/установка времени, будильники, температура на реальном
  чипе DS3231 на шине I2C (см. `../../TESTING.md`).

## Ссылки

- Ядро и конвенции: `../../TRS.md`
- Слоистая структура: `../../LAYERS.md`
- Общие утилиты: `../../TRS.md` §3.1.12 (`common/`)
- Сборка: `../../BUILD.md`
- Реестр компонентов: `../../INVENTORY.md`

> Если модуль читается вне дерева ядра (standalone), корневые документы доступны в
> репозитории ядра avr-fota.
