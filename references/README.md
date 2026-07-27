# Локальные референсы

Эта папка предназначена только для локального исследования. Содержимое из неё не
включается в сборку и не распространяется вместе с модом.

## Загружено 2026-07-19

| Каталог | Источник | Ревизия / хеш | Назначение |
|---|---|---|---|
| `Space-Rangers-Mods-Sources/` | https://github.com/denballakh/Space-Rangers-Mods-Sources | commit `172601f08fba353f71db89612e11cd25be86d622` | исходники модов, оригинальные PC-скрипты, список функций |
| `OpenSR/` | https://github.com/ObKo/OpenSR | branch `rework`, commit `6050ca285adda5b462690b3898c084ba91edb3f7` | справочник форматов старой «Перезагрузки»; не доказательство для HD |
| `tools/RScript_4.10f.zip` | Wayback-копия `https://vertix.games/tools/RScript_4.10f.zip` | SHA-256 `E98E2EBD9102D648C744DCB40DA04FC94B00C133BA7C2DF86F58F5AA04C35850` | RScript 4.10f и встроенный BlockPar |
| `tools/llvm-mingw-20260407-ucrt-x86_64.zip` | https://github.com/mstorsjo/llvm-mingw/releases/tag/20260407 | SHA-256 `3FC6E54B5F1102089D4D37095BA49F7B24E22290DA78178B514A86B3126C6D9E` | portable toolchain для 32-битной DLL; не устанавливается в систему |
| `sr-mods-aggregator/` (по запросу, локально) | https://github.com/ArtYudin89/sr-mods-aggregator + датасет HF `Artyudin/sr-mods-assets` | живой источник, ветка `main`; версия каждого мода — поле `version` в его дескрипторе | каталог 694 модов HD: метаданные, декомпилированные RSON-исходники, побайтовые блоба кода и ассетов |

Репозиторий Universe, на который ссылается вики, во время аудита отвечал `404`
для `git clone`. Нужный список функций и исходники доступны во втором репозитории.

## Агрегатор модов: как достать конкретный файл

Публичный, без токена и без `git clone` — оба конца (GitHub raw и HF) отдают по
HTTPS анонимно. Ничего из этого в репозиторий не тащим: `references/` не
коммитится, а блоба — чужой контент.

1. **Каталог.** `https://raw.githubusercontent.com/ArtYudin89/sr-mods-aggregator/main/descriptors/catalog.json`
   — `schema: srmod-catalog/2`, `mods[<id>]` = мод, у каждого `variants[]` с
   полями `source`, `version` и `path` (путь дескриптора в том же репозитории).
   `id` — логический, вида `Evolution/EvoAmmoBox`; один и тот же мод обычно
   существует в нескольких паках (Redux, Universe), отсюда варианты.
2. **Дескриптор мода.** Берём `path` варианта и тянем его тем же raw-URL:
   `…/main/descriptors/<source>/<Group>/<Name>.json`. Внутри — `depends`,
   `conflicts`, `cosmetic`, `chunk_index_url` и, главное, `files`: две группы,
   `code` и `assets`, каждая — путь установки (`{app}/Mods/…`) → `{sha256, size, mtime}`.
   Пути описывают установленный мод, поэтому дескриптор один читается как
   полная опись модуля, даже если ни один байт не скачан.
3. **Блоб по sha256.** `chunk_index_url` из дескриптора указывает на
   `https://huggingface.co/datasets/Artyudin/sr-mods-assets/resolve/main/asset_index.json`
   (та же карта лежит и в git — `state/asset_index.json`, но каноничен HF-экземпляр).
   В нём `blobs[<sha256>] = {chunk, size}` и `chunks[<имя чанка>] = {url, store, group, blob_count}`.
   Качаем `chunks[…].url` — это zip; запись внутри архива названа самим `<sha256>`,
   без расширения и без каталогов. Хранение content-addressed: одинаковый файл
   из десяти модов лежит одним блобом.

То есть: `catalog.json` → дескриптор → `sha256` нужного файла → `asset_index.json`
→ URL чанка → запись `<sha256>` в zip. Мелкие исходники (`.scr`, `CFG`, RSON после
декомпиляции) лежат прямо в git агрегатора под `mods/<camp>/<unit>/`, их можно
брать raw-URL без обращения к HF.

Чем это полезно здесь: 694 мода как корпус для аудита — чем именно чужие моды
правят `CFG/Main.dat` и `CacheData.dat`, как выглядит рабочий `ModuleInfo.txt`,
какие `.gi` реально встречаются в HD. Ровно тот материал, на котором проверяются
`tools/srdecompile.py`, `tools/srblockpar.py`, `tools/srgi.py` и `tools/srpkg.py`.

⚠️ Чужие моды остаются чужими: как референс — да, копировать код или ресурсы в
`dist/` без разрешения авторов — нет (то же ограничение, что у
`Space-Rangers-Mods-Sources` ниже).

## Ограничения

- У `Space-Rangers-Mods-Sources` в корне не найден файл лицензии. Использовать как
  референс; не копировать код или ресурсы в релиз без разрешения автора.
- `RScript.exe` не имеет цифровой подписи. Архив сохранён вместе с хешем и не
  должен попадать в `dist/`.
- LLVM-MinGW распространяется отдельно от CE; в `dist/` попадает только собранная
  DLL адаптера.
- Ресурсы локальной игры не копируются сюда.
