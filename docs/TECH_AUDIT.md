# Технический аудит

Дата: 2026-07-19. Статусы: **CONFIRMED**, **PARTIAL**, **BLOCKED**.

## Установка и версия

- **CONFIRMED:** Steam AppID `214730`, build ID `20648864`.
- **CONFIRMED:** `Rangers.exe` имеет `FileVersion 2.1.2500.0` и
  `ProductVersion 2.1.2500`.
- `<GAME_ROOT>` в этой среде: `G:\SteamLibrary\steamapps\common\Space Rangers HD A War Apart`.
- Пользовательские данные и логи: `%USERPROFILE%\Documents\SpaceRangersHD`.
- Оригинальные файлы не изменялись.

## Формат модуля

Встроенные модули в `<GAME_ROOT>\Mods\Tweaks` подтверждают структуру:

```text
Mods/<Section>/<Module>/
  ModuleInfo.txt
  INSTALL.TXT или INSTALL_RUSSIAN.TXT
  CFG/...
  DATA/...
```

`ModuleInfo.txt` содержит `Name`, `Author`, `Conflict`, `Dependence`, `Priority`,
`Section`, `Languages` и описания. `INSTALL*.TXT` подключает пакеты блоком
`Packages { Package=... }`. Фактический выбор активного модуля хранится игрой в
её конфигурации; точное поведение нового CE-модуля требует smoke-теста.

## RScript

- **CONFIRMED:** RScript `4.10f`, `FileVersion 4.10.0.0`, финальная версия от
  22.12.2025.
- **CONFIRMED:** `.rson` имеет JSON-представление и `FileVersion: 8` в актуальных
  референсах.
- **CONFIRMED:** CLI описывает `--cli --build --full input.rson output.scr text.txt`.
- **CONFIRMED:** тестовый `LastOneHP.rson` скомпилирован в `.scr` размером 570 байт;
  SHA-256 результата `E739DEEB6CB1FCE7AF0EABD95BA3AD8D38645AA21D67CC9FBBCA6419DCDD370B`.
- **PARTIAL:** бинарник не подписан; источник и SHA-256 архива записаны в
  `references/README.md`.

## Доступные источники истины

- список функций: `references/Space-Rangers-Mods-Sources/script-functions/Script functions list.txt`;
- реализация/перечень функций: `references/Space-Rangers-Mods-Sources/script-functions/aScriptFun.pas`;
- оригинальная пиратская кампания: `references/Space-Rangers-Mods-Sources/OriginalScripts/PC_*.rson`;
- новая кастомная фракция: `references/Space-Rangers-Mods-Sources/AnotherMods/AMod_Invaders/Invaders (den).rson`;
- мод с гражданским трафиком: `references/Space-Rangers-Mods-Sources/AnotherMods/AMod_Merchant/Merchant.rson`.

## Подтверждённые механики для CE-P00

| Механика | Статус | Референс |
|---|---|---|
| одноразовый/глобальный запуск | CONFIRMED | `GRun`, `GCntRun`, `GLastTurnRun`, `IsScriptActive` в списке функций |
| NPC и скриптовая группа | CONFIRMED | TGroup/TState в `Invaders (den).rson`; `Buy*` API для нативных ролей |
| диалог | CONFIRMED | `Dialog(...)`, например `Invaders (den).rson:2431` |
| квестовый предмет | CONFIRMED | `CreateQuestItem`, `LinkItemToScript`, `AddItemToShip`, строки 1402–1405 того же файла |
| таймер | CONFIRMED | `CurTurn() + N`, строки 1428, 1457–1458 того же файла |
| очистка | CONFIRMED | `ShipDestroy`, строка 2550; `FreeItem`/`ItemDestroy` в референсах |
| save/load CE-переменных | PARTIAL | сериализация переменных скрипта существует; CE-схема ещё не проверена в игре |

## Invaders / Арксы

`Invaders (den).rson` доказывает кастомного владельца через
`StarCustomFaction(..., 'AMod_Invaders')`, кастомные корабли/предметы, таймеры,
диалоги и очистку. Это допустимый паттерн для технической фракции уровня MVP, но
не доказательство четырёх новых нативных рас.

## Lang.dat и ресурсы

- **CONFIRMED:** русский `Lang.dat` находится в `<GAME_ROOT>\CFG\Rus\Lang.dat`.
- **CONFIRMED:** встроенный `SR2PQuestStyle` подключает собственные
  `CFG\Rus\Lang.dat` и `CFG\Eng\Lang.dat` рядом с модулем.
- **CONFIRMED:** RScript при сборке экспортирует диалоги в отдельный txt и умеет
  встраивать их в секцию Script связанного Lang.
- **BLOCKED:** окончательный merge CE-Lang и упаковка `.pkg` ещё не проверены на
  отдельном тестовом модуле.

## Итог этапа

Можно переходить к безопасному каркасу и CE-P00 после создания локального
пакета-модуля. Полноразмерная вторая карта не подтверждена; см.
`SECOND_MAP_FEASIBILITY.md`. Никакой игровой код CE пока не объявляется рабочим.
