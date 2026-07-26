#!/usr/bin/env python3
"""Turn an ordinary save into the Second Home arm.

Takes a normally-saved game as the base and rewrites everything the player
reads off the map into Children of Eltan naming: system names, planet names
and station names.

Why a normally-saved game and not one produced by the mod: saves written
mid-generation carry a half-built interface with them, which surfaces as a
hangar dialog drawn over the star map. Base this on a game that was saved the
ordinary way, from space, after the intro.

What is deliberately NOT touched:

  * races and owners. The Eltan peoples reuse the engine's five slots --
    Maloc reads as Strongs, Peleng as Agills, People as Mediums, Fei as
    Intells, Gaal as the Fifth Treaty menzols -- so the mapping is a reskin,
    not a data change. Reassigning owners here would only scramble the
    galaxy's politics.
  * NPC ship names. There are over a thousand and their names are generated
    per race, so they follow the reskin on their own.
  * planet models, star graphics and portraits. Those are graph_name /
    graph_object_type / face fields and are the visual pass, done separately.

Usage:
    python tools/make_second_home.py <base.sav> <out.sav> [--name TITLE]
"""

from __future__ import annotations

import argparse
import io
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from sav_tool import read_save  # noqa: E402


# Systems. Named places first -- these are the ones the story points at --
# then the forge and ash registers the survivors named their sky after.
SYSTEMS = [
    "Эльтанская Рана", "Сердце Пепла", "Первый Приют", "Ковчег-IV",
    "Крепость Карх", "Узел Без Лица", "Призма Единства", "Пепельный рынок",
    "Сиротское гнездо", "Город Незажжённых", "Врата Второго Дома",
    "Игла Айока", "Новая Айока", "Большая Цена", "Дом Пятого договора",
    "Люмен", "Горн", "Шлак", "Искра", "Зарево", "Пепел", "Сталь", "Латунь",
    "Окалина", "Клинок", "Молот", "Кузнец", "Горнило", "Тигель", "Плавка",
    "Литьё", "Сплав", "Закал", "Клеймо", "Оковы", "Засека", "Дозор",
    "Стража", "Караул", "Рубеж", "Порог", "Предел", "Грань", "Излом",
    "Разлом", "Раскол", "Шрам", "Ожог", "Пепелище", "Тлен", "Хмарь",
    "Морок", "Мгла", "Сумрак", "Полночь", "Затмение", "Немота", "Бездна",
    "Провал", "Утёс", "Гряда", "Хребет", "Терраса", "Уступ", "Ложбина",
    "Впадина", "Ущелье", "Каньон", "Затон", "Плёс", "Омут", "Каргаш",
    "Тарнов", "Эльтас", "Агилар", "Медиан", "Интель", "Клиссар", "Молотар",
    "Зарница", "Порубежье", "Приграничье", "Пустошь", "Окоём", "Первопуток",
    "Взгорье", "Низина", "Прогалина", "Урочище", "Чащоба", "Раздолье",
    "Застенок", "Заслон", "Отрог", "Кряж", "Даль", "Простор", "Ширь",
    "Захолустье", "Новоземье",
]

# Planets. Vanilla planet names are single invented words of wildly varying
# length, so these are built the same way from Eltan roots rather than as
# "adjective + noun" phrases, which read nothing like the original table.
_ROOTS = [
    "Карх", "Тарг", "Эльт", "Агилл", "Медиум", "Интелл", "Клисс", "Мензол",
    "Ош", "Рив", "Зорх", "Уннар", "Пельт", "Крих", "Манд", "Тэл", "Орх",
    "Иннар", "Гурт", "Велл", "Сантор", "Микд", "Аррач", "Шао", "Таррук",
    "Ийо", "Гтао", "Лиан", "Сера", "Ирша", "Аш", "Тео", "Журук", "Кера",
]
_TAILS = [
    "", "ан", "ор", "ис", "ай", "ун", "ей", "им", "ол", "ад", "ур", "эн",
    "ик", "ос", "ат", "ель", "ин", "ак", "ог", "уш",
]


def planet_names(count: int) -> list[str]:
    """Deterministic pool of single-word planet names, no repeats."""
    names: list[str] = []
    seen: set[str] = set()
    for tail in _TAILS:
        for root in _ROOTS:
            name = root + tail
            if name in seen:
                continue
            seen.add(name)
            names.append(name)
            if len(names) >= count:
                return names
    # Ran out of combinations: fall back to numbered variants rather than
    # repeating, since duplicate planet names look like a bug in play.
    index = 2
    while len(names) < count:
        for base in list(names):
            candidate = f"{base}-{index}"
            if candidate not in seen:
                seen.add(candidate)
                names.append(candidate)
                if len(names) >= count:
                    return names
        index += 1
    return names


# Stations, by engine type. Types 6..12 are bases; 0..4 are NPC ships and are
# left alone. The split was read off the base save: type 6 military, 7 pirate,
# 8 weapons, 9 science, 10 business, 11 medical, 12 the odd pair.
STATIONS = {
    6: ["Крепость Карха", "Молот Возвращения", "Наковальня", "Редут Кузнецов",
        "Бастион Пепла", "Застава Стронгов", "Форт Второго Восхода"],
    7: ["Серые Причалы", "Пепельный Капер", "Чёрная Паутина"],
    8: ["Верфь Молота", "Кузня", "Оружейня Карха", "Литейный Двор"],
    9: ["Тихая Формула", "Резонанс", "Сияющий Архив", "Контур Единства",
        "Решётка Эха"],
    10: ["Медный Реестр", "Долина Договоров", "Счётчик Долга",
         "Большая Сделка"],
    11: ["Санктум", "Ковчег Жизни", "Дом Первой Крови", "Тихий Покой",
         "Ясли Нового Рождения", "Приют Аррачи", "Последняя Цена"],
    12: ["Отражение", "Цитадель Молчания"],
}


def rewrite(save, title: str) -> dict[str, int]:
    data = save.data
    galaxy = data["data3"]
    counts = {"systems": 0, "planets": 0, "stations": 0}

    data["save_name"] = title

    stars = galaxy["stars"]
    for index, star in enumerate(stars):
        star["name"] = SYSTEMS[index % len(SYSTEMS)] if index >= len(SYSTEMS) \
            else SYSTEMS[index]
        counts["systems"] += 1

    total_planets = sum(len(star.get("planets") or []) for star in stars)
    pool = planet_names(total_planets)
    cursor = 0
    for star in stars:
        for planet in star.get("planets") or []:
            planet["name"] = pool[cursor]
            cursor += 1
            counts["planets"] += 1

    used: dict[int, int] = {}
    for star in stars:
        for entry in star.get("ships") or []:
            if not (isinstance(entry, tuple) and isinstance(entry[1], dict)):
                continue
            obj = entry[1]
            kind = obj.get("type")
            if kind not in STATIONS:
                continue
            pool_for_kind = STATIONS[kind]
            position = used.get(kind, 0)
            obj["name"] = pool_for_kind[position % len(pool_for_kind)]
            used[kind] = position + 1
            counts["stations"] += 1

    return counts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("--name", default="Второй Дом")
    args = parser.parse_args()

    save = read_save(args.base)
    counts = rewrite(save, args.name)

    noise = io.StringIO()
    stdout, sys.stdout = sys.stdout, noise
    try:
        save.to_file(args.out)
    finally:
        sys.stdout = stdout

    print(f"{args.base.name} -> {args.out.name} ({args.out.stat().st_size} bytes)")
    for key, value in counts.items():
        print(f"  {key}: {value}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
