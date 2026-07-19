# ABI адаптера второй карты

## Назначение

`CESecondMapAdapter.dll` — 32-битный C-адаптер между RScript и будущим
build-specific слоем второй галактики. ABI 3 подтверждён внутри Steam build
`20648864`: игра загрузила DLL, RScript вызвал smoke-экспорт, передал ненулевой
`GalaxyPtr()` и выполнил ограниченный read-only fingerprint. ABI 4 добавил
сравнимый layout sample без сырых значений или адресов. ABI 5 добавил rolling
latest-наблюдение по подтверждённому `CurTurn()`. ABI 6 добавляет hashes локальной
копии, в которой dword, классифицированные как readable pointers, заменены нулями.

## Подтверждённая цепочка

1. RScript возвращает текущий `Galaxy` как 32-битный `dword` через `GalaxyPtr()`.
2. `ScriptLibs` в `Main.dat` может объявить C-функции DLL.
3. `ImportedFunction()` и `ImportAll()` связывают эти функции со скриптом.
4. Адаптер собирается как PE32/i386 и проходит отдельный x86 smoke-host.

Основания: `aScriptFun.pas:14387-14413`, `aScriptFun.pas:15247-15250` и
`Script functions list.txt:3804,3819-3829,3878-3902` в зафиксированном
репозитории референсов.

## Экспорты ABI 6

| Экспорт | Контракт |
|---|---|
| `CEAdapterAbiVersion()` | возвращает `6` |
| `CEAdapterCapabilities()` | bind, smoke, fingerprint, sample, rolling latest и normalized hash (`497`) |
| `CEAdapterBindGalaxy(dword)` | принимает ненулевой непрозрачный адрес и сохраняет его |
| `CEAdapterGetBoundGalaxy()` | возвращает последний сохранённый адрес |
| `CEAdapterEchoDword(dword)` | безопасный smoke-вызов без доступа к игре |
| `CEAdapterRunSmoke(dword,dword)` | bind и атомарная запись диагностического маркера |
| `CEAdapterProbeGalaxy(dword,dword)` | проверка региона и чтение 16–256 байт через `ReadProcessMemory` |
| `CEAdapterGetLastFingerprintHash()` | последний FNV-1a хеш без выдачи сырых данных |
| `CEAdapterGetLastFingerprintBytes()` | размер последней успешной выборки |
| `CEAdapterSampleGalaxyLayout(dword,dword)` | один 256-байтный sample на процесс и append в JSONL |
| `CEAdapterObserveGalaxyLayout(dword,dword,dword)` | перезаписывает latest для нового `CurTurn` |
| `CEAdapterGetLayoutObservationCount()` | число уникальных ходов, увиденных процессом |
| `CEAdapterGetLayoutBlockHash(dword)` | FNV-1a одного из четырёх 64-байтных блоков |
| `CEAdapterGetLayoutNormalizedBlockHash(dword)` | FNV-1a блока после обнуления pointer-class dword в копии |
| `CEAdapterGetLayoutZeroMaskLow/High()` | 64-битная маска нулевых dword двумя половинами |
| `CEAdapterGetLayoutReadablePointerMaskLow/High()` | маска dword, похожих на читаемые выровненные указатели |
| `CEAdapterGetLayoutSampleBytes()` | `256` после успешного sample |
| `CEAdapterSupportsNativeMultiGalaxy()` | возвращает `0` |

Все функции используют `cdecl`, фиксированные 32-битные типы и не владеют
переданным объектом Galaxy.

## Ограничения layout sampler

- перед чтением весь диапазон проверяется через `VirtualQuery`;
- копирование выполняется через `ReadProcessMemory(GetCurrentProcess())`;
- размер жёстко ограничен первыми 256 байтами;
- повтор одного `CurTurn` не создаёт нового наблюдения;
- rolling-файл перезаписывается и не растёт со временем;
- JSONL не содержит сырые байты, `GalaxyPtr` или адрес региона;
- `pointer_normalized_fnv1a32` не меняет память игры и содержит только hashes;
- readable-pointer mask является только классификацией, а не доказательством поля;
- четыре 64-байтных FNV-1a хеша нужны только для межпроцессного сравнения.

Файл: `%TEMP%\ChildrenOfEltan\galaxy-layout-samples.jsonl`. Сравнение:

```powershell
python tools\game_smoke_check.py layouts --minimum 3
```

ABI 5 rolling-файл: `%TEMP%\ChildrenOfEltan\galaxy-layout-latest.json`.
Контрольные точки архивируются командой `capture`, которая умеет проверять
ожидаемые PID и ход.

## Запреты до завершения reverse engineering

- не обращаться к предполагаемым полям Galaxy напрямую;
- не менять глобальную переменную `Galaxy`;
- не патчить `Rangers.exe`;
- не создавать второй объект Galaxy по предполагаемому layout;
- не заявлять поддержку native save или native switch;
- не включать `CESecondMapAdapter.Main.fragment.txt` в релизный `Main.dat` до
  ручного smoke-теста на `Rangers.exe 2.1.2500.0`.

## Результат in-game smoke

- `CESecondMapAdapter.dll` присутствовал в списке модулей `Rangers.exe`;
- `%TEMP%\ChildrenOfEltan\adapter-smoke.json` подтвердил ABI 3, capability 49,
  контрольный marker и ненулевой `GalaxyPtr()`;
- анализатор вернул `PASS` без подозрительных строк в новых логах;
- read-only probe подтвердил `MEM_COMMIT`, `PAGE_READWRITE`, `MEM_PRIVATE`,
  регион 139264 байта и 64-байтный fingerprint без записи в память;
- игра была штатно закрыта, диагностический мод отключён;
- корневой `INSTALL.TXT` восстановлен побайтно до исходного SHA256
  `A2E1A160662E1B07EC5C519345369C243553887BC53D4D16F3CFFBF01E93C77B`.

## Результат ABI 4 in-game

Три новые игры в трёх отдельных процессах успешно создали валидные read-only
образцы. Блок 128–191 имел одинаковый хеш во всех трёх запусках; zero-mask была
полностью стабильна, pointer-классификация менялась только на offset 92. Полные
offsets и ограничения вывода записаны в `GALAXY_LAYOUT_ABI4_RESULTS.md`.

Следующий gate — сравнение одной партии до хода и после save/load. Запись в память
остаётся запрещённой до подтверждения layout.

## Результат ABI 5 in-game

Сравнение `CurTurn 300 -> 301` в одном процессе прошло: все четыре блока
изменились, zero-mask и pointer-class mask сохранились. Same-turn reload gate
заблокирован жизненным циклом RScript: ни `Turn`, ни проверенный `Init` не вызвали
DLL после открытия save в новом процессе. Детали и защита от stale PID записаны в
`GALAXY_LAYOUT_ABI5_RESULTS.md`.

## ABI 6 gate

ABI 5 attach reload сохранил zero/pointer masks, но не сохранил ни один raw
64-байтный hash между процессами. ABI 6 вычисляет второй набор hashes после
обнуления всех значений, классифицированных как readable pointers, исключительно
в 256-байтной локальной копии. DLL и внешний scanner используют один алгоритм.
Offline x86 host пройден; in-game turn/save/load цикл ещё требуется.
