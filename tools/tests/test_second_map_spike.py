import json
import tempfile
import unittest
from pathlib import Path

from tools.second_map_spike import (
    REQUIRED_NODES,
    SECTOR_ARCHETYPES,
    SimulatedCrash,
    StateError,
    StateStore,
    begin_transit,
    create_state,
    recover_transit,
    run_demo,
    simulate_days,
    validate_state,
)


class SecondMapSpikeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name) / "state.json"
        self.store = StateStore(self.path)

    def tearDown(self):
        self.temp.cleanup()

    def test_three_transits_keep_both_maps_and_debit_once_each(self):
        state = run_demo(self.store, old_stars=80, cells=5, seed=2441)
        self.assertEqual(state["current_arm"], "SECOND_HOME")
        self.assertEqual(state["cargo"]["CE_Item_ResonanceCell"], 2)
        self.assertEqual(len(state["history"]), 3)
        self.assertEqual(state["maps"]["OLD_ARM"]["star_count"], 80)
        self.assertEqual(state["maps"]["SECOND_HOME"]["star_count"], 80)
        self.assertEqual(state["maps"]["OLD_ARM"]["sector_count"], 19)
        self.assertEqual(state["maps"]["SECOND_HOME"]["sector_count"], 19)
        self.assertEqual(set(state["maps"]["SECOND_HOME"]["required_nodes"]), set(REQUIRED_NODES))

    def test_recovery_after_every_persisted_phase_is_idempotent(self):
        for phase in ("PREPARED", "DEBITED", "SWITCHED"):
            with self.subTest(phase=phase):
                path = Path(self.temp.name) / f"{phase}.json"
                store = StateStore(path)
                store.save(create_state(64, 3, 3500))
                with self.assertRaises(SimulatedCrash):
                    begin_transit(store, crash_after=phase)
                recovered = recover_transit(store)
                recovered_again = recover_transit(store)
                self.assertEqual(recovered_again["current_arm"], "SECOND_HOME")
                self.assertEqual(recovered_again["cargo"]["CE_Item_ResonanceCell"], 2)
                self.assertEqual(len(recovered_again["history"]), 1)
                self.assertEqual(recovered, recovered_again)

    def test_inactive_arm_simulates_in_aggregate_ticks(self):
        state = create_state(64, 5, 100)
        old_before = json.loads(json.dumps(state["maps"]["OLD_ARM"]))
        second_before = json.loads(json.dumps(state["maps"]["SECOND_HOME"]))
        simulate_days(state, 200)
        self.assertEqual(state["maps"]["OLD_ARM"]["aggregate_ticks"], 0)
        self.assertGreater(state["maps"]["SECOND_HOME"]["aggregate_ticks"], 0)
        self.assertEqual(state["maps"]["OLD_ARM"]["economy_index"], old_before["economy_index"])
        self.assertNotEqual(state["maps"]["SECOND_HOME"], second_before)
        self.assertEqual(state["maps"]["SECOND_HOME"]["last_sim_day"], 200)

    def test_simulation_is_deterministic_for_same_seed(self):
        left = create_state(64, 5, 99)
        right = create_state(64, 5, 99)
        simulate_days(left, 500)
        simulate_days(right, 500)
        self.assertEqual(left, right)

    def test_sector_layout_mirrors_old_arm_and_uses_real_unique_names(self):
        state = create_state(80, 5, 2441, old_sector_count=19)
        sectors = state["maps"]["SECOND_HOME"]["sectors"]
        self.assertEqual(len(sectors), 19)
        self.assertEqual(len({sector["id"] for sector in sectors}), 19)
        self.assertEqual(len({sector["display_name"] for sector in sectors}), 19)
        self.assertEqual({sector["archetype"] for sector in sectors}, set(SECTOR_ARCHETYPES))
        self.assertTrue(all(not sector["display_name"].startswith("CE_") for sector in sectors))

    def test_missing_anchor_and_scale_violation_are_rejected(self):
        state = create_state(64, 5, 99)
        state["cargo"]["CE_Item_TwinHomeAnchor"] = 0
        with self.assertRaises(StateError):
            validate_state(state)
        state = create_state(64, 5, 99)
        state["maps"]["SECOND_HOME"]["star_count"] = 40
        with self.assertRaises(StateError):
            validate_state(state)
        state = create_state(64, 5, 99, old_sector_count=19)
        state["maps"]["SECOND_HOME"]["sector_count"] = 10
        with self.assertRaises(StateError):
            validate_state(state)


if __name__ == "__main__":
    unittest.main()
