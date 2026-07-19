# Legacy DirectDraw capture bridge

## Зачем он нужен

`Rangers.exe` использует старый полноэкранный DirectDraw. Стандартный захват
отдельного окна через Windows Graphics Capture возвращает ошибку интерфейса, а
DXGI Desktop Duplication получает чёрный кадр эксклюзивной поверхности.

`ce_legacy_display_capture.exe` снимает текущий монитор двумя безопасными путями:

1. пробует DXGI Desktop Duplication;
2. распознаёт почти чёрный кадр и автоматически повторяет захват через GDI
   `BitBlt` с `CAPTUREBLT`.

Инструмент является внешним: он не открывает процесс игры, не внедряет DLL, не
ставит hooks и не изменяет файлы установки. Результат — локальный 32-bit BGRA
BMP без сетевой передачи.

## Сборка

```powershell
powershell -ExecutionPolicy Bypass -File tools\build-legacy-display-capture.ps1
```

## Один кадр

```powershell
dist\legacy-display-capture\ce_legacy_display_capture.exe `
  --output dist\legacy-display-capture\current.bmp
```

Дополнительные параметры:

- `--monitor N` — индекс монитора, по умолчанию `0`;
- `--timeout MS` — ожидание DXGI-кадра, по умолчанию `2000`;
- `--gdi-only` — сразу использовать совместимый с игрой GDI-путь.

В проверке на `Rangers.exe 2.1.2500.0` DXGI корректно распознан как чёрный, GDI
получил полный кадр `1920x1080` с загруженным игровым интерфейсом.
