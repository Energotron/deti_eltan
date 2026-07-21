import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from migrate_game_state import migrate


SCHEMA = json.loads(
    (Path(__file__).resolve().parents[2] / "data" / "game_state.schema.json")
    .read_text(encoding="utf-8")
)


class GameStateMigrationTests(unittest.TestCase):
    def test_sampled_orphan_keeps_sample_fact(self):
        state = migrate({"vCE_SchemaVersion": 1, "vCE_OrphanState": "SAMPLED"}, SCHEMA)
        self.assertEqual(state["vCE_SchemaVersion"], 3)
        self.assertEqual(state["vCE_OrphanState"], "ACTIVE")
        self.assertTrue(state["vCE_OrphanSampleTaken"])

    def test_player_membership_does_not_resurrect_destroyed_clan(self):
        state = migrate({
            "vCE_SchemaVersion": 2,
            "vCE_WarApartState": "COALITION_VICTORY",
            "vCE_PlayerPirateStatus": True,
        }, SCHEMA)
        self.assertEqual(state["vCE_PirateClanExistsAtCorridor"], "NO")
        self.assertTrue(state["vCE_PlayerPirateStatus"])


if __name__ == "__main__":
    unittest.main()
