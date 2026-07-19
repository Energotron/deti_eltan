# Read-only аудит состояния War Apart

Источник: оригинальные `PC_*.rson` и актуальный список функций из локальных
референсов. CE не должен вызывать setter-варианты этих функций.

## Подтверждённые сигналы

| Сигнал | Read-only выражение | Значение |
|---|---|---|
| пиратская линия / исход | `PirateWin()` | `0` война продолжается; `1` пираты во власти; `2` анархический пиратский исход; `3` Коалиция спасена; `4` неудачи пиратов |
| игрок когда-либо вступал | `ShipInPirateClan(Player())` | bool, исторический признак для игрока |
| игрок сейчас на стороне клана | `ShipOnSidePirateClan(Player())` | bool |
| текущий пиратский ранг | `GetShipPirateRank(Player())` | `0..7`: Салага…Барон |
| очки рейтинга | `ShipPirateRankPoints(Player())` | int; читать только при необходимости |
| пиратские системы | `ControlledSystems(2)` | текущее количество систем |
| ключевая планета | `PlanetPirateClan()` | Роджерия |
| владелец Роджерии | `PlanetOwner(PlanetPirateClan())` | дополнительный сигнал состояния |
| поражение Коалиции | `CoalitionDefeated()` | bool |
| активность PC-скрипта | `IsScriptActive('PC_partN')` | вспомогательный сигнал, имя проверить в `Main.dat` |

## Предлагаемая read-only классификация

Порядок важен:

```text
snapshot.playerEverJoined = ShipInPirateClan(Player())
snapshot.playerCurrentlyClan = ShipOnSidePirateClan(Player())
snapshot.playerRank = GetShipPirateRank(Player())
snapshot.pirateWin = PirateWin()
snapshot.pirateSystems = ControlledSystems(2)
snapshot.coalitionDefeated = CoalitionDefeated()

if playerCurrentlyClan or (playerEverJoined and playerRank > 0): PLAYER_PIRATE overlay
if pirateWin in [1, 2]: PIRATE_VICTORY
else if pirateWin in [3, 4]: COALITION_VICTORY
else if pirateWin == 0 and (pirateSystems > 0 or PC script active): CLAN_ACTIVE
else: NOT_STARTED or UNKNOWN
```

`PLAYER_PIRATE` лучше хранить как отдельный overlay, а не взаимоисключающее
значение: бывший пират может существовать одновременно с победой Коалиции.

## Нерешённая граница NOT_STARTED / CLAN_ACTIVE

`PirateWin()==0` означает и начальный статус сюжетки, и продолжающуюся войну.
Для точного разделения нужно проверить наличие/активность `PC_part0` и связанные
квестовые Ether в четырёх контрольных сохранениях. До этого:

- если сигналы противоречат друг другу — `UNKNOWN`;
- использовать нейтральную ветку разрозненных капитанов;
- записывать полный debug snapshot;
- не менять `PirateWin`, `CoalitionDefeated`, ранг, владельца Роджерии или системы.

## Что не использовать напрямую

Локальные переменные `clan_win_choice`, `domic_win_choice`, `end_type` и
`pirate_win_type` из `PC_final.rson` не объявлены Global и не являются надёжным
публичным API. Глобальный `pirate_planet_unlocked` также не нужен для основной
классификации без проверки доступа между скриптами.

## Требуемый spike

CE-SPIKE-AWA должен вывести только read-only значения для сохранений:

1. сюжетка не начата;
2. клан активен;
3. победа Коалиции (`PirateWin` 3 или 4);
4. победа пиратов (`PirateWin` 1 или 2);
5. игрок вступал, но сейчас не в клане;
6. противоречивое/неизвестное состояние.

Статус: **PARTIAL, TODO: VERIFY_IN_GAME**.
