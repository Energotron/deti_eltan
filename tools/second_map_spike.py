#!/usr/bin/env python3
"""Offline CE-SPIKE-MAP state model.

This proves deterministic two-arm state, inactive-arm batch simulation and an
idempotent transit transaction. It does not patch Rangers.exe or claim that a
second native Galaxy is already available in game.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from uuid import uuid4


SCHEMA_VERSION = 11
ARMS = ("OLD_ARM", "SECOND_HOME")
DATA_ROOT = Path(__file__).resolve().parents[1] / "data"
MAP_SCHEMA = json.loads(
    (DATA_ROOT / "second_home_map.schema.json").read_text(
        encoding="utf-8"
    )
)
MISSION_DEFINITIONS = json.loads(
    (DATA_ROOT / "missions.json").read_text(encoding="utf-8")
)
MISSION_BY_ID = {mission["id"]: mission for mission in MISSION_DEFINITIONS}
SECTOR_ARCHETYPES = tuple(MAP_SCHEMA["sector_archetypes"])
SECTOR_NAME_POOL = tuple(MAP_SCHEMA["sector_name_pool"])
SYSTEM_NAME_POOL = tuple(MAP_SCHEMA["system_name_pool"])
REQUIRED_NODES = tuple(MAP_SCHEMA["required_nodes"])
STORY_NODE_TITLES = MAP_SCHEMA["display_names"]["story_nodes"]
STORY_NODE_ARCHETYPES = MAP_SCHEMA["story_node_preferred_archetypes"]
CAPITAL_STORY_NODES = MAP_SCHEMA["capital_story_nodes"]
LOCKED_SECTOR_BRIEFINGS = MAP_SCHEMA["locked_sector_briefings"]
EARLY_STORY_NODES = tuple(MAP_SCHEMA["story_progression"]["early_story_nodes"])
STARTING_SECTOR_COUNT = MAP_SCHEMA["story_progression"]["starting_open_sector_count"]
MIN_LATE_STORY_DEPTH = MAP_SCHEMA["story_progression"]["minimum_late_story_map_purchases"]
STORY_DECK_STAGES = tuple(
    (stage["id"], tuple(stage["node_ids"]))
    for stage in MAP_SCHEMA["story_progression"]["deck_stages"]
)
DISCOVERY_RULE = MAP_SCHEMA["sector_discovery_rule"]
POPULATION_RULE = MAP_SCHEMA["system_population_rule"]
PIRATE_MIGRATION_RULE = MAP_SCHEMA["interarm_pirate_migration"]
DOMINATOR_INVASION_RULE = MAP_SCHEMA["interarm_dominator_invasions"]
DOMINATOR_BOSS_STATES = ("ACTIVE", "ELIMINATED")
WAR_STATE_RULE = MAP_SCHEMA["war_state_model"]
STRATEGIC_SYSTEM_RULE = MAP_SCHEMA["strategic_system_rule"]
DOMINATOR_SERIES = tuple(DOMINATOR_INVASION_RULE["series"])
RACES = ("STRONG", "AGILL", "MEDIUM", "INTELL")
WAR_APART_STATES = (
    "UNKNOWN",
    "NOT_STARTED",
    "CLAN_ACTIVE",
    "COALITION_VICTORY",
    "PIRATE_VICTORY",
    "PLAYER_PIRATE",
)
CORE_FACTION_BY_ARCHETYPE = {
    "STRONG": "CE_FACTION_STRONG",
    "AGILL": "CE_FACTION_AGILL",
    "MEDIUM": "CE_FACTION_MEDIUM",
    "INTELL": "CE_FACTION_INTELL",
}
MIN_SECTOR_COUNT = max(STARTING_SECTOR_COUNT + len(SECTOR_ARCHETYPES) + 1, sum(
    2 if any(STORY_NODE_ARCHETYPES[node] == archetype for node in EARLY_STORY_NODES) and
    any(STORY_NODE_ARCHETYPES[node] == archetype for node in set(REQUIRED_NODES) - set(EARLY_STORY_NODES))
    else 1
    for archetype in SECTOR_ARCHETYPES
))
TRANSIT_PHASES = ("PREPARED", "DEBITED", "SWITCHED")


def _pirate_snapshot_from_outcome(outcome: str) -> tuple[str, bool]:
    if outcome == "UNKNOWN":
        return "UNKNOWN", False
    if outcome == "COALITION_VICTORY":
        return "NO", False
    return "YES", outcome == "PLAYER_PIRATE"


class StateError(ValueError):
    pass


class SimulatedCrash(RuntimeError):
    pass


def _stable_int(*parts: object) -> int:
    payload = "|".join(str(part) for part in parts).encode("utf-8")
    return int.from_bytes(hashlib.sha256(payload).digest()[:8], "little")


def _other_arm(arm: str) -> str:
    if arm not in ARMS:
        raise StateError(f"unknown arm: {arm}")
    return ARMS[1] if arm == ARMS[0] else ARMS[0]


@dataclass(frozen=True)
class StateStore:
    path: Path

    def load(self) -> dict[str, Any]:
        return migrate_state(json.loads(self.path.read_text(encoding="utf-8")))

    def save(self, state: dict[str, Any]) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        temp = self.path.with_name(f".{self.path.name}.{uuid4().hex}.tmp")
        temp.write_text(
            json.dumps(state, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        os.replace(temp, self.path)


def migrate_state(state: dict[str, Any]) -> dict[str, Any]:
    """Upgrade the previous spike save without reviving defeated factions."""
    version = state.get("schema_version")
    if version == SCHEMA_VERSION:
        return state
    if version != 10:
        raise StateError("unsupported schema version")
    second = state.get("maps", {}).get("SECOND_HOME", {})
    for system in second.get("systems", []):
        controller = system.get("controller", "CE_UNCLAIMED")
        system["owner"] = controller
        system["system_state"] = (
            "KLISSAN_INFESTED" if system.get("condition") == "INFESTED"
            else "CONTROLLED"
        )
        military_owner = controller in set(CORE_FACTION_BY_ARCHETYPE.values()) | {
            "CE_FACTION_PIRATES", "CE_DOMINATOR_BLAZEROIDS",
            "CE_DOMINATOR_KELLEROIDS", "CE_DOMINATOR_TERRONOIDS",
        }
        system["military_production_enabled"] = military_owner
        system["strategic_expansion_enabled"] = military_owner
        system["last_capture_day"] = 0
    migration = state.get("pirate_migration", {})
    clan_state, player_pirate = _pirate_snapshot_from_outcome(state.get("war_apart_state"))
    state.setdefault("pirate_clan_exists_at_corridor", clan_state)
    state.setdefault("player_pirate", player_pirate)
    migration["eligible"] = state["pirate_clan_exists_at_corridor"] == "YES"
    migration["status"] = {
        "LOCKED": "INELIGIBLE",
        "SCOUTING": "STEALING_TECH",
        "EXTINCT": "INELIGIBLE",
        "UNRESOLVED": "INELIGIBLE",
    }.get(migration.get("status"), migration.get("status"))
    for series, invasion in state.get("dominator_invasions", {}).items():
        invasion["eligible"] = state.get("dominator_boss_states", {}).get(series) == "ACTIVE"
        invasion["status"] = {
            "DORMANT": "INELIGIBLE",
            "EXTINCT": "INELIGIBLE",
        }.get(invasion.get("status"), invasion.get("status"))
    state.setdefault("corridor_opened", any(
        event.get("type") == "CE_TRANSIT_COMPLETE" and
        event.get("from_arm") == "OLD_ARM" and event.get("to_arm") == "SECOND_HOME"
        for event in state.get("history", [])
    ))
    state.setdefault("union_invasion_started", state["corridor_opened"])
    state.setdefault("transit_access_mode", "PLAYER_ONLY")
    state.setdefault("scouts", {
        "spared": 0, "destroyed": 0, "coerced": 0, "followed": 0,
        "arrival_profile": "UNKNOWN",
    })
    state.setdefault("race_states", {race: "ACTIVE" for race in RACES})
    state.setdefault("capital_boss_states", {race: "ALIVE" for race in RACES})
    legacy_orphan = state.pop("orphan_state", "UNKNOWN")
    state["orphan_sample_taken"] = legacy_orphan == "SAMPLED"
    state["orphan_state"] = {
        "CONTACTED": "ACTIVE", "SAMPLED": "HIDDEN"
    }.get(legacy_orphan, legacy_orphan)
    state.setdefault("coalition_gate", {
        "status": "RESEARCHING" if state["union_invasion_started"] else "LOCKED",
        "progress": 0,
        "material_tons": 0,
        "mass_transit": False,
        "science_bases": 1,
        "anti_dominator_program_active": False,
        "coalition_alive": True,
        "peace_reached": False,
    })
    state["schema_version"] = SCHEMA_VERSION
    return state


def create_state(
    old_star_count: int,
    cells: int,
    seed: int,
    old_sector_count: int = 19,
    initial_credits: int = 100_000,
    war_apart_state: str = "CLAN_ACTIVE",
    dominator_boss_states: dict[str, str] | None = None,
    pirate_clan_exists_at_corridor: str | None = None,
    player_pirate: bool | None = None,
) -> dict[str, Any]:
    if old_star_count < max(len(REQUIRED_NODES), old_sector_count * 2):
        raise StateError("old arm is too sparse for the required Second Home layout")
    if cells < 0:
        raise StateError("resonance cell count cannot be negative")
    if initial_credits < 0:
        raise StateError("credit balance cannot be negative")
    if war_apart_state not in WAR_APART_STATES:
        raise StateError("unknown War Apart outcome")
    if dominator_boss_states is None:
        dominator_boss_states = {series: "ACTIVE" for series in DOMINATOR_SERIES}
    if set(dominator_boss_states) != set(DOMINATOR_SERIES) or any(
        value not in DOMINATOR_BOSS_STATES
        for value in dominator_boss_states.values()
    ):
        raise StateError("invalid dominator boss state snapshot")
    if old_sector_count < MIN_SECTOR_COUNT:
        raise StateError("old arm has too few sectors for Second Home progression")
    if old_sector_count > len(SECTOR_NAME_POOL):
        raise StateError("sector name pool is too small for the old arm scale")
    second_seed = _stable_int("CE_SECOND_HOME", seed) & 0x7FFFFFFF
    second_sectors = _generate_sector_layout(second_seed, old_sector_count)
    second_systems = _generate_system_layout(
        second_seed, old_star_count, second_sectors
    )
    derived_clan_state, derived_player_pirate = _pirate_snapshot_from_outcome(war_apart_state)
    pirate_clan_state = pirate_clan_exists_at_corridor or derived_clan_state
    if pirate_clan_state not in {"UNKNOWN", "YES", "NO"}:
        raise StateError("invalid pirate clan survival snapshot")
    if player_pirate is None:
        player_pirate = derived_player_pirate
    if not isinstance(player_pirate, bool):
        raise StateError("invalid player pirate membership snapshot")
    if pirate_clan_state == "YES":
        migration_status = PIRATE_MIGRATION_RULE["eligible_initial_status"]
    elif pirate_clan_state == "NO":
        migration_status = PIRATE_MIGRATION_RULE["destroyed_initial_status"]
    else:
        migration_status = PIRATE_MIGRATION_RULE["unknown_initial_status"]
    state = {
        "schema_version": SCHEMA_VERSION,
        "revision": 0,
        "current_day": 0,
        "current_arm": "OLD_ARM",
        "cargo": {
            "CE_Item_TwinHomeAnchor": 1,
            "CE_Item_ResonanceCell": cells,
        },
        "credits": initial_credits,
        "war_apart_state": war_apart_state,
        "pirate_clan_exists_at_corridor": pirate_clan_state,
        "player_pirate": player_pirate,
        "corridor_opened": False,
        "union_invasion_started": False,
        "transit_access_mode": "PLAYER_ONLY",
        "scouts": {
            "spared": 0,
            "destroyed": 0,
            "coerced": 0,
            "followed": 0,
            "arrival_profile": "UNKNOWN",
        },
        "race_states": {race: "ACTIVE" for race in RACES},
        "capital_boss_states": {race: "ALIVE" for race in RACES},
        "orphan_state": "UNKNOWN",
        "orphan_sample_taken": False,
        "coalition_gate": {
            "status": "LOCKED",
            "progress": 0,
            "material_tons": 0,
            "mass_transit": False,
            "science_bases": 1,
            "anti_dominator_program_active": False,
            "coalition_alive": True,
            "peace_reached": False,
        },
        "dominator_boss_states": dict(dominator_boss_states),
        "dominator_invasions": {
            series: {
                "status": "INELIGIBLE",
                "eligible": dominator_boss_states[series] == "ACTIVE",
                "discovery_day": None,
                "arrival_day": None,
                "converted_system_ids": [],
            }
            for series in DOMINATOR_SERIES
        },
        "pirate_migration": {
            "status": migration_status,
            "eligible": pirate_clan_state == "YES",
            "first_passage_day": None,
            "arrival_day": None,
            "arrivals": 0,
            "converted_system_ids": [],
        },
        "story": {
            "node_order": list(_generate_story_node_order(second_seed)),
            "completed_node_ids": [],
        },
        "missions": {
            "order": list(_generate_mission_order(second_seed)),
            "completed_ids": [],
        },
        "transit": None,
        "maps": {
            "OLD_ARM": _new_map(seed, old_star_count, old_sector_count, (), (), ()),
            "SECOND_HOME": _new_map(
                second_seed, old_star_count, old_sector_count,
                REQUIRED_NODES, second_sectors, second_systems,
            ),
        },
        "history": [],
    }
    validate_state(state)
    return state


def _generate_story_node_order(seed: int) -> tuple[str, ...]:
    result = []
    for stage_id, node_ids in STORY_DECK_STAGES:
        result.extend(sorted(
            node_ids,
            key=lambda node_id: _stable_int(
                seed, "story-deck", stage_id, node_id
            ),
        ))
    return tuple(result)


def _generate_mission_order(seed: int) -> tuple[str, ...]:
    remaining = set(MISSION_BY_ID)
    completed = set()
    result = []
    while remaining:
        eligible = [
            mission_id for mission_id in remaining
            if set(MISSION_BY_ID[mission_id].get("prerequisites", ())) <= completed
        ]
        if not eligible:
            raise StateError("mission prerequisite graph contains a cycle")
        mission_id = min(
            eligible,
            key=lambda candidate: _stable_int(
                seed, "mission-priority", candidate
            ),
        )
        result.append(mission_id)
        completed.add(mission_id)
        remaining.remove(mission_id)
    return tuple(result)


def _generate_sector_layout(seed: int, sector_count: int) -> tuple[dict[str, str], ...]:
    by_archetype = {
        archetype: sorted(
            (item for item in SECTOR_NAME_POOL if item["archetype"] == archetype),
            key=lambda item: _stable_int(seed, "sector-name", item["id"]),
        )
        for archetype in SECTOR_ARCHETYPES
    }
    later_nodes = set(REQUIRED_NODES) - set(EARLY_STORY_NODES)
    selected = []
    for archetype in SECTOR_ARCHETYPES:
        needs_open_and_locked = (
            any(STORY_NODE_ARCHETYPES[node] == archetype for node in EARLY_STORY_NODES) and
            any(STORY_NODE_ARCHETYPES[node] == archetype for node in later_nodes)
        )
        selected.extend(by_archetype[archetype][:(2 if needs_open_and_locked else 1)])
    selected_ids = {item["id"] for item in selected}
    remaining = sorted(
        (item for item in SECTOR_NAME_POOL if item["id"] not in selected_ids),
        key=lambda item: _stable_int(seed, "sector-fill", item["id"]),
    )
    selected.extend(remaining[:sector_count - len(selected)])
    selected.sort(key=lambda item: _stable_int(seed, "sector-order", item["id"]))
    starting_sector_ids = set()
    for node_id in EARLY_STORY_NODES:
        archetype = STORY_NODE_ARCHETYPES[node_id]
        candidate = min(
            (
                item for item in selected
                if item["archetype"] == archetype and
                item["id"] not in starting_sector_ids
            ),
            key=lambda item: _stable_int(seed, "starting-sector", node_id, item["id"]),
        )
        starting_sector_ids.add(candidate["id"])
    if len(starting_sector_ids) != STARTING_SECTOR_COUNT:
        raise StateError("starting sector layout does not match story progression")
    layout = [
        {"id": item["id"], "display_name": item["display_name"],
         "archetype": item["archetype"],
         "discovery_tier": (
             "STARTING" if item["id"] in starting_sector_ids else "LOCKED"
         )}
        for item in selected
    ]
    _add_sector_adjacency(seed, layout)
    return tuple(layout)


def _add_sector_adjacency(seed: int, sectors: list[dict[str, Any]]) -> None:
    by_id = {sector["id"]: sector for sector in sectors}
    starting = sorted(
        (sector for sector in sectors if sector["discovery_tier"] == "STARTING"),
        key=lambda sector: _stable_int(seed, "starting-chain", sector["id"]),
    )
    locked = [sector for sector in sectors if sector["discovery_tier"] == "LOCKED"]
    adjacency = {sector["id"]: set() for sector in sectors}
    depth = {sector["id"]: 0 for sector in starting}

    for left, right in zip(starting, starting[1:]):
        adjacency[left["id"]].add(right["id"])
        adjacency[right["id"]].add(left["id"])

    representatives = [
        min(
            (sector for sector in locked if sector["archetype"] == archetype),
            key=lambda sector: _stable_int(
                seed, "depth-two-representative", archetype, sector["id"]
            ),
        )
        for archetype in SECTOR_ARCHETYPES
    ]
    representative_ids = {sector["id"] for sector in representatives}
    extras = sorted(
        (sector for sector in locked if sector["id"] not in representative_ids),
        key=lambda sector: _stable_int(seed, "locked-extra", sector["id"]),
    )
    if not extras:
        raise StateError("sector graph needs a border gateway before story sectors")
    gateway = extras.pop(0)
    gateway_parent = min(
        starting,
        key=lambda sector: _stable_int(seed, "gateway-parent", sector["id"]),
    )
    adjacency[gateway["id"]].add(gateway_parent["id"])
    adjacency[gateway_parent["id"]].add(gateway["id"])
    depth[gateway["id"]] = 1

    for sector in representatives:
        adjacency[sector["id"]].add(gateway["id"])
        adjacency[gateway["id"]].add(sector["id"])
        depth[sector["id"]] = 2

    connected_ids = [
        sector["id"] for sector in starting
    ] + [gateway["id"]] + [sector["id"] for sector in representatives]
    for sector in extras:
        parent_id = min(
            (sector_id for sector_id in connected_ids if depth[sector_id] >= 1),
            key=lambda candidate_id: _stable_int(
                seed, "sector-parent", sector["id"], candidate_id
            ),
        )
        adjacency[sector["id"]].add(parent_id)
        adjacency[parent_id].add(sector["id"])
        depth[sector["id"]] = depth[parent_id] + 1
        connected_ids.append(sector["id"])

    base_price = DISCOVERY_RULE["base_price_credits"]
    depth_step = DISCOVERY_RULE["depth_price_step_credits"]
    for sector_id, sector in by_id.items():
        sector["adjacent_sector_ids"] = sorted(adjacency[sector_id])
        sector["discovery_depth"] = depth[sector_id]
        sector["map_price"] = 0 if depth[sector_id] == 0 else (
            base_price + depth[sector_id] * depth_step
        )


def _generate_system_layout(
    seed: int,
    star_count: int,
    sectors: tuple[dict[str, str], ...],
) -> tuple[dict[str, Any], ...]:
    if star_count > len(SYSTEM_NAME_POOL):
        raise StateError("system name pool is too small for the old arm scale")
    by_archetype = {
        archetype: sorted(
            (item for item in SYSTEM_NAME_POOL if item["archetype"] == archetype),
            key=lambda item: _stable_int(seed, "system-name", item["id"]),
        )
        for archetype in SECTOR_ARCHETYPES
    }
    selected_names = []
    for archetype in SECTOR_ARCHETYPES:
        required_hosts = sum(
            STORY_NODE_ARCHETYPES[node] == archetype for node in REQUIRED_NODES
        )
        archetype_sectors = [
            sector for sector in sectors if sector["archetype"] == archetype
        ]
        shallow_locked = sum(
            sector["discovery_tier"] == "LOCKED" and
            sector["discovery_depth"] < MIN_LATE_STORY_DEPTH
            for sector in archetype_sectors
        )
        minimum_systems = max(
            required_hosts, len(archetype_sectors) + shallow_locked
        )
        selected_names.extend(by_archetype[archetype][:minimum_systems])
    selected_ids = {item["id"] for item in selected_names}
    remaining = sorted(
        (item for item in SYSTEM_NAME_POOL if item["id"] not in selected_ids),
        key=lambda item: _stable_int(seed, "system-fill", item["id"]),
    )
    selected_names.extend(remaining[:star_count - len(selected_names)])
    selected = [
        {
            "id": item["id"],
            "display_name": item["display_name"],
            "archetype": item["archetype"],
            "story_node": None,
            "story_node_title": None,
            "capital_for": None,
        }
        for item in selected_names
    ]

    assigned_system_ids = set()
    for node_id in REQUIRED_NODES:
        preferred = STORY_NODE_ARCHETYPES[node_id]
        candidates = sorted(
            (
                item for item in selected
                if item["archetype"] == preferred and
                item["id"] not in assigned_system_ids
            ),
            key=lambda item: _stable_int(seed, "story-node", node_id, item["id"]),
        )
        if not candidates:
            raise StateError(f"no generated system can host story node {node_id}")
        host = candidates[0]
        host["story_node"] = node_id
        host["story_node_title"] = STORY_NODE_TITLES[node_id]
        host["capital_for"] = next(
            (race for race, capital_node in CAPITAL_STORY_NODES.items()
             if capital_node == node_id),
            None,
        )
        assigned_system_ids.add(host["id"])

    sectors_by_archetype = {
        archetype: sorted(
            (sector for sector in sectors if sector["archetype"] == archetype),
            key=lambda sector: _stable_int(seed, "system-sector", sector["id"]),
        )
        for archetype in SECTOR_ARCHETYPES
    }
    archetype_offsets = {archetype: 0 for archetype in SECTOR_ARCHETYPES}
    ordered = sorted(
        selected,
        key=lambda item: _stable_int(seed, "system-order", item["id"]),
    )
    result = []
    for item in ordered:
        archetype = item["archetype"]
        candidates = sectors_by_archetype[archetype]
        if item["story_node"] in EARLY_STORY_NODES:
            candidates = [
                sector for sector in candidates
                if sector["discovery_tier"] == "STARTING"
            ]
        elif item["story_node"]:
            candidates = [
                sector for sector in candidates
                if sector["discovery_tier"] == "LOCKED" and
                sector["discovery_depth"] >= MIN_LATE_STORY_DEPTH
            ]
        if not candidates:
            raise StateError("story system has no sector in its discovery tier")
        offset = archetype_offsets[archetype]
        sector = candidates[offset % len(candidates)]
        archetype_offsets[archetype] += 1
        result.append({**item, "sector_id": sector["id"]})
    for empty_sector in sectors:
        if any(system["sector_id"] == empty_sector["id"] for system in result):
            continue
        sector_loads = {
            sector["id"]: sum(
                system["sector_id"] == sector["id"] for system in result
            )
            for sector in sectors
        }
        movable = [
            system for system in result
            if system["archetype"] == empty_sector["archetype"] and
            not system["story_node"] and sector_loads[system["sector_id"]] > 1
        ]
        if not movable:
            raise StateError(f"sector {empty_sector['id']} has no generated system")
        system = min(
            movable,
            key=lambda item: _stable_int(
                seed, "fill-empty-sector", empty_sector["id"], item["id"]
            ),
        )
        system["sector_id"] = empty_sector["id"]
    for system in result:
        system["government_map_office"] = False
    for sector in sectors:
        candidates = [
            system for system in result if system["sector_id"] == sector["id"]
        ]
        if not candidates:
            raise StateError(f"sector {sector['id']} has no system for a map office")
        office = min(
            candidates,
            key=lambda system: _stable_int(
                seed, "government-map-office", sector["id"], system["id"]
            ),
        )
        office["government_map_office"] = True
    _apply_system_profiles(seed, result)
    return tuple(result)


def _apply_system_profiles(seed: int, systems: list[dict[str, Any]]) -> None:
    core_factions = tuple(CORE_FACTION_BY_ARCHETYPE.values())
    economy_bases = {
        "STRONG": 1150,
        "AGILL": 950,
        "MEDIUM": 1250,
        "INTELL": 1100,
        "ASH_BORDER": 700,
        "KLISSAN_SCAR": 350,
    }
    security_bases = {
        "STRONG": 760,
        "AGILL": 620,
        "MEDIUM": 580,
        "INTELL": 680,
        "ASH_BORDER": 280,
        "KLISSAN_SCAR": 180,
    }
    office_specializations = {
        "STRONG": "INDUSTRIAL",
        "AGILL": "COVERT",
        "MEDIUM": "TRADE",
        "INTELL": "RESEARCH",
        "ASH_BORDER": "SMUGGLING",
        "KLISSAN_SCAR": "QUARANTINE",
    }
    for system in systems:
        archetype = system["archetype"]
        specializations = POPULATION_RULE["specializations"][archetype]
        specialization = specializations[
            _stable_int(seed, "specialization", system["id"]) % len(specializations)
        ]
        if system["government_map_office"]:
            specialization = office_specializations[archetype]
        roll = _stable_int(seed, "controller", system["id"]) % 100
        if archetype in CORE_FACTION_BY_ARCHETYPE:
            controller = CORE_FACTION_BY_ARCHETYPE[archetype]
            if not system["government_map_office"] and roll >= 85:
                controller = (
                    "CE_LOCAL_ASH_CORSAIRS" if roll < 95 else "CE_UNCLAIMED"
                )
        elif archetype == "ASH_BORDER":
            if system["government_map_office"] or roll < 50:
                controller = "CE_LOCAL_ASH_CORSAIRS"
            elif roll < 80:
                controller = "CE_UNCLAIMED"
            else:
                controller = core_factions[
                    _stable_int(seed, "ash-controller", system["id"]) % len(core_factions)
                ]
        else:
            if system["government_map_office"]:
                controller = core_factions[
                    _stable_int(seed, "scar-outpost", system["id"]) % len(core_factions)
                ]
                specialization = "QUARANTINE"
            elif roll < 70:
                controller = "CE_HOSTILE_KLISSAN"
            elif roll < 90:
                controller = "CE_UNCLAIMED"
            else:
                controller = core_factions[
                    _stable_int(seed, "scar-controller", system["id"]) % len(core_factions)
                ]

        if system["government_map_office"]:
            condition = "INHABITED"
        elif specialization == "INFESTED":
            condition = "INFESTED"
        elif specialization == "DEAD":
            condition = "DEAD"
        elif specialization in {"BLACK_HOLE_OUTPOST", "QUARANTINE", "SALVAGE"}:
            condition = "OUTPOST"
        else:
            condition = (
                "INHABITED"
                if _stable_int(seed, "habitability", system["id"]) % 100 < 72
                else "OUTPOST"
            )

        if condition == "INFESTED":
            controller = "CE_HOSTILE_KLISSAN"

        if condition == "INHABITED":
            population = 50_000 + _stable_int(seed, "population", system["id"]) % 4_950_001
        elif condition == "OUTPOST":
            population = 1 + _stable_int(seed, "population", system["id"]) % 49_999
        else:
            population = 0
        economy = economy_bases[archetype] + (
            _stable_int(seed, "economy", system["id"]) % 401 - 200
        )
        security = security_bases[archetype] + (
            _stable_int(seed, "security", system["id"]) % 301 - 150
        )
        if condition in {"DEAD", "INFESTED"}:
            economy = min(economy, 250)
            security = min(security, 250)
        system["controller"] = controller
        system["owner"] = controller
        system["condition"] = condition
        system["specialization"] = specialization
        system["population_thousands"] = population
        system["economy_index"] = max(100, min(2000, economy))
        system["security_index"] = max(0, min(1000, security))
        system["system_state"] = (
            "KLISSAN_INFESTED" if condition == "INFESTED" else "CONTROLLED"
        )
        military_owner = controller in set(core_factions) | {
            "CE_FACTION_PIRATES", "CE_DOMINATOR_BLAZEROIDS",
            "CE_DOMINATOR_KELLEROIDS", "CE_DOMINATOR_TERRONOIDS",
        }
        system["military_production_enabled"] = military_owner
        system["strategic_expansion_enabled"] = military_owner
        system["last_capture_day"] = 0


def _new_map(
    seed: int,
    star_count: int,
    sector_count: int,
    nodes: tuple[str, ...],
    sectors: tuple[dict[str, str], ...],
    systems: tuple[dict[str, Any], ...],
) -> dict[str, Any]:
    starting_sector_ids = [
        sector["id"] for sector in sectors
        if sector.get("discovery_tier") == "STARTING"
    ]
    return {
        "seed": seed,
        "star_count": star_count,
        "sector_count": sector_count,
        "sectors": list(sectors),
        "systems": list(systems),
        "known_sector_ids": starting_sector_ids,
        "last_sim_day": 0,
        "aggregate_ticks": 0,
        "economy_index": 1000,
        "security_index": 500,
        "fronts": {"STRONG": 50, "AGILL": 50, "MEDIUM": 50, "INTELL": 50},
        "required_nodes": list(nodes),
        "event_serial": 0,
    }


def _scout_arrival_profile(scouts: dict[str, Any]) -> str:
    spared = scouts.get("spared", 0)
    destroyed = scouts.get("destroyed", 0)
    coerced = scouts.get("coerced", 0)
    followed = scouts.get("followed", 0)
    if spared + destroyed + coerced + followed < 2:
        return "UNKNOWN"
    if destroyed > spared:
        return "HUNTER"
    if spared > destroyed:
        return "MERCIFUL"
    if coerced + followed >= 2 and destroyed == 0:
        return "INTRUSIVE"
    return "UNKNOWN"


def record_scout_outcome(state: dict[str, Any], outcome: str) -> None:
    key_by_outcome = {
        "SPARED": "spared", "DESTROYED": "destroyed",
        "COERCED": "coerced", "FOLLOWED": "followed",
    }
    if outcome not in key_by_outcome:
        raise StateError("unknown scout outcome")
    scouts = state["scouts"]
    scouts[key_by_outcome[outcome]] += 1
    scouts["arrival_profile"] = _scout_arrival_profile(scouts)
    state["revision"] += 1
    validate_state(state)


def validate_state(state: dict[str, Any]) -> None:
    if state.get("schema_version") != SCHEMA_VERSION:
        raise StateError("unsupported schema version")
    if state.get("current_arm") not in ARMS:
        raise StateError("invalid current_arm")
    if set(state.get("maps", {})) != set(ARMS):
        raise StateError("state must contain exactly OLD_ARM and SECOND_HOME")
    if state.get("current_day", -1) < 0:
        raise StateError("current_day cannot be negative")
    if state.get("transit_access_mode") not in WAR_STATE_RULE["transit_access_modes"]:
        raise StateError("invalid transit access mode")
    if not isinstance(state.get("corridor_opened"), bool) or \
            not isinstance(state.get("union_invasion_started"), bool):
        raise StateError("invalid corridor chronology flags")
    if state["union_invasion_started"] and not state["corridor_opened"]:
        raise StateError("Union invasion cannot precede stable corridor opening")
    scouts = state.get("scouts", {})
    if set(scouts) != {"spared", "destroyed", "coerced", "followed", "arrival_profile"} or \
            any(type(scouts[key]) is not int or scouts[key] < 0
                for key in ("spared", "destroyed", "coerced", "followed")) or \
            scouts.get("arrival_profile") not in WAR_STATE_RULE["scout_arrival_profiles"]:
        raise StateError("invalid scout memory")
    if scouts["arrival_profile"] != _scout_arrival_profile(scouts):
        raise StateError("scout arrival profile does not match its counters")
    race_states = state.get("race_states", {})
    capital_states = state.get("capital_boss_states", {})
    if set(race_states) != set(RACES) or set(capital_states) != set(RACES) or \
            any(value not in WAR_STATE_RULE["race_states"] for value in race_states.values()) or \
            any(value not in WAR_STATE_RULE["capital_boss_states"] for value in capital_states.values()):
        raise StateError("invalid racial or capital state")
    if any(
        (capital_states[race] == "DESTROYED") != (race_states[race] != "ACTIVE")
        for race in RACES
    ):
        raise StateError("capital boss and racial state disagree")
    if state.get("orphan_state") not in WAR_STATE_RULE["orphan_states"] or \
            not isinstance(state.get("orphan_sample_taken"), bool):
        raise StateError("invalid Orphan state")
    gate = state.get("coalition_gate", {})
    if gate.get("status") not in WAR_STATE_RULE["coalition_gate_states"] or \
            not 0 <= gate.get("progress", -1) <= 10000 or \
            gate.get("material_tons", -1) < 0 or gate.get("science_bases", -1) < 0 or \
            not isinstance(gate.get("mass_transit"), bool) or \
            not isinstance(gate.get("anti_dominator_program_active"), bool) or \
            not isinstance(gate.get("coalition_alive"), bool) or \
            not isinstance(gate.get("peace_reached"), bool):
        raise StateError("invalid Coalition gate project")
    expected_mass = gate["status"] in {"ACTIVE", "PEACEFUL"}
    if gate["mass_transit"] != expected_mass:
        raise StateError("Coalition mass transit and gate state disagree")
    expected_mode = (
        "COALITION_MASS" if gate["status"] == "ACTIVE" else
        "PEACEFUL_MASS" if gate["status"] == "PEACEFUL" else "PLAYER_ONLY"
    )
    if state["transit_access_mode"] != expected_mode:
        raise StateError("transit access mode and Coalition gate disagree")

    old_count = state["maps"]["OLD_ARM"].get("star_count", 0)
    second_count = state["maps"]["SECOND_HOME"].get("star_count", 0)
    if old_count <= 0 or abs(second_count - old_count) / old_count > 0.10:
        raise StateError("Second Home star count exceeds the ±10% scale rule")

    old_sector_count = state["maps"]["OLD_ARM"].get("sector_count", 0)
    second_sector_count = state["maps"]["SECOND_HOME"].get("sector_count", 0)
    if old_sector_count <= 0 or \
            abs(second_sector_count - old_sector_count) / old_sector_count > 0.10:
        raise StateError("Second Home sector count exceeds the ±10% scale rule")
    sectors = state["maps"]["SECOND_HOME"].get("sectors", [])
    if len(sectors) != second_sector_count:
        raise StateError("Second Home sector layout does not match sector_count")
    sector_ids = [sector.get("id") for sector in sectors]
    sector_names = [sector.get("display_name") for sector in sectors]
    if len(sector_ids) != len(set(sector_ids)) or \
            len(sector_names) != len(set(sector_names)):
        raise StateError("Second Home sector ids and names must be unique")
    if any(not name or "sector_" in name.lower() for name in sector_names):
        raise StateError("Second Home contains a technical sector display name")
    if set(sector.get("archetype") for sector in sectors) != set(SECTOR_ARCHETYPES):
        raise StateError("Second Home sector layout must cover every archetype")
    starting_sector_ids = {
        sector["id"] for sector in sectors
        if sector.get("discovery_tier") == "STARTING"
    }
    if len(starting_sector_ids) != STARTING_SECTOR_COUNT or any(
        sector.get("discovery_tier") not in {"STARTING", "LOCKED"}
        for sector in sectors
    ):
        raise StateError("Second Home must have exactly three starting sectors")
    sector_by_id = {sector["id"]: sector for sector in sectors}
    for sector in sectors:
        adjacent = sector.get("adjacent_sector_ids", [])
        if len(adjacent) != len(set(adjacent)) or any(
            adjacent_id not in sector_by_id or adjacent_id == sector["id"]
            for adjacent_id in adjacent
        ):
            raise StateError("Second Home sector adjacency is invalid")
        if any(
            sector["id"] not in sector_by_id[adjacent_id]["adjacent_sector_ids"]
            for adjacent_id in adjacent
        ):
            raise StateError("Second Home sector adjacency must be symmetric")
        expected_price = 0 if sector["discovery_depth"] == 0 else (
            DISCOVERY_RULE["base_price_credits"] +
            sector["discovery_depth"] * DISCOVERY_RULE["depth_price_step_credits"]
        )
        if sector.get("map_price") != expected_price:
            raise StateError("Second Home sector map price is invalid")
    reachable = set(starting_sector_ids)
    while True:
        expanded = reachable | {
            adjacent_id
            for sector_id in reachable
            for adjacent_id in sector_by_id[sector_id]["adjacent_sector_ids"]
        }
        if expanded == reachable:
            break
        reachable = expanded
    if reachable != set(sector_ids):
        raise StateError("Second Home sector graph must be connected")

    known_sector_ids = state["maps"]["SECOND_HOME"].get("known_sector_ids", [])
    if len(known_sector_ids) != len(set(known_sector_ids)) or \
            not set(starting_sector_ids) <= set(known_sector_ids) or \
            not set(known_sector_ids) <= set(sector_ids):
        raise StateError("Second Home known sector set is invalid")

    systems = state["maps"]["SECOND_HOME"].get("systems", [])
    if len(systems) != second_count:
        raise StateError("Second Home system layout does not match star_count")
    system_ids = [system.get("id") for system in systems]
    system_names = [system.get("display_name") for system in systems]
    if len(system_ids) != len(set(system_ids)) or \
            len(system_names) != len(set(system_names)):
        raise StateError("Second Home system ids and names must be unique")
    forbidden = ("system_", "sector_", "placeholder", "todo", "test")
    if any(
        not isinstance(name, str) or not name.strip() or
        any(fragment in name.lower() for fragment in forbidden)
        for name in system_names
    ):
        raise StateError("Second Home contains a technical system display name")
    if any(system.get("sector_id") not in set(sector_ids) for system in systems):
        raise StateError("Second Home system is assigned to an unknown sector")
    sector_archetypes = {sector["id"]: sector["archetype"] for sector in sectors}
    if any(
        system.get("archetype") != sector_archetypes[system["sector_id"]]
        for system in systems
    ):
        raise StateError("Second Home system and sector archetypes do not match")

    nodes = state["maps"]["SECOND_HOME"].get("required_nodes", [])
    if len(nodes) != len(set(nodes)) or set(nodes) != set(REQUIRED_NODES):
        raise StateError("Second Home required nodes are missing or duplicated")
    story_hosts = [
        system["story_node"] for system in systems if system.get("story_node")
    ]
    if len(story_hosts) != len(set(story_hosts)) or \
            set(story_hosts) != set(REQUIRED_NODES):
        raise StateError("Second Home story nodes are missing or duplicated")
    if any(
        system.get("story_node") and
        system["archetype"] != STORY_NODE_ARCHETYPES[system["story_node"]]
        for system in systems
    ):
        raise StateError("Second Home story node has an invalid host archetype")
    if any(
        system.get("story_node") and
        system.get("story_node_title") != STORY_NODE_TITLES[system["story_node"]]
        for system in systems
    ):
        raise StateError("Second Home story node title is not canonical")
    story_systems = {
        system["story_node"]: system for system in systems
        if system.get("story_node")
    }
    capital_hosts = {
        system.get("capital_for"): system.get("story_node")
        for system in systems if system.get("capital_for") is not None
    }
    if capital_hosts != CAPITAL_STORY_NODES:
        raise StateError("Second Home capital systems do not match their races")
    if any(
        story_systems[node_id]["sector_id"] not in starting_sector_ids
        for node_id in EARLY_STORY_NODES
    ):
        raise StateError("early Second Home story node is outside starting sectors")
    if any(
        story_systems[node_id]["sector_id"] in starting_sector_ids
        for node_id in set(REQUIRED_NODES) - set(EARLY_STORY_NODES)
    ):
        raise StateError("later Second Home story node leaked into starting sectors")
    if any(
        sector_by_id[story_systems[node_id]["sector_id"]]["discovery_depth"] <
        MIN_LATE_STORY_DEPTH
        for node_id in set(REQUIRED_NODES) - set(EARLY_STORY_NODES)
    ):
        raise StateError("later Second Home story node is too close to the known map")
    story = state.get("story", {})
    node_order = story.get("node_order", [])
    expected_node_order = list(_generate_story_node_order(
        state["maps"]["SECOND_HOME"]["seed"]
    ))
    completed_node_ids = story.get("completed_node_ids", [])
    if node_order != expected_node_order or set(node_order) != set(REQUIRED_NODES):
        raise StateError("Second Home story deck does not match its seed")
    if not isinstance(completed_node_ids, list) or \
            completed_node_ids != node_order[:len(completed_node_ids)]:
        raise StateError("Second Home story completion is not a deck prefix")
    missions = state.get("missions", {})
    mission_order = missions.get("order", [])
    expected_mission_order = list(_generate_mission_order(
        state["maps"]["SECOND_HOME"]["seed"]
    ))
    completed_mission_ids = missions.get("completed_ids", [])
    if mission_order != expected_mission_order or \
            set(mission_order) != set(MISSION_BY_ID):
        raise StateError("mission deck does not match its seed")
    if not isinstance(completed_mission_ids, list) or \
            completed_mission_ids != mission_order[:len(completed_mission_ids)]:
        raise StateError("mission completion is not a deck prefix")
    earlier_missions = set()
    for mission_id in mission_order:
        if not set(MISSION_BY_ID[mission_id].get("prerequisites", ())) <= earlier_missions:
            raise StateError("mission deck violates a prerequisite")
        earlier_missions.add(mission_id)
    office_counts = {
        sector_id: sum(
            system.get("government_map_office") is True and
            system["sector_id"] == sector_id
            for system in systems
        )
        for sector_id in sector_ids
    }
    if any(count != 1 for count in office_counts.values()):
        raise StateError("every Second Home sector needs one government map office")
    allowed_controllers = set(POPULATION_RULE["controllers"])
    allowed_conditions = set(POPULATION_RULE["conditions"])
    economy_min, economy_max = POPULATION_RULE["economy_range"]
    security_min, security_max = POPULATION_RULE["security_range"]
    population_min, population_max = POPULATION_RULE["population_thousands_range"]
    for system in systems:
        if system.get("controller") not in allowed_controllers:
            raise StateError("Second Home system has an invalid controller")
        if system.get("owner") != system.get("controller"):
            raise StateError("Second Home owner and controller disagree")
        if system.get("system_state") not in STRATEGIC_SYSTEM_RULE["system_states"] or \
                not isinstance(system.get("military_production_enabled"), bool) or \
                not isinstance(system.get("strategic_expansion_enabled"), bool) or \
                not isinstance(system.get("last_capture_day"), int) or \
                system["last_capture_day"] < 0:
            raise StateError("Second Home strategic system fields are invalid")
        owner_race = next((race for race,faction in CORE_FACTION_BY_ARCHETYPE.items()
                           if faction == system["owner"]), None)
        if owner_race and race_states[owner_race] != "ACTIVE" and \
                (system["military_production_enabled"] or system["strategic_expansion_enabled"]):
            raise StateError("decapitated race cannot produce or expand")
        if system["system_state"] == "DEVASTATED" and (
            system["owner"] != STRATEGIC_SYSTEM_RULE["devastated_owner"] or
            system["military_production_enabled"] or system["strategic_expansion_enabled"]
        ):
            raise StateError("devastated system still has organized military control")
        if system["system_state"] == "KLISSAN_INFESTED" and \
                system["owner"] != STRATEGIC_SYSTEM_RULE["klissan_owner"]:
            raise StateError("Klissan-infested system has an invalid owner")
        if system.get("condition") not in allowed_conditions:
            raise StateError("Second Home system has an invalid condition")
        if system.get("condition") == "INHABITED" and \
                system.get("specialization") not in POPULATION_RULE["specializations"][system["archetype"]]:
            raise StateError("Second Home system has an invalid specialization")
        if not economy_min <= system.get("economy_index", -1) <= economy_max:
            raise StateError("Second Home system economy is outside its range")
        if not security_min <= system.get("security_index", -1) <= security_max:
            raise StateError("Second Home system security is outside its range")
        if not population_min <= system.get("population_thousands", -1) <= population_max:
            raise StateError("Second Home system population is outside its range")
        if system["condition"] in {"DEAD", "INFESTED"} and system["population_thousands"] != 0:
            raise StateError("dead or infested Second Home system has population")
        if (system["specialization"] == "DEAD") != (system["condition"] == "DEAD"):
            raise StateError("dead Second Home specialization and condition disagree")
        if (system["specialization"] == "INFESTED") != (system["condition"] == "INFESTED"):
            raise StateError("infested Second Home specialization and condition disagree")
        if system["government_map_office"] and (
            system["condition"] != "INHABITED" or system["population_thousands"] <= 0
        ):
            raise StateError("government map office needs an inhabited planet")

    cargo = state.get("cargo", {})
    if cargo.get("CE_Item_TwinHomeAnchor") != 1:
        raise StateError("Twin Home Anchor must exist exactly once")
    if cargo.get("CE_Item_ResonanceCell", -1) < 0:
        raise StateError("resonance cell count cannot be negative")
    if state.get("credits", -1) < 0:
        raise StateError("credit balance cannot be negative")

    migration = state.get("pirate_migration", {})
    migration_status = migration.get("status")
    war_apart_state = state.get("war_apart_state")
    if war_apart_state not in WAR_APART_STATES:
        raise StateError("invalid War Apart outcome snapshot")
    clan_state = state.get("pirate_clan_exists_at_corridor")
    if clan_state not in {"UNKNOWN", "YES", "NO"} or \
            not isinstance(state.get("player_pirate"), bool):
        raise StateError("invalid independent pirate survival snapshot")
    if migration_status not in WAR_STATE_RULE["pirate_states"] or \
            not isinstance(migration.get("eligible"), bool):
        raise StateError("invalid interarm pirate migration status")
    pirate_system_ids = {
        system["id"] for system in systems
        if system["controller"] == "CE_FACTION_PIRATES"
    }
    converted_ids = migration.get("converted_system_ids", [])
    if len(converted_ids) != len(set(converted_ids)):
        raise StateError("interarm pirate migration duplicated a system")
    eligible_outcome = clan_state == "YES"
    if migration["eligible"] != eligible_outcome:
        raise StateError("pirate clan survival snapshot and eligibility disagree")
    if migration_status == "INELIGIBLE":
        if pirate_system_ids or migration.get("first_passage_day") is not None or \
                migration.get("arrival_day") is not None or \
                migration.get("arrivals") != 0 or converted_ids:
            raise StateError("War Apart pirates exist before the interarm route opens")
    elif migration_status == "STEALING_TECH":
        first_day = migration.get("first_passage_day")
        arrival_day = migration.get("arrival_day")
        if not isinstance(first_day, int) or \
                arrival_day != first_day + _pirate_arrival_delay(state) or \
                state["current_day"] >= arrival_day or pirate_system_ids or \
                migration.get("arrivals") != 0 or converted_ids:
            raise StateError("invalid War Apart pirate scouting state")
    elif migration_status in {"ESTABLISHED", "SPLINTERED"}:
        if state["current_day"] < migration.get("arrival_day", state["current_day"] + 1) or \
                migration.get("arrivals") != len(converted_ids) or \
                len(converted_ids) < PIRATE_MIGRATION_RULE["first_wave_min_outposts"] or \
                pirate_system_ids != set(converted_ids):
            raise StateError("invalid established War Apart pirate foothold")
    elif migration_status == "ELIMINATED":
        if pirate_system_ids or converted_ids or migration.get("arrivals") != 0:
            raise StateError("eliminated War Apart pirate expedition still has a foothold")

    boss_states = state.get("dominator_boss_states", {})
    invasions = state.get("dominator_invasions", {})
    if set(boss_states) != set(DOMINATOR_SERIES) or \
            set(invasions) != set(DOMINATOR_SERIES):
        raise StateError("dominator invasion snapshot is incomplete")
    for series in DOMINATOR_SERIES:
        boss_state = boss_states[series]
        invasion = invasions[series]
        rule = DOMINATOR_INVASION_RULE["series"][series]
        status = invasion.get("status")
        if boss_state not in DOMINATOR_BOSS_STATES or \
                status not in WAR_STATE_RULE["dominator_states"] or \
                not isinstance(invasion.get("eligible"), bool):
            raise StateError("invalid dominator invasion branch")
        controlled_ids = {
            system["id"] for system in systems
            if system["controller"] == rule["controller"]
        }
        converted_ids = invasion.get("converted_system_ids", [])
        if len(converted_ids) != len(set(converted_ids)):
            raise StateError("dominator invasion duplicated a system")
        if invasion["eligible"] != (boss_state == "ACTIVE"):
            raise StateError("dominator boss and invasion eligibility disagree")
        if status == "INELIGIBLE":
            if controlled_ids or converted_ids or invasion.get("discovery_day") is not None or \
                    invasion.get("arrival_day") is not None:
                raise StateError("ineligible dominator series entered Second Home")
        elif status == "TRACKING":
            discovery_day = invasion.get("discovery_day")
            arrival_day = invasion.get("arrival_day")
            if not isinstance(discovery_day, int) or \
                    arrival_day != discovery_day + _dominator_arrival_delay(state, series) or \
                    state["current_day"] >= arrival_day or controlled_ids or converted_ids:
                raise StateError("invalid dominator tracking state")
        elif status == "ESTABLISHED":
            if boss_state != "ACTIVE":
                raise StateError("established dominator series has no living boss")
            if state["current_day"] < invasion.get("arrival_day", state["current_day"] + 1) or \
                    controlled_ids != set(converted_ids) or \
                    len(converted_ids) < DOMINATOR_INVASION_RULE["first_wave_min_systems"]:
                raise StateError("invalid dominator foothold")
        elif status == "STALLED":
            if boss_state != "ELIMINATED" or controlled_ids != set(converted_ids) or \
                    not converted_ids:
                raise StateError("invalid stalled dominator foothold")
        elif status == "ELIMINATED":
            if boss_state != "ELIMINATED" or controlled_ids or converted_ids:
                raise StateError("eliminated dominator series still controls systems")

    transit = state.get("transit")
    if transit is not None:
        if transit.get("phase") not in TRANSIT_PHASES:
            raise StateError("invalid transit phase")
        if transit.get("from_arm") not in ARMS or transit.get("to_arm") not in ARMS:
            raise StateError("invalid transit endpoints")
        if transit["from_arm"] == transit["to_arm"]:
            raise StateError("transit endpoints must differ")

    for arm, map_state in state["maps"].items():
        if map_state.get("last_sim_day", -1) > state["current_day"]:
            raise StateError(f"{arm} simulation is ahead of the global day")
        fronts = map_state.get("fronts", {})
        if set(fronts) != {"STRONG", "AGILL", "MEDIUM", "INTELL"}:
            raise StateError(f"{arm} has an invalid front set")
        if any(not 0 <= value <= 100 for value in fronts.values()):
            raise StateError(f"{arm} front value is outside 0..100")


def available_sector_maps(
    state: dict[str, Any], current_system_id: str
) -> tuple[dict[str, Any], ...]:
    validate_state(state)
    if state["current_arm"] != "SECOND_HOME":
        raise StateError("Second Home maps are sold only inside Second Home")
    second = state["maps"]["SECOND_HOME"]
    system = next(
        (item for item in second["systems"] if item["id"] == current_system_id),
        None,
    )
    if system is None:
        raise StateError("unknown Second Home system")
    if system["sector_id"] not in second["known_sector_ids"]:
        raise StateError("current system belongs to an unknown sector")
    if not system.get("government_map_office"):
        raise StateError("current system has no government map office")
    sectors = {sector["id"]: sector for sector in second["sectors"]}
    known = set(second["known_sector_ids"])
    return tuple(
        {
            "sector_id": sector_id,
            "display_name": sectors[sector_id]["display_name"],
            "price": sectors[sector_id]["map_price"],
        }
        for sector_id in sectors[system["sector_id"]]["adjacent_sector_ids"]
        if sector_id not in known
    )


def purchase_sector_map(
    state: dict[str, Any], current_system_id: str, sector_id: str
) -> None:
    offer = next(
        (
            item for item in available_sector_maps(state, current_system_id)
            if item["sector_id"] == sector_id
        ),
        None,
    )
    if offer is None:
        raise StateError("sector map is not sold from this bordering system")
    if state["credits"] < offer["price"]:
        raise StateError("not enough credits for the sector map")
    state["credits"] -= offer["price"]
    state["maps"]["SECOND_HOME"]["known_sector_ids"].append(sector_id)
    state["history"].append(
        {
            "type": "CE_SECTOR_MAP_PURCHASED",
            "day": state["current_day"],
            "system_id": current_system_id,
            "sector_id": sector_id,
            "price": offer["price"],
        }
    )
    state["revision"] += 1
    validate_state(state)


def story_sector_brief(state: dict[str, Any], story_node_id: str) -> dict[str, Any]:
    validate_state(state)
    if story_node_id not in REQUIRED_NODES:
        raise StateError("unknown Second Home story node")
    second = state["maps"]["SECOND_HOME"]
    system = next(
        item for item in second["systems"]
        if item.get("story_node") == story_node_id
    )
    sector = next(
        item for item in second["sectors"] if item["id"] == system["sector_id"]
    )
    is_known = sector["id"] in second["known_sector_ids"]
    instruction = (
        f'Навигационный архив обновлён: система «{system["display_name"]}», '
        f'сектор «{sector["display_name"]}».'
        if is_known else
        LOCKED_SECTOR_BRIEFINGS[story_node_id].format(
            sector=sector["display_name"]
        )
    )
    return {
        "story_node_id": story_node_id,
        "story_title": STORY_NODE_TITLES[story_node_id],
        "system_id": system["id"] if is_known else None,
        "system_name": system["display_name"] if is_known else None,
        "sector_id": sector["id"],
        "sector_name": sector["display_name"],
        "sector_known": is_known,
        "instruction": instruction,
    }


def current_story_target(state: dict[str, Any]) -> dict[str, Any] | None:
    validate_state(state)
    story = state["story"]
    index = len(story["completed_node_ids"])
    if index == len(story["node_order"]):
        return None
    node_id = story["node_order"][index]
    stage_id = next(
        stage_id for stage_id, node_ids in STORY_DECK_STAGES
        if node_id in node_ids
    )
    return {
        **story_sector_brief(state, node_id),
        "sequence_index": index,
        "stage_id": stage_id,
    }


def complete_story_target(state: dict[str, Any], story_node_id: str) -> None:
    target = current_story_target(state)
    if target is None:
        raise StateError("Second Home story deck is already complete")
    if story_node_id != target["story_node_id"]:
        raise StateError("story node is not the current randomized target")
    if state["current_arm"] != "SECOND_HOME":
        raise StateError("Second Home story target requires the Second Home map")
    if not target["sector_known"]:
        raise StateError("current story target remains behind an unknown sector map")
    state["story"]["completed_node_ids"].append(story_node_id)
    state["history"].append(
        {
            "type": "CE_STORY_NODE_COMPLETED",
            "day": state["current_day"],
            "story_node_id": story_node_id,
            "stage_id": target["stage_id"],
        }
    )
    state["revision"] += 1
    validate_state(state)


def current_mission(state: dict[str, Any]) -> dict[str, Any] | None:
    validate_state(state)
    missions = state["missions"]
    index = len(missions["completed_ids"])
    if index == len(missions["order"]):
        return None
    mission_id = missions["order"][index]
    definition = MISSION_BY_ID[mission_id]
    return {
        "mission_id": mission_id,
        "name_ru": definition["name_ru"],
        "act": definition["act"],
        "prerequisites": list(definition.get("prerequisites", ())),
        "sequence_index": index,
    }


def complete_current_mission(state: dict[str, Any], mission_id: str) -> None:
    mission = current_mission(state)
    if mission is None:
        raise StateError("randomized mission deck is already complete")
    if mission_id != mission["mission_id"]:
        raise StateError("mission is not the current randomized target")
    state["missions"]["completed_ids"].append(mission_id)
    state["history"].append(
        {
            "type": "CE_MISSION_COMPLETED",
            "day": state["current_day"],
            "mission_id": mission_id,
        }
    )
    state["revision"] += 1
    validate_state(state)


def simulate_days(state: dict[str, Any], days: int) -> None:
    if days < 0:
        raise StateError("days cannot be negative")
    if state.get("transit") is not None:
        raise StateError("recover pending transit before simulation")
    target_day = state["current_day"] + days
    active = state["current_arm"]
    inactive = _other_arm(active)
    state["maps"][active]["last_sim_day"] = target_day
    _simulate_inactive(state["maps"][inactive], target_day)
    _update_interarm_arrivals(state, target_day)
    _advance_coalition_gate(state, days)
    _advance_strategic_occupation(state, state["current_day"], target_day)
    state["current_day"] = target_day
    state["revision"] += 1
    validate_state(state)


def _simulate_inactive(map_state: dict[str, Any], target_day: int) -> None:
    day = map_state["last_sim_day"]
    while day < target_day:
        interval = 10 + _stable_int(map_state["seed"], day, "interval") % 21
        next_day = min(day + interval, target_day)
        if next_day == target_day and next_day - day < 10:
            break
        serial = map_state["event_serial"]
        economy_delta = int(_stable_int(map_state["seed"], serial, "economy") % 11) - 5
        security_delta = int(_stable_int(map_state["seed"], serial, "security") % 9) - 4
        map_state["economy_index"] = max(100, min(2000, map_state["economy_index"] + economy_delta))
        map_state["security_index"] = max(0, min(1000, map_state["security_index"] + security_delta))
        for index, faction in enumerate(map_state["fronts"]):
            delta = int(_stable_int(map_state["seed"], serial, faction, index) % 5) - 2
            map_state["fronts"][faction] = max(0, min(100, map_state["fronts"][faction] + delta))
        for system in map_state.get("systems", []):
            economy_delta = int(
                _stable_int(map_state["seed"], serial, system["id"], "system-economy") % 7
            ) - 3
            security_delta = int(
                _stable_int(map_state["seed"], serial, system["id"], "system-security") % 5
            ) - 2
            system["economy_index"] = max(
                POPULATION_RULE["economy_range"][0],
                min(POPULATION_RULE["economy_range"][1], system["economy_index"] + economy_delta),
            )
            system["security_index"] = max(
                POPULATION_RULE["security_range"][0],
                min(POPULATION_RULE["security_range"][1], system["security_index"] + security_delta),
            )
            if system["population_thousands"] > 0:
                population_delta = int(
                    _stable_int(map_state["seed"], serial, system["id"], "population") % 101
                ) - 50
                system["population_thousands"] = max(
                    1, min(
                        POPULATION_RULE["population_thousands_range"][1],
                        system["population_thousands"] + population_delta,
                    )
                )
        map_state["aggregate_ticks"] += 1
        map_state["event_serial"] += 1
        day = next_day
    map_state["last_sim_day"] = target_day


def _schedule_pirate_migration(state: dict[str, Any], passage_day: int) -> None:
    migration = state["pirate_migration"]
    if migration["status"] != "INELIGIBLE" or not migration["eligible"]:
        return
    migration["status"] = "STEALING_TECH"
    migration["first_passage_day"] = passage_day
    migration["arrival_day"] = (
        passage_day + _pirate_arrival_delay(state)
    )


def _pirate_arrival_delay(state: dict[str, Any]) -> int:
    if state["player_pirate"]:
        return PIRATE_MIGRATION_RULE["player_pirate_arrival_delay_days"]
    minimum, maximum = PIRATE_MIGRATION_RULE["arrival_delay_days_range"]
    return minimum + (
        _stable_int(
            state["maps"]["SECOND_HOME"]["seed"], "pirate-arrival-delay"
        ) % (maximum - minimum + 1)
    )


def _update_pirate_migration(state: dict[str, Any], target_day: int) -> None:
    migration = state["pirate_migration"]
    if migration["status"] != "STEALING_TECH" or target_day < migration["arrival_day"]:
        return
    systems = state["maps"]["SECOND_HOME"]["systems"]
    preferred = PIRATE_MIGRATION_RULE["preferred_archetype"]
    candidates = sorted(
        (
            system for system in systems
            if system["archetype"] == preferred and
            system["controller"] in {"CE_LOCAL_ASH_CORSAIRS", "CE_UNCLAIMED"}
        ),
        key=lambda system: _stable_int(
            state["maps"]["SECOND_HOME"]["seed"],
            "war-apart-pirate-arrival",
            system["id"],
        ),
    )
    wave_span = (
        PIRATE_MIGRATION_RULE["first_wave_max_outposts"] -
        PIRATE_MIGRATION_RULE["first_wave_min_outposts"] + 1
    )
    wave_size = PIRATE_MIGRATION_RULE["first_wave_min_outposts"] + (
        _stable_int(
            state["maps"]["SECOND_HOME"]["seed"], "pirate-wave-size"
        ) % wave_span
    )
    selected = candidates[:wave_size]
    if len(selected) < PIRATE_MIGRATION_RULE["first_wave_min_outposts"]:
        raise StateError("no valid Ash Border foothold for War Apart pirates")
    converted_ids = []
    for system in selected:
        system["controller"] = "CE_FACTION_PIRATES"
        system["owner"] = "CE_FACTION_PIRATES"
        system["system_state"] = "CONTROLLED"
        system["military_production_enabled"] = True
        system["strategic_expansion_enabled"] = True
        system["last_capture_day"] = migration["arrival_day"]
        converted_ids.append(system["id"])
    migration["status"] = "ESTABLISHED"
    migration["arrivals"] = len(converted_ids)
    migration["converted_system_ids"] = converted_ids
    state["history"].append(
        {
            "type": "CE_WAR_APART_PIRATES_ARRIVE",
            "day": migration["arrival_day"],
            "system_ids": converted_ids,
            "route": PIRATE_MIGRATION_RULE["route_explanation"],
        }
    )


def _schedule_dominator_invasions(state: dict[str, Any], passage_day: int) -> None:
    for series in DOMINATOR_SERIES:
        invasion = state["dominator_invasions"][series]
        if invasion["status"] != "INELIGIBLE" or not invasion["eligible"]:
            continue
        invasion["status"] = "TRACKING"
        invasion["discovery_day"] = passage_day
        invasion["arrival_day"] = (
            passage_day + _dominator_arrival_delay(state, series)
        )


def _dominator_arrival_delay(state: dict[str, Any], series: str) -> int:
    minimum, maximum = DOMINATOR_INVASION_RULE["series"][series][
        "arrival_delay_days_range"
    ]
    return minimum + (
        _stable_int(
            state["maps"]["SECOND_HOME"]["seed"],
            "dominator-arrival-delay",
            series,
        ) % (maximum - minimum + 1)
    )


def _update_dominator_invasions(state: dict[str, Any], target_day: int) -> None:
    systems = state["maps"]["SECOND_HOME"]["systems"]
    seed = state["maps"]["SECOND_HOME"]["seed"]
    for series in DOMINATOR_SERIES:
        invasion = state["dominator_invasions"][series]
        if invasion["status"] != "TRACKING" or target_day < invasion["arrival_day"]:
            continue
        rule = DOMINATOR_INVASION_RULE["series"][series]
        candidates = sorted(
            (
                system for system in systems
                if system["archetype"] in rule["preferred_archetypes"] and
                not system["story_node"] and
                not system["government_map_office"] and
                system["controller"] != "CE_FACTION_PIRATES" and
                not system["controller"].startswith("CE_DOMINATOR_")
            ),
            key=lambda system: _stable_int(
                seed, "dominator-arrival", series, system["id"]
            ),
        )
        wave_span = (
            DOMINATOR_INVASION_RULE["first_wave_max_systems"] -
            DOMINATOR_INVASION_RULE["first_wave_min_systems"] + 1
        )
        wave_size = DOMINATOR_INVASION_RULE["first_wave_min_systems"] + (
            _stable_int(seed, "dominator-wave-size", series) % wave_span
        )
        selected = candidates[:wave_size]
        if len(selected) < DOMINATOR_INVASION_RULE["first_wave_min_systems"]:
            raise StateError(f"no valid Second Home foothold for {series}")
        converted_ids = []
        for system in selected:
            system["controller"] = rule["controller"]
            system["owner"] = rule["controller"]
            system["system_state"] = "CONTROLLED"
            system["military_production_enabled"] = True
            system["strategic_expansion_enabled"] = True
            system["last_capture_day"] = invasion["arrival_day"]
            converted_ids.append(system["id"])
        invasion["status"] = "ESTABLISHED"
        invasion["converted_system_ids"] = converted_ids
        state["history"].append(
            {
                "type": "CE_DOMINATOR_SERIES_ARRIVES",
                "day": invasion["arrival_day"],
                "series": series,
                "system_ids": converted_ids,
                "route": rule["route"],
                "entry_front": rule["entry_front"],
            }
        )


def _update_interarm_arrivals(state: dict[str, Any], target_day: int) -> None:
    due_days = {
        invasion["arrival_day"]
        for invasion in state["dominator_invasions"].values()
        if invasion["status"] == "TRACKING" and
        invasion["arrival_day"] <= target_day
    }
    migration = state["pirate_migration"]
    if migration["status"] == "STEALING_TECH" and migration["arrival_day"] <= target_day:
        due_days.add(migration["arrival_day"])
    for arrival_day in sorted(due_days):
        _update_pirate_migration(state, arrival_day)
        _update_dominator_invasions(state, arrival_day)


def begin_transit(store: StateStore, crash_after: str | None = None) -> dict[str, Any]:
    state = store.load()
    validate_state(state)
    if state["transit"] is not None:
        raise StateError("a transit transaction is already pending")
    if state["cargo"]["CE_Item_ResonanceCell"] < 1:
        raise StateError("at least one resonance cell is required")
    source = state["current_arm"]
    state["transit"] = {
        "id": f"CE_TRANSIT_{uuid4().hex}",
        "phase": "PREPARED",
        "from_arm": source,
        "to_arm": _other_arm(source),
        "cell_balance_before": state["cargo"]["CE_Item_ResonanceCell"],
        "day": state["current_day"],
    }
    _commit(store, state)
    _maybe_crash("PREPARED", crash_after)
    return recover_transit(store, crash_after)


def debug_open_second_home(store: StateStore) -> dict[str, Any]:
    state = store.load()
    validate_state(state)
    if state["transit"] is not None:
        raise StateError("recover pending transit before using the debug entrance")
    if state["current_arm"] == "SECOND_HOME":
        return state
    state["current_arm"] = "SECOND_HOME"
    _open_stable_corridor(state, state["current_day"])
    state["history"].append(
        {
            "type": "CE_DEBUG_SECOND_HOME_OPENED",
            "day": state["current_day"],
        }
    )
    _update_interarm_arrivals(state, state["current_day"])
    _commit(store, state)
    return state


def recover_transit(store: StateStore, crash_after: str | None = None) -> dict[str, Any]:
    state = store.load()
    validate_state(state)
    transit = state.get("transit")
    if transit is None:
        return state

    if transit["phase"] == "PREPARED":
        expected = transit["cell_balance_before"]
        if state["cargo"]["CE_Item_ResonanceCell"] != expected:
            raise StateError("cell balance changed during PREPARED transit")
        state["cargo"]["CE_Item_ResonanceCell"] = expected - 1
        transit["phase"] = "DEBITED"
        _commit(store, state)
        _maybe_crash("DEBITED", crash_after)

    if transit["phase"] == "DEBITED":
        if state["cargo"]["CE_Item_ResonanceCell"] != transit["cell_balance_before"] - 1:
            raise StateError("resonance cell was not debited exactly once")
        state["current_arm"] = transit["to_arm"]
        transit["phase"] = "SWITCHED"
        _commit(store, state)
        _maybe_crash("SWITCHED", crash_after)

    if transit["phase"] == "SWITCHED":
        if state["current_arm"] != transit["to_arm"]:
            raise StateError("map switch did not reach the transaction target")
        state["history"].append(
            {
                "type": "CE_TRANSIT_COMPLETE",
                "id": transit["id"],
                "day": transit["day"],
                "from_arm": transit["from_arm"],
                "to_arm": transit["to_arm"],
            }
        )
        if transit["from_arm"] == "OLD_ARM" and transit["to_arm"] == "SECOND_HOME":
            _open_stable_corridor(state, transit["day"])
            _update_interarm_arrivals(state, transit["day"])
        state["transit"] = None
        _commit(store, state)
    return state


def _open_stable_corridor(state: dict[str, Any], day: int) -> None:
    if state["corridor_opened"]:
        return
    state["corridor_opened"] = True
    state["union_invasion_started"] = True
    state["scouts"]["arrival_profile"] = _scout_arrival_profile(state["scouts"])
    state["coalition_gate"]["status"] = (
        "RESEARCHING" if state["coalition_gate"]["science_bases"] > 0 else "PAUSED"
    )
    _schedule_pirate_migration(state, day)
    _schedule_dominator_invasions(state, day)
    state["history"].append({"type": "CE_UNION_INVASION_STARTED", "day": day})


def defeat_dominator_boss(state: dict[str, Any], series: str) -> None:
    if series not in DOMINATOR_SERIES:
        raise StateError("unknown dominator series")
    invasion = state["dominator_invasions"][series]
    state["dominator_boss_states"][series] = "ELIMINATED"
    invasion["eligible"] = False
    if invasion["status"] == "TRACKING":
        invasion.update(status="INELIGIBLE", discovery_day=None, arrival_day=None)
    elif invasion["status"] == "ESTABLISHED":
        invasion["status"] = "STALLED"
        controller = DOMINATOR_INVASION_RULE["series"][series]["controller"]
        for system in state["maps"]["SECOND_HOME"]["systems"]:
            if system["controller"] == controller:
                system["military_production_enabled"] = False
                system["strategic_expansion_enabled"] = False
    state["revision"] += 1
    validate_state(state)


def destroy_pirate_clan(state: dict[str, Any]) -> None:
    migration = state["pirate_migration"]
    if migration["status"] == "STEALING_TECH":
        migration.update(
            status="INELIGIBLE", eligible=False, first_passage_day=None,
            arrival_day=None,
        )
    elif migration["status"] == "ESTABLISHED":
        migration["status"] = "SPLINTERED"
        for system in state["maps"]["SECOND_HOME"]["systems"]:
            if system["controller"] == "CE_FACTION_PIRATES":
                system["military_production_enabled"] = False
                system["strategic_expansion_enabled"] = False
    else:
        migration["eligible"] = False
    state["revision"] += 1
    validate_state(state)


def defeat_capital(
    state: dict[str, Any], race: str, *, defenders_cleared: bool,
    producers_disabled: bool,
) -> None:
    if race not in RACES:
        raise StateError("unknown Second Home race")
    state["capital_boss_states"][race] = "DESTROYED"
    if not defenders_cleared or not producers_disabled:
        state["capital_boss_states"][race] = "ENGAGED"
        state["revision"] += 1
        validate_state(state)
        return
    state["race_states"][race] = "DECAPITATED"
    faction = CORE_FACTION_BY_ARCHETYPE[race]
    for map_state in state["maps"].values():
        for system in map_state.get("systems", []):
            if system.get("owner", system.get("controller")) == faction:
                system["military_production_enabled"] = False
                system["strategic_expansion_enabled"] = False
    state["revision"] += 1
    validate_state(state)


def resolve_orphan(state: dict[str, Any], outcome: str) -> None:
    """Resolve the Orphan without silently reviving an unavailable recipient."""
    target_by_outcome = {
        "DESTROY": "DESTROYED",
        "PACIFY": "PACIFIED",
        "FREE": "FREE",
        "HIDE": "HIDDEN",
        "SAMPLE": "HIDDEN",
        "TRANSFER_TO_INTELLS": "INTELL_CONTROLLED",
    }
    if outcome not in target_by_outcome:
        raise StateError("unknown Orphan outcome")
    if outcome == "TRANSFER_TO_INTELLS" and not can_transfer_orphan_to_intells(state):
        raise StateError("Intells cannot receive the Orphan")
    state["orphan_state"] = target_by_outcome[outcome]
    if outcome == "SAMPLE":
        state["orphan_sample_taken"] = True
    state["history"].append({
        "type": "CE_ORPHAN_RESOLVED",
        "day": state["current_day"],
        "outcome": outcome,
    })
    state["revision"] += 1
    validate_state(state)


def occupy_devastated_system(
    state: dict[str, Any], system_id: str, controller: str,
) -> None:
    """Give an empty system to an eligible strategic force."""
    systems = state["maps"]["SECOND_HOME"]["systems"]
    system = next((item for item in systems if item["id"] == system_id), None)
    if system is None or system["system_state"] != "DEVASTATED":
        raise StateError("only a devastated Second Home system can be occupied")
    allowed = set(STRATEGIC_SYSTEM_RULE["pre_coalition_occupiers"])
    if controller == "CE_FACTION_COALITION":
        if state["transit_access_mode"] != \
                STRATEGIC_SYSTEM_RULE["coalition_capture_requires_transit_mode"]:
            raise StateError("Coalition cannot occupy Second Home before its mass portal")
    elif controller not in allowed:
        raise StateError("force cannot occupy a devastated Second Home system")

    race = next((key for key, value in CORE_FACTION_BY_ARCHETYPE.items()
                 if value == controller), None)
    if race and state["race_states"][race] != "ACTIVE":
        raise StateError("decapitated race cannot capture systems")
    series = next((key for key, rule in DOMINATOR_INVASION_RULE["series"].items()
                   if rule["controller"] == controller), None)
    if series and state["dominator_invasions"][series]["status"] != "ESTABLISHED":
        raise StateError("inactive dominator series cannot capture systems")
    if controller == "CE_FACTION_PIRATES" and \
            state["pirate_migration"]["status"] != "ESTABLISHED":
        raise StateError("inactive War Apart pirates cannot capture systems")
    if controller == "CE_HOSTILE_KLISSAN" and state["orphan_state"] != "ACTIVE":
        raise StateError("Klissan nests cannot spread without an active Orphan")

    _set_system_occupation(state, system, controller, state["current_day"])
    state["revision"] += 1
    validate_state(state)


def _set_system_occupation(
    state: dict[str, Any], system: dict[str, Any], controller: str, day: int,
) -> None:
    system.update(
        owner=controller, controller=controller, system_state="CONTROLLED",
        military_production_enabled=controller != "CE_HOSTILE_KLISSAN",
        strategic_expansion_enabled=controller != "CE_HOSTILE_KLISSAN",
        last_capture_day=day,
    )
    if controller == "CE_HOSTILE_KLISSAN":
        system.update(
            condition="INFESTED", specialization="INFESTED",
            population_thousands=0, military_production_enabled=False,
            strategic_expansion_enabled=False, system_state="KLISSAN_INFESTED",
        )
    if controller == "CE_FACTION_PIRATES":
        migration = state["pirate_migration"]
        migration["converted_system_ids"].append(system["id"])
        migration["arrivals"] = len(migration["converted_system_ids"])
    for series, rule in DOMINATOR_INVASION_RULE["series"].items():
        if rule["controller"] == controller:
            state["dominator_invasions"][series]["converted_system_ids"].append(system["id"])
    state["history"].append({
        "type": "CE_SYSTEM_OCCUPIED",
        "day": day,
        "system_id": system["id"],
        "controller": controller,
    })


def _advance_strategic_occupation(
    state: dict[str, Any], start_day: int, target_day: int,
) -> None:
    """Apply sparse deterministic Klissan/Coalition captures, independent of tick size."""
    systems = state["maps"]["SECOND_HOME"]["systems"]
    for day in range(start_day + 1, target_day + 1):
        controller = None
        cadence = None
        if state["transit_access_mode"] == "COALITION_MASS":
            controller, cadence = "CE_FACTION_COALITION", 20
        elif state["orphan_state"] == "ACTIVE":
            controller, cadence = "CE_HOSTILE_KLISSAN", 30
        if cadence is None or day % cadence:
            continue
        candidates = sorted(
            (item for item in systems if item["system_state"] == "DEVASTATED"),
            key=lambda item: _stable_int(
                state["maps"]["SECOND_HOME"]["seed"], controller, day, item["id"]
            ),
        )
        if not candidates:
            continue
        _set_system_occupation(state, candidates[0], controller, day)


def devastate_system(state: dict[str, Any], system_id: str) -> None:
    systems = state["maps"]["SECOND_HOME"]["systems"]
    system = next((item for item in systems if item["id"] == system_id), None)
    if system is None:
        raise StateError("unknown Second Home system")
    old_controller = system["controller"]
    system.update(
        owner="CE_UNCLAIMED", controller="CE_UNCLAIMED",
        system_state="DEVASTATED", military_production_enabled=False,
        strategic_expansion_enabled=False, last_capture_day=state["current_day"],
    )
    for series, rule in DOMINATOR_INVASION_RULE["series"].items():
        if old_controller != rule["controller"]:
            continue
        invasion = state["dominator_invasions"][series]
        invasion["converted_system_ids"] = [
            value for value in invasion["converted_system_ids"] if value != system_id
        ]
        if invasion["status"] == "STALLED" and not invasion["converted_system_ids"]:
            invasion["status"] = "ELIMINATED"
    if old_controller == "CE_FACTION_PIRATES":
        migration = state["pirate_migration"]
        migration["converted_system_ids"] = [
            value for value in migration["converted_system_ids"] if value != system_id
        ]
        migration["arrivals"] = len(migration["converted_system_ids"])
        if migration["status"] == "SPLINTERED" and not migration["converted_system_ids"]:
            migration["status"] = "ELIMINATED"
    for race, faction in CORE_FACTION_BY_ARCHETYPE.items():
        if old_controller == faction and state["race_states"][race] == "DECAPITATED" and not any(
            item["owner"] == faction for item in systems
        ):
            state["race_states"][race] = "ELIMINATED"
    state["revision"] += 1
    validate_state(state)


def can_transfer_orphan_to_intells(state: dict[str, Any]) -> bool:
    return state["race_states"]["INTELL"] == "ACTIVE"


def donate_gate_material(state: dict[str, Any], tons: int) -> None:
    if tons <= 0:
        raise StateError("gate research donation must be positive")
    state["coalition_gate"]["material_tons"] += tons
    state["revision"] += 1
    validate_state(state)


def configure_coalition_research(
    state: dict[str, Any], *, science_bases: int | None = None,
    anti_dominator_active: bool | None = None, coalition_alive: bool | None = None,
    peace_reached: bool | None = None,
) -> None:
    gate = state["coalition_gate"]
    if science_bases is not None:
        if science_bases < 0:
            raise StateError("science base count cannot be negative")
        gate["science_bases"] = science_bases
    if anti_dominator_active is not None:
        gate["anti_dominator_program_active"] = anti_dominator_active
    if coalition_alive is not None:
        gate["coalition_alive"] = coalition_alive
    if peace_reached is not None:
        gate["peace_reached"] = peace_reached
    if not gate["coalition_alive"]:
        gate["status"] = "FAILED"
        gate["mass_transit"] = False
        state["transit_access_mode"] = "PLAYER_ONLY"
    elif state["union_invasion_started"] and gate["status"] not in {"ACTIVE", "PEACEFUL"}:
        gate["status"] = "RESEARCHING" if gate["science_bases"] else "PAUSED"
    state["revision"] += 1
    validate_state(state)


def _advance_coalition_gate(state: dict[str, Any], days: int) -> None:
    gate = state["coalition_gate"]
    if gate["status"] in {"LOCKED", "FAILED", "ACTIVE", "PEACEFUL"}:
        return
    if not gate["coalition_alive"]:
        gate["status"] = "FAILED"
        return
    if gate["science_bases"] == 0:
        gate["status"] = "PAUSED"
        return
    gate["status"] = "RESEARCHING"
    rule = WAR_STATE_RULE["coalition_gate_research"]
    for day_offset in range(1, days + 1):
        raw_speed = min(
            rule["raw_speed_percent_max"],
            rule["raw_speed_percent_base"] +
            rule["raw_speed_percent_per_material_ton"] * gate["material_tons"],
        )
        multiplier = (
            rule["parallel_program_multiplier_basis_points"] / 10000
            if gate["anti_dominator_program_active"] else 1.0
        )
        gate["progress"] = min(10000, gate["progress"] + int(raw_speed * multiplier))
        if gate["material_tons"]:
            gate["material_tons"] -= 1
        if gate["progress"] == 10000:
            gate["status"] = "PEACEFUL" if gate["peace_reached"] else "ACTIVE"
            gate["mass_transit"] = True
            state["transit_access_mode"] = (
                "PEACEFUL_MASS" if gate["peace_reached"] else "COALITION_MASS"
            )
            state["history"].append({
                "type": "CE_COALITION_GATE_ACTIVATED",
                "day": state["current_day"] + day_offset,
                "mode": state["transit_access_mode"],
            })
            break


def _commit(store: StateStore, state: dict[str, Any]) -> None:
    state["revision"] += 1
    validate_state(state)
    store.save(state)


def _maybe_crash(phase: str, crash_after: str | None) -> None:
    if phase == crash_after:
        raise SimulatedCrash(f"simulated crash after {phase}")


def run_demo(
    store: StateStore,
    old_stars: int,
    cells: int,
    seed: int,
    old_sectors: int = 19,
    war_apart_state: str = "CLAN_ACTIVE",
    dominator_boss_states: dict[str, str] | None = None,
) -> dict[str, Any]:
    store.save(create_state(
        old_stars, cells, seed, old_sectors,
        war_apart_state=war_apart_state,
        dominator_boss_states=dominator_boss_states,
    ))
    state = store.load()
    simulate_days(state, 40)
    store.save(state)
    for _ in range(3):
        begin_transit(store)
        state = store.load()
        simulate_days(state, 25)
        store.save(state)
    state = store.load()
    validate_state(state)
    return state


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    init = sub.add_parser("init")
    init.add_argument("state", type=Path)
    init.add_argument("--old-stars", type=int, default=64)
    init.add_argument("--old-sectors", type=int, default=19)
    init.add_argument("--cells", type=int, default=10)
    init.add_argument("--seed", type=int, default=3500)
    init.add_argument("--war-apart-state", choices=WAR_APART_STATES, default="CLAN_ACTIVE")
    for series in DOMINATOR_SERIES:
        init.add_argument(
            f"--{series.lower()}-state",
            choices=DOMINATOR_BOSS_STATES,
            default="ACTIVE",
        )
    transit = sub.add_parser("transit")
    transit.add_argument("state", type=Path)
    transit.add_argument("--crash-after", choices=TRANSIT_PHASES)
    recover = sub.add_parser("recover")
    recover.add_argument("state", type=Path)
    debug_open = sub.add_parser("debug-open-second-home")
    debug_open.add_argument("state", type=Path)
    simulate = sub.add_parser("simulate")
    simulate.add_argument("state", type=Path)
    simulate.add_argument("--days", type=int, required=True)
    maps = sub.add_parser("maps")
    maps.add_argument("state", type=Path)
    maps.add_argument("--system", required=True)
    buy_map = sub.add_parser("buy-map")
    buy_map.add_argument("state", type=Path)
    buy_map.add_argument("--system", required=True)
    buy_map.add_argument("--sector", required=True)
    verify = sub.add_parser("verify")
    verify.add_argument("state", type=Path)
    demo = sub.add_parser("demo")
    demo.add_argument("state", type=Path)
    demo.add_argument("--old-stars", type=int, default=64)
    demo.add_argument("--old-sectors", type=int, default=19)
    demo.add_argument("--cells", type=int, default=10)
    demo.add_argument("--seed", type=int, default=3500)
    demo.add_argument("--war-apart-state", choices=WAR_APART_STATES, default="CLAN_ACTIVE")
    for series in DOMINATOR_SERIES:
        demo.add_argument(
            f"--{series.lower()}-state",
            choices=DOMINATOR_BOSS_STATES,
            default="ACTIVE",
        )
    return parser


def main() -> None:
    args = _parser().parse_args()
    store = StateStore(args.state)
    try:
        if args.command == "init":
            state = create_state(
                args.old_stars, args.cells, args.seed, args.old_sectors,
                war_apart_state=args.war_apart_state,
                dominator_boss_states={
                    series: getattr(args, f"{series.lower()}_state")
                    for series in DOMINATOR_SERIES
                },
            )
            store.save(state)
        elif args.command == "transit":
            state = begin_transit(store, args.crash_after)
        elif args.command == "recover":
            state = recover_transit(store)
        elif args.command == "debug-open-second-home":
            state = debug_open_second_home(store)
        elif args.command == "simulate":
            state = store.load()
            simulate_days(state, args.days)
            store.save(state)
        elif args.command == "maps":
            state = store.load()
            offers = available_sector_maps(state, args.system)
            print(json.dumps(offers, ensure_ascii=False, indent=2))
        elif args.command == "buy-map":
            state = store.load()
            purchase_sector_map(state, args.system, args.sector)
            store.save(state)
        elif args.command == "verify":
            state = store.load()
            validate_state(state)
        else:
            state = run_demo(
                store, args.old_stars, args.cells, args.seed, args.old_sectors,
                args.war_apart_state,
                {
                    series: getattr(args, f"{series.lower()}_state")
                    for series in DOMINATOR_SERIES
                },
            )
    except SimulatedCrash as exc:
        print(f"CRASH: {exc}")
        raise SystemExit(75) from exc
    print(
        f"OK: arm={state['current_arm']} day={state['current_day']} "
        f"cells={state['cargo']['CE_Item_ResonanceCell']} "
        f"sectors={state['maps']['SECOND_HOME']['sector_count']} "
        f"transits={sum(event['type'] == 'CE_TRANSIT_COMPLETE' for event in state['history'])} "
        f"maps={sum(event['type'] == 'CE_SECTOR_MAP_PURCHASED' for event in state['history'])} "
        f"revision={state['revision']}"
    )


if __name__ == "__main__":
    main()
