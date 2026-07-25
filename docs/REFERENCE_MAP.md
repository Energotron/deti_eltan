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
| создать квестовый груз в обычном трюме | `CreateQuestItem(name, owner)` | `script-functions/aScriptFun.pas:8079-8089`; `Solyanka/GTLBHGadgets/Mod_GTLBHGadgets.rson:293` | создаёт `TUselessItem`, а не устанавливаемый артефакт; item tag должен существовать в `Lang.dat/UselessItems` |
| связать предмет со скриптом | `LinkItemToScript` | `Invaders (den).rson:1404` | нужна уникальная TItem-ссылка |
| положить предмет на корабль | `AddItemToShip` | `Invaders (den).rson:1405` | отдельно проверять свободный трюм |
| запретить выброс квестового груза | `NoDropItem(item,1)` | `script-functions/aScriptFun.pas:9975-9993` | не заменяет проверку продажи |
| активировать квестовый груз из трюма | `UselessItems.<id>.OnUseCode` | `Solyanka/ShuQuad/Lang_ShuQuad.txt:142-153`; `Solyanka/GTLBHGadgets/Lang_Rus_GTLBHGadgets.txt:69-89` | код хранится в Lang; предмет остаётся `TUselessItem` |
| уничтожить предмет | `ItemDestroy` / `FreeItem` | `OriginalScripts/sr1/Script02.rson:703`; `Invaders (den).rson:1738` | `FreeItem` использовать только после извлечения |
| начать диалог | `Dialog(dialog,target)` | `Invaders (den).rson:2431` | цель должна существовать и быть доступна |
| удалить NPC | `ShipDestroy(ship,type)` | `Invaders (den).rson:2550` | тип эффекта проверить на CE-placeholder |
| назначить кастомную фракцию системе | `StarCustomFaction` | `Invaders (den).rson:1358` | требует иконку; неверные данные могут вызвать crash |
| владелец кастомной планеты | `PlanetCustomFaction` | список функций, строки 691–694 | UI незаселённых планет ограничен |
| создать чёрную дыру | `HoleCreate2(star1,star2)` | список функций, строки 3485–3489 | только между системами текущей галактики |
| мгновенно повторить UX Субпортала | `OnUseCodeBlackHole(CreateArt(t_ArtBlackHole,None))` | `script-functions/aScriptFun.pas:12966-13045` | временный Subportal уничтожается; созданная дыра всё ещё принадлежит текущей `Galaxy`, поэтому межрукавное состояние переключает адаптер |
| сменить систему выхода созданной ЧД | `HoleStar2(hole,star)` | `script-functions/aScriptFun.pas:7934-7943` | координаты выхода остаются уже сгенерированными в новой системе |
| проверить участие игрока в клане когда-либо | `ShipInPirateClan(Player())` | `OriginalScripts/PC_part0.rson:668` | не равно текущей стороне |
| проверить текущую сторону клана | `ShipOnSidePirateClan(Player())` | список функций, строки 1497–1498 | использовать вместе с историческим флагом |
| получить пиратский ранг | `GetShipPirateRank(Player())` | `OriginalScripts/PC_final.rson:2429` | диапазон 0–7 |
| получить исход пиратской сюжетки | `PirateWin()` | `OriginalScripts/PC_final.rson:1545` | значения 0–4; только чтение в CE |
| проверить поражение Коалиции | `CoalitionDefeated()` | `OriginalScripts/PC_final.rson:2373` | только чтение в CE |
| число систем стороны | `ControlledSystems(side)` | `OriginalScripts/PC_final.rson:2905` | 0 Коалиция, 1 доминаторы, 2 пираты |
| получить указатель текущей Galaxy | `GalaxyPtr()` | `script-functions/aScriptFun.pas:14408-14411` | только непрозрачный 32-битный адрес до проверки layout |
| подключить функцию DLL | `ScriptLibs`, `ImportedFunction`, `ImportAll` | `aScriptFun.pas:14387-14405`; список функций, строки 3878–3902 | ABI DLL должен быть PE32/cdecl и совпадать с Main |
| снять полный нативный снимок Galaxy | хук после `TGalaxy.SaveToStream`, RVA `0x43a1a4` | `src/engine_adapter/ce_second_map_adapter.c`; живой тест ABI 10 | только Steam build 2.1.2500; прямой вызов из `Turn` запрещён |
| преобразовать карту после штатной загрузки | хук после `TGalaxy.LoadFromStream`, RVA `0x43b6cc` | `src/engine_adapter/ce_second_map_adapter.c`; живой тест ABI 11, 73 `TCon` | исследовательский one-shot; это ещё не одновременные две Galaxy |
| сохранить всю партию штатным lifecycle | `SaveGame`, VA `0x006008f8`: `eax`=полный путь, `edx`=заголовок, результат в `AL` | прямые вызовы `0x0050acbf`, `0x0052a1a7`, `0x0079e478`, `0x007a2f9e`, `0x007a3119`; `tools/find_pe_string_xrefs.py` | сериализация синхронна, запись файла заканчивает штатный writer thread; вызывать из нативного save/new-game контекста, не из `Turn` |
| загрузить всю партию штатным lifecycle | `LoadGame`, VA `0x00601150`: `eax`=полный путь, результат в `AL` | `TThreadGameLoad.Execute`, VA `0x0053c954`, прямой вызов loader в `0x0053c98a` | удаляет и пересоздаёт `Galaxy`, игрока и глобальные реестры; это требуемая замена live-swap одного указателя |
| построить полноценную новую партию | `TThreadCreateNewGame.Execute`, VA `0x005d36d8`, `eax`=Self | VMT `TThreadCreateNewGame` у `0x005d36ac`; живой результат 20 секторов / 73 системы | безопасно только в штатном new-game worker; вызов посреди партии заменяет игрока и ломает сессию |

## Внешние инструменты, перепроверенные 22 июля 2026

- [Инструментарий](https://rangers.fandom.com/ru/wiki/Инструментарий) — сводная
  страница редакторов и утилит сообщества.
- [RScript](https://rangers.fandom.com/ru/wiki/RScript) — основной редактор,
  компилятор/декомпилятор и CLI для игровых скриптов; публичного API замены
  `Galaxy` или загрузки второй карты на странице не описано.
- [Space Rangers Universe](https://rangers.fandom.com/ru/wiki/Space_Rangers_Universe)
  — исследовательский открытый проект по КР; упомянутый исходный репозиторий
  сейчас не дал доступного кода, пригодного как подтверждение ABI HD-движка.
- [Cassandra](https://github.com/indiemagpie/Cassandra) — актуальный редактор
  сохранений, полезный для независимой проверки `.sav`; опубликованный
  репозиторий содержит готовые бинарные релизы, но не дал исходного описания
  формата, которое можно было бы безопасно встроить вместо engine lifecycle.

## Неподтверждённые операции

Для следующих операций нет документированного API в имеющемся списке функций:

- создание новой звёздной карты **посреди уже идущей партии**;
- переключение `GalaxyPtr` на другой объект;
- сериализация двух нативных галактик в одном `.sav`;
- создание новой нативной расы/владельца сверх custom-faction слоя;
- динамическое создание полноценной станции/базы через `BuyBase`/`CreateBase`.

Полная новая карта теперь подтверждена только внутри штатного цикла создания
партии. Целевой native multi-map использует две полные save-сущности и штатный
`LoadGame`; постоянный live-swap двух `TGalaxy*` признан несовместимым с
отдельными ссылками игрока, форм и глобальных реестров.
