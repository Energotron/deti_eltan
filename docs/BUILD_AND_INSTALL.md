# Сборка и локальная установка

Статус: процедура подтверждена для компиляции отдельного `.rson` и изолированного
smoke-модуля; сборка полного игрового CE-модуля ещё не выполнялась.

## Требования

1. Игра Steam build `20648864`, `Rangers.exe 2.1.2500.0`.
2. RScript 4.10f из `references/tools/RScript_4.10f/`.
3. Python 3 для `tools/validate_content.py`.
4. LLVM-MinGW 20260407 для PE32 engine-adapter.
5. Все результаты сборки только в `dist/`.

## Проверка данных

```powershell
python tools/validate_content.py
python -m unittest discover -s tools/tests -v
python tools/second_map_spike.py demo dist/second-map-spike/demo-state.json `
  --old-stars 80 --old-sectors 19 --cells 10 --seed 2441
```

Временный debug-вход в офлайн-модель пропускает сборку Якоря и не расходует
резонансную ячейку:

```powershell
python tools/second_map_spike.py init dist/second-map-spike/preview-state.json `
  --old-stars 80 --old-sectors 19 --cells 0 --seed 2441
python tools/second_map_spike.py debug-open-second-home `
  dist/second-map-spike/preview-state.json
```

Это пока не игровая UI-кнопка: native multi-galaxy остаётся отключён, поэтому
игровой smoke-мод не может честно открыть отсутствующий объект второй карты.

## Сборка безопасного engine-adapter

```powershell
.\tools\build-engine-adapter.ps1
```

Скрипт собирает DLL как PE32/i386, проверяет архитектуру и запускает отдельный
32-битный smoke-host. Успешный тест не разрешает разыменование `GalaxyPtr()` и
не доказывает native multi-map; подробности в `SECOND_MAP_ADAPTER_ABI.md`.

## Компиляция одного скрипта

Документированный CLI-вызов:

```powershell
references\tools\RScript_4.10f\RScript.exe --cli --build --full `
  src\scripts\Mod_CE_Core.rson `
  dist\ChildrenOfEltan\DATA\Script\Mod_CE_Core.scr `
  dist\ChildrenOfEltan\CFG\Rus\Mod_CE_Core.txt
```

RScript является GUI-приложением и может завершать родительский процесс раньше,
чем физически появятся файлы. `tools/build.ps1` должен запускать его через
`Start-Process -Wait` и затем проверять наличие/размер результатов.

## Структура будущей сборки

```text
dist/ChildrenOfEltan/
  ModuleInfo.txt
  INSTALL_RUSSIAN.TXT
  DATA/CESecondMapAdapter.dll
  CFG/Rus/Lang.dat
  DATA/Script/Mod_CE_*.scr
```

Фактические имена путей Script и способ merge Lang должны быть подтверждены
smoke-модулем. Не копировать исходный `Lang.dat` или игровые `.pkg` в репозиторий.

## Установка

Диагностический smoke-модуль сначала запускается в безопасном режиме:

```powershell
.\tools\install-game-smoke.ps1 -GameRoot '<каталог Space Rangers HD>'
```

После проверки путей отдельный модуль можно скопировать явным флагом:

```powershell
.\tools\install-game-smoke.ps1 -GameRoot '<каталог Space Rangers HD>' -Install
```

Будущий `tools/install-local.ps1` обязан:

- требовать явный `-GameRoot` либо безопасно найти Steam AppID `214730`;
- по умолчанию работать как `-DryRun`;
- копировать только `dist/ChildrenOfEltan` в отдельный каталог мода;
- никогда не заменять оригинальные `DATA`, `CFG` или `Rangers.exe`;
- выводить полный список создаваемых файлов;
- перед реальной установкой проверять build игры.

## Ручной smoke-тест

1. Сделать резервную копию `%USERPROFILE%\Documents\SpaceRangersHD\Save`.
2. Установить только отдельный CE-модуль.
3. Включить мод в меню игры.
4. Создать новую игру, сохранить и загрузить её.
5. Проверить `%USERPROFILE%\Documents\SpaceRangersHD\########.log` и каталог
   `Errors` на строки `CE_`.
6. Вернуть лог и скриншот списка активных модулей.
