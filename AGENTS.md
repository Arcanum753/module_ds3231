# module_ds3231 — RTC DS3231

> **Опциональный модуль.** Подключается через `src_filter` + `build_flags`.
> **Работоспособен только в составе сборки, содержащей ядро.**

- **Репозиторий:** https://github.com/Arcanum753/module_ds3231
- **Папка:** `src/module_ds3231/`
- **Флаг активации:** `-D MODULE_DS3231`
- **Registry:** `object=module_ds3231`, `define=MODULE_DS3231`, `web=1`, `loop=0`,
  `time_source=1`
- **Зависит от ядра:** `core_web`, `core_sys`, `core_state`, `core_json`, `core_terminal`

## Назначение

Часы реального времени DS3231 (I2C), будильники и выход SQW (`DS3231_SQW_PIN`).
Конфиг `/config_ds3231.json`; страница `ds3231.html`.

## Источник времени: эталон с записью

`module_ds3231` — **источник времени** для `core_sys` и поддерживает **запись** (обратная
синхронизация RTC). Не владеет `time.*` и TZ/DST.

```cpp
static bool ds3231GetTime(time_t& out) {
    if (!module_ds3231.isConnected()) return false;
    time_t t = (time_t)module_ds3231.getTime();   // одна I2C-операция
    if (t < CORE_SYS_TIME_MIN_VALID) return false;
    out = t; return true;
}
static bool ds3231SetTime(time_t in) { return module_ds3231.setTime((time_t)in); }
static const char* ds3231Status() {
    if (!module_ds3231.isConnected()) return "not connected";
    if (module_ds3231.getStatusReg() & 0x80) return "osf (battery low?)";
    return "";
}

void CLASS_MODULE_DS3231::registerTimeSource() {
    core_sys.addTimeSource("ds3231", 50, ds3231GetTime, ds3231SetTime, ds3231Status);
}
```

| Параметр | Значение |
|---|---|
| Имя источника | `ds3231` |
| Приоритет | `50` |
| Критерий валидности `get()` | `isConnected() && t ≥ 2020-01-01` (OSF диагностируется в `status()`) |
| `set()` | `setTime(in)` — поддерживается; при успешной записи времени сбрасывает OSF (`0x0F` bit7) |
| `status()` | `"not connected"` / `"osf (battery low?)"` |

`ds3231GetTime()` — **одна I2C-транзакция** (без отдельного `getStatusReg()`); диагностика OSF
вызывается в `status()` только при невалидном источнике.

Установка времени (`setTime` → `_writeTime`) после успешной записи регистров времени очищает бит
OSF: время, записанное вручную, считается достоверным. Поэтому после `time.set` / RTC-обратной
синхронизации источник `ds3231` перестаёт быть «osf» (важно для RTC без батарейки).

TZ/DST теперь в `config_time.json` (владелец `core_sys`, секция «Time Sources» на `system.html`), не в конфиге/странице DS3231.

## Автокоррекция времени при старте

Поле `config_ds3231.json` — `autoCorrectOnStart` (по умолчанию `true`; старые конфиги без
поля тоже считаются включёнными). Настройка на странице DS3231, применяется при следующей
загрузке.

При `begin()` модуль оценивает RTC:

- `OSF` (0x0F bit7) или невалидные BCD/диапазон (время вне `2020-01-01 … 2099-12-31`) —
  время недостоверно; при OSF детекция не требует валидных регистров времени (иначе RTC с
  потерянным питанием не был бы найден);
- RTC не читается по I2C — не трогаем (чтобы не затереть неизвестное состояние), только лог;
- достоверное время — при старте не перезаписываем (штатная обратная синхронизация
  `core_sys` раз в час остаётся как есть).

Если RTC недостоверен и автокоррекция включена:

- при уже валидном источнике пишем `core_sys.timeNow()`;
- иначе пишем fallback внутренних часов (`2000-01-01 + аптайм`), чтобы RTC сразу начал
  идти, и ставим `_startupCorrectionPending`;
- на первом `time.source_changed` с валидным источником (NTP/GPS/ручной `time.sync_from`)
  один раз перезаписываем точное время и снимаем pending.

`_writeTime()` очищает `CH` (sec bit7) и `OSF`. Явная установка времени (веб/`time.set`)
сбрасывает pending. Fallback-время 2000 года ниже `CORE_SYS_TIME_MIN_VALID`, поэтому DS3231
не становится активным источником `core_sys` до записи реального времени.

Разбор регистров `0x00..0x06` вынесен в `common_module` (`_bcdValid`, `_timeRegsValid`,
`_decodeTime`), чтобы `_readTime()`, `_readTimeChecked()` и детекция `_detectDS3231()` не
расходились при будущих правках.

## Тестирование

Автотесты — в репозитории `module_ds3231` (вне ядра).
