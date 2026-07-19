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


SCHEMA_VERSION = 1
ARMS = ("OLD_ARM", "SECOND_HOME")
REQUIRED_NODES = (
    "CE_SYS_ELTAN_WOUND",
    "CE_SYS_FIRST_SHELTER",
    "CE_SYS_ARK_IV",
    "CE_SYS_KARH_FORTRESS",
    "CE_SYS_FACELESS_NODE",
    "CE_SYS_LUMEN",
    "CE_SYS_UNITY_PRISM",
    "CE_SYS_ASH_MARKET",
    "CE_SYS_ORPHAN_NEST",
    "CE_SYS_UNLIT_CITY",
    "CE_SYS_SECOND_GATE",
)
TRANSIT_PHASES = ("PREPARED", "DEBITED", "SWITCHED")


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
        return json.loads(self.path.read_text(encoding="utf-8"))

    def save(self, state: dict[str, Any]) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        temp = self.path.with_name(f".{self.path.name}.{uuid4().hex}.tmp")
        temp.write_text(
            json.dumps(state, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        os.replace(temp, self.path)


def create_state(old_star_count: int, cells: int, seed: int) -> dict[str, Any]:
    if old_star_count < len(REQUIRED_NODES):
        raise StateError("old arm is too small for the required Second Home nodes")
    if cells < 0:
        raise StateError("resonance cell count cannot be negative")
    second_seed = _stable_int("CE_SECOND_HOME", seed) & 0x7FFFFFFF
    state = {
        "schema_version": SCHEMA_VERSION,
        "revision": 0,
        "current_day": 0,
        "current_arm": "OLD_ARM",
        "cargo": {
            "CE_Item_TwinHomeAnchor": 1,
            "CE_Item_ResonanceCell": cells,
        },
        "transit": None,
        "maps": {
            "OLD_ARM": _new_map(seed, old_star_count, ()),
            "SECOND_HOME": _new_map(second_seed, old_star_count, REQUIRED_NODES),
        },
        "history": [],
    }
    validate_state(state)
    return state


def _new_map(seed: int, star_count: int, nodes: tuple[str, ...]) -> dict[str, Any]:
    return {
        "seed": seed,
        "star_count": star_count,
        "last_sim_day": 0,
        "aggregate_ticks": 0,
        "economy_index": 1000,
        "security_index": 500,
        "fronts": {"STRONG": 50, "AGILL": 50, "MEDIUM": 50, "INTELL": 50},
        "required_nodes": list(nodes),
        "event_serial": 0,
    }


def validate_state(state: dict[str, Any]) -> None:
    if state.get("schema_version") != SCHEMA_VERSION:
        raise StateError("unsupported schema version")
    if state.get("current_arm") not in ARMS:
        raise StateError("invalid current_arm")
    if set(state.get("maps", {})) != set(ARMS):
        raise StateError("state must contain exactly OLD_ARM and SECOND_HOME")
    if state.get("current_day", -1) < 0:
        raise StateError("current_day cannot be negative")

    old_count = state["maps"]["OLD_ARM"].get("star_count", 0)
    second_count = state["maps"]["SECOND_HOME"].get("star_count", 0)
    if old_count <= 0 or abs(second_count - old_count) / old_count > 0.10:
        raise StateError("Second Home star count exceeds the ±10% scale rule")

    nodes = state["maps"]["SECOND_HOME"].get("required_nodes", [])
    if len(nodes) != len(set(nodes)) or set(nodes) != set(REQUIRED_NODES):
        raise StateError("Second Home required nodes are missing or duplicated")

    cargo = state.get("cargo", {})
    if cargo.get("CE_Item_TwinHomeAnchor") != 1:
        raise StateError("Twin Home Anchor must exist exactly once")
    if cargo.get("CE_Item_ResonanceCell", -1) < 0:
        raise StateError("resonance cell count cannot be negative")

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
        map_state["aggregate_ticks"] += 1
        map_state["event_serial"] += 1
        day = next_day
    map_state["last_sim_day"] = target_day


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
        state["transit"] = None
        _commit(store, state)
    return state


def _commit(store: StateStore, state: dict[str, Any]) -> None:
    state["revision"] += 1
    validate_state(state)
    store.save(state)


def _maybe_crash(phase: str, crash_after: str | None) -> None:
    if phase == crash_after:
        raise SimulatedCrash(f"simulated crash after {phase}")


def run_demo(store: StateStore, old_stars: int, cells: int, seed: int) -> dict[str, Any]:
    store.save(create_state(old_stars, cells, seed))
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
    init.add_argument("--cells", type=int, default=10)
    init.add_argument("--seed", type=int, default=3500)
    transit = sub.add_parser("transit")
    transit.add_argument("state", type=Path)
    transit.add_argument("--crash-after", choices=TRANSIT_PHASES)
    recover = sub.add_parser("recover")
    recover.add_argument("state", type=Path)
    simulate = sub.add_parser("simulate")
    simulate.add_argument("state", type=Path)
    simulate.add_argument("--days", type=int, required=True)
    verify = sub.add_parser("verify")
    verify.add_argument("state", type=Path)
    demo = sub.add_parser("demo")
    demo.add_argument("state", type=Path)
    demo.add_argument("--old-stars", type=int, default=64)
    demo.add_argument("--cells", type=int, default=10)
    demo.add_argument("--seed", type=int, default=3500)
    return parser


def main() -> None:
    args = _parser().parse_args()
    store = StateStore(args.state)
    try:
        if args.command == "init":
            state = create_state(args.old_stars, args.cells, args.seed)
            store.save(state)
        elif args.command == "transit":
            state = begin_transit(store, args.crash_after)
        elif args.command == "recover":
            state = recover_transit(store)
        elif args.command == "simulate":
            state = store.load()
            simulate_days(state, args.days)
            store.save(state)
        elif args.command == "verify":
            state = store.load()
            validate_state(state)
        else:
            state = run_demo(store, args.old_stars, args.cells, args.seed)
    except SimulatedCrash as exc:
        print(f"CRASH: {exc}")
        raise SystemExit(75) from exc
    print(
        f"OK: arm={state['current_arm']} day={state['current_day']} "
        f"cells={state['cargo']['CE_Item_ResonanceCell']} "
        f"transits={len(state['history'])} revision={state['revision']}"
    )


if __name__ == "__main__":
    main()
