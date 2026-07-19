# DirectDraw wrapper evaluation for Space Rangers HD

## Проверенные готовые решения

Upstream `cnc-ddraw` 7.1 прямо включает оригинальный Space Rangers в список
поддерживаемых игр и предоставляет DirectDraw replacement с GDI/OpenGL/Direct3D
9 renderers, borderless/windowed режимом, Alt+Enter и screenshots. Поэтому он был
проверен первым, но Steam HD build `20648864` оказался другим случаем.

Альтернативы:

- DDrawCompat исправляет совместимость, но не переводит API и предупреждает о
  несовместимости с другими hooks/recorders;
- DxWrapper функционально шире, но включает не нужные здесь injection/patching
  возможности;
- dgVoodoo2 рассчитан на более широкий набор старых 2D/3D API и для этой задачи
  избыточен.

## Зафиксированный пакет

- upstream: `FunkyFr3sh/cnc-ddraw`;
- release: `7.1.0.0`, commit `541b5de`;
- ZIP SHA256: `0B13AB89A64C9918189B1DADD449EF6ED3CB3B7B19CABD96D8ADBD95505BB908`;
- `ddraw.dll` SHA256:
  `85E0F7D530DFDA134793A57CB3E76B0287DCC96892EE57162DD68F47283B03A9`;
- license: MIT.

Скачанный пакет хранится только в ignored `references/downloads/` и не входит в
репозиторий или мод.

## Результат на Steam HD build

После обратимой установки `ddraw.dll` рядом с `Rangers.exe`:

- процесс загрузил системный `d3d9.dll`;
- `cnc-ddraw` не создал лог и его `ddraw.dll` не появился в модулях процесса;
- Windows Graphics Capture вернул прежний `SetIsBorderRequired 0x80004002`;
- добавленные `ddraw.dll`/`ddraw.ini` полностью удалены;
- оригинальный `INSTALL.TXT` сохранил контрольный SHA256.

Вывод: поддержка оригинального Space Rangers не переносится автоматически на
Space Rangers HD: A War Apart. Графический wrapper не решает наблюдаемую ошибку.

## Реальная причина захвата

Система работает на Windows 10 build `19045`. Свойство WinRT
`GraphicsCaptureSession.IsBorderRequired` появилось только в build `20348`,
поэтому вызов helper завершается `E_NOINTERFACE (0x80004002)` до получения кадра.

Рабочий локальный обход — `ce_legacy_display_capture.exe` с GDI `BitBlt`, уже
подтвердивший полный кадр `1920x1080`. Штатный Windows Graphics Capture требует
более нового Windows build либо feature-detection в вызывающем helper.
