# Карта подтверждённых референсов

Все пути относительны корня проекта. Номера строк относятся к ревизии
`172601f08fba353f71db89612e11cd25be86d622`.

| Операция CE | API / паттерн | Рабочий пример | Ограничение |
|---|---|---|---|
| запустить глобальный скрипт | `GRun()` | `references/Space-Rangers-Mods-Sources/Astro/LastOneHP.rson:75` | только из Global-кода |
| не повторять запуск | `GCntRun`, `GLastTurnRun`, `IsScriptActive` | `script-functions/Script functions list.txt:420` | имя скрипта должно совпадать с Main |
| получить игровой день | `CurTurn()` | `AnotherMods/AMod_Invaders/Invaders (den).rson:1428` | сохранять дедлайн, не пересчитывать после load |
| вызвать DLL сразу после load | подтверждённого callback нет | `docs/GALAXY_LAYOUT_ABI5_RESULTS.md` | `Turn` ждёт следующий день; `Init` при восстановлении save не вызывается |
| создать нативного рейнджера | `BuyRanger(planet, finance)` | список функций, строки 1404–1408 | корпус/раса выбираются нативно |
| создать пирата | `BuyPirate(planet, finance)` | список функций, строки 1409–1414 | зависит от владельца планеты |
| создать военного/флагман | `BuyWarrior`, `BuyBigWarrior` | список функций, строки 1415–1425 | нативные роли и владельцы |
| создать транспорт/лайнер/дипломата | `BuyTransport(planet,type,finance)` | список функций, строки 1426–1433 | типы `0/1/2` подтверждены |
| вернуть NPC к нативной логике | `ShipFreeFlight(ship)` | список функций, строки 1637–1642 | проверить после снятия стейта/OrderLock |
| переместить корабль/станцию | `TransferShip(ship,target)` | список функций, строки 1454–1459 | это мгновенный перенос, не переход карт |
| создать квестовый предмет | `CreateQuestItem` | `Invaders (den).rson:1402` | конкретный item tag должен существовать в данных |
| связать предмет со скриптом | `LinkItemToScript` | `Invaders (den).rson:1404` | нужна уникальная TItem-ссылка |
| положить предмет на корабль | `AddItemToShip` | `Invaders (den).rson:1405` | отдельно проверять свободный трюм |
| уничтожить предмет | `ItemDestroy` / `FreeItem` | `OriginalScripts/sr1/Script02.rson:703`; `Invaders (den).rson:1738` | `FreeItem` использовать только после извлечения |
| начать диалог | `Dialog(dialog,target)` | `Invaders (den).rson:2431` | цель должна существовать и быть доступна |
| удалить NPC | `ShipDestroy(ship,type)` | `Invaders (den).rson:2550` | тип эффекта проверить на CE-placeholder |
| назначить кастомную фракцию системе | `StarCustomFaction` | `Invaders (den).rson:1358` | требует иконку; неверные данные могут вызвать crash |
| владелец кастомной планеты | `PlanetCustomFaction` | список функций, строки 691–694 | UI незаселённых планет ограничен |
| создать чёрную дыру | `HoleCreate2(star1,star2)` | список функций, строки 3485–3489 | только между системами текущей галактики |
| проверить участие игрока в клане когда-либо | `ShipInPirateClan(Player())` | `OriginalScripts/PC_part0.rson:668` | не равно текущей стороне |
| проверить текущую сторону клана | `ShipOnSidePirateClan(Player())` | список функций, строки 1497–1498 | использовать вместе с историческим флагом |
| получить пиратский ранг | `GetShipPirateRank(Player())` | `OriginalScripts/PC_final.rson:2429` | диапазон 0–7 |
| получить исход пиратской сюжетки | `PirateWin()` | `OriginalScripts/PC_final.rson:1545` | значения 0–4; только чтение в CE |
| проверить поражение Коалиции | `CoalitionDefeated()` | `OriginalScripts/PC_final.rson:2373` | только чтение в CE |
| число систем стороны | `ControlledSystems(side)` | `OriginalScripts/PC_final.rson:2905` | 0 Коалиция, 1 доминаторы, 2 пираты |
| получить указатель текущей Galaxy | `GalaxyPtr()` | `script-functions/aScriptFun.pas:14408-14411` | только непрозрачный 32-битный адрес до проверки layout |
| подключить функцию DLL | `ScriptLibs`, `ImportedFunction`, `ImportAll` | `aScriptFun.pas:14387-14405`; список функций, строки 3878–3902 | ABI DLL должен быть PE32/cdecl и совпадать с Main |

## Неподтверждённые операции

Для следующих операций нет документированного API в имеющемся списке функций:

- создание новой звёздной карты или второго объекта Galaxy;
- переключение `GalaxyPtr` на другой объект;
- сериализация двух нативных галактик в одном `.sav`;
- создание новой нативной расы/владельца сверх custom-faction слоя;
- динамическое создание полноценной станции/базы через `BuyBase`/`CreateBase`.

Для native multi-map по-прежнему использовать `TODO: VERIFY_ENGINE_LAYOUT`.
Поддержка DLL как транспорта подтверждена отдельно и не доказывает возможность
создания, переключения или сохранения второй Galaxy.
