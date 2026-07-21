#!/usr/bin/env python3
"""Apply deterministic Children of Eltan state migrations to a JSON snapshot."""

from __future__ import annotations

from copy import deepcopy
from pathlib import Path
import argparse
import json


def condition_matches(state: dict, operation: dict) -> bool:
    exact = operation.get("when", {})
    choices = operation.get("when_in", {})
    return all(state.get(key) == value for key, value in exact.items()) and all(
        state.get(key) in values for key, values in choices.items()
    )


def migrate(state: dict, schema: dict) -> dict:
    result = deepcopy(state)
    target = next(
        variable["default"] for variable in schema["variables"]
        if variable["id"] == "vCE_SchemaVersion"
    )
    version = int(result.get("vCE_SchemaVersion", 1))
    migrations = {item["from_version"]: item for item in schema.get("migrations", [])}
    while version < target:
        migration = migrations.get(version)
        if migration is None or migration["to_version"] <= version:
            raise ValueError(f"no valid migration from schema version {version}")
        for operation in migration.get("operations", []):
            if condition_matches(result, operation):
                result.update(operation.get("set", {}))
        version = migration["to_version"]
        result["vCE_SchemaVersion"] = version
    if version > target:
        raise ValueError(f"state version {version} is newer than supported version {target}")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("state", type=Path)
    parser.add_argument(
        "--schema", type=Path,
        default=Path(__file__).resolve().parents[1] / "data" / "game_state.schema.json",
    )
    parser.add_argument("--output", type=Path, help="write migrated JSON; stdout otherwise")
    args = parser.parse_args()
    result = migrate(
        json.loads(args.state.read_text(encoding="utf-8")),
        json.loads(args.schema.read_text(encoding="utf-8")),
    )
    rendered = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.write_text(rendered, encoding="utf-8")
    else:
        print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
