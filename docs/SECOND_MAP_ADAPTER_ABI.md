# ABI адаптера второй карты

## Назначение

`CESecondMapAdapter.dll` — 32-битный C-адаптер между RScript и будущим
build-specific слоем второй галактики. Текущая версия ABI 3 подтверждена внутри
Steam build `20648864`: игра загрузила DLL, RScript вызвал smoke-экспорт и передал
ненулевой непрозрачный `GalaxyPtr()` без чтения памяти игры.

## Подтверждённая цепочка

1. RScript возвращает текущий `Galaxy` как 32-битный `dword` через `GalaxyPtr()`.
2. `ScriptLibs` в `Main.dat` может объявить C-функции DLL.
3. `ImportedFunction()` и `ImportAll()` связывают эти функции со скриптом.
4. Адаптер собирается как PE32/i386 и проходит отдельный x86 smoke-host.

Основания: `aScriptFun.pas:14387-14413`, `aScriptFun.pas:15247-15250` и
`Script functions list.txt:3804,3819-3829,3878-3902` в зафиксированном
репозитории референсов.

## Экспорты ABI 3

| Экспорт | Контракт |
|---|---|
| `CEAdapterAbiVersion()` | возвращает `3` |
| `CEAdapterCapabilities()` | bind, smoke-маркер и read-only fingerprint |
| `CEAdapterBindGalaxy(dword)` | принимает ненулевой непрозрачный адрес и сохраняет его |
| `CEAdapterGetBoundGalaxy()` | возвращает последний сохранённый адрес |
| `CEAdapterEchoDword(dword)` | безопасный smoke-вызов без доступа к игре |
| `CEAdapterRunSmoke(dword,dword)` | bind и атомарная запись диагностического маркера |
| `CEAdapterProbeGalaxy(dword,dword)` | проверка региона и чтение 16–256 байт через `ReadProcessMemory` |
| `CEAdapterGetLastFingerprintHash()` | последний FNV-1a хеш без выдачи сырых данных |
| `CEAdapterGetLastFingerprintBytes()` | размер последней успешной выборки |
| `CEAdapterSupportsNativeMultiGalaxy()` | возвращает `0` |

Все функции используют `cdecl`, фиксированные 32-битные типы и не владеют
переданным объектом Galaxy.

## Запреты до reverse engineering

- не разыменовывать `GalaxyPtr()`;
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

## Следующий безопасный gate

In-game fingerprint пройден. Следующий этап — искать стабильные read-only
инварианты и указатели внутри первых полей `Galaxy`, сравнивая несколько новых игр
и save/load. Запись в память остаётся запрещённой до подтверждения layout.
