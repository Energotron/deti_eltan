import json
import tempfile
import unittest
from pathlib import Path

from tools.second_map_spike import (
    EARLY_STORY_NODES,
    REQUIRED_NODES,
    SECTOR_ARCHETYPES,
    SimulatedCrash,
    StateError,
    StateStore,
    available_sector_maps,
    begin_transit,
    create_state,
    purchase_sector_map,
    recover_transit,
    run_demo,
    simulate_days,
    story_sector_brief,
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
        self.assertEqual(
            sum(event["type"] == "CE_TRANSIT_COMPLETE" for event in state["history"]),
            3,
        )
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
        before_systems = {
            system["id"]: (
                system["economy_index"],
                system["security_index"],
                system["population_thousands"],
            )
            for system in second_before["systems"]
        }
        after_systems = {
            system["id"]: (
                system["economy_index"],
                system["security_index"],
                system["population_thousands"],
            )
            for system in state["maps"]["SECOND_HOME"]["systems"]
        }
        self.assertNotEqual(after_systems, before_systems)

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

    def test_every_second_home_star_has_a_real_unique_name_and_sector(self):
        state = create_state(80, 5, 2441, old_sector_count=19)
        second = state["maps"]["SECOND_HOME"]
        systems = second["systems"]
        sector_ids = {sector["id"] for sector in second["sectors"]}
        self.assertEqual(len(systems), 80)
        self.assertEqual(len({system["id"] for system in systems}), 80)
        self.assertEqual(len({system["display_name"] for system in systems}), 80)
        self.assertTrue(all(system["sector_id"] in sector_ids for system in systems))
        self.assertTrue(all(not system["display_name"].startswith("CE_") for system in systems))
        self.assertTrue(all("system_" not in system["display_name"].lower() for system in systems))
        story_hosts = {
            system["story_node"]: system["id"]
            for system in systems if system["story_node"]
        }
        self.assertEqual(set(story_hosts), set(REQUIRED_NODES))
        self.assertEqual(len(set(story_hosts.values())), len(REQUIRED_NODES))

    def test_every_system_has_a_living_economic_profile(self):
        state = create_state(80, 5, 2441, old_sector_count=19)
        systems = state["maps"]["SECOND_HOME"]["systems"]
        controllers = {system["controller"] for system in systems}
        self.assertEqual(len(systems), 80)
        self.assertTrue({
            "CE_FACTION_STRONG",
            "CE_FACTION_AGILL",
            "CE_FACTION_MEDIUM",
            "CE_FACTION_INTELL",
            "CE_LOCAL_ASH_CORSAIRS",
            "CE_HOSTILE_KLISSAN",
        } <= controllers)
        self.assertNotIn("CE_FACTION_PIRATES", controllers)
        self.assertFalse(any(controller.startswith("CE_DOMINATOR_") for controller in controllers))
        self.assertTrue(any(system["condition"] == "DEAD" for system in systems))
        self.assertTrue(any(system["condition"] == "INFESTED" for system in systems))
        self.assertTrue(all(100 <= system["economy_index"] <= 2000 for system in systems))
        self.assertTrue(all(0 <= system["security_index"] <= 1000 for system in systems))
        offices = [system for system in systems if system["government_map_office"]]
        self.assertEqual(len(offices), 19)
        self.assertTrue(all(
            system["condition"] == "INHABITED" and system["population_thousands"] > 0
            for system in offices
        ))

    def test_invalid_system_profile_is_rejected(self):
        state = create_state(80, 5, 2441, old_sector_count=19)
        office = next(
            system for system in state["maps"]["SECOND_HOME"]["systems"]
            if system["government_map_office"]
        )
        office["condition"] = "DEAD"
        with self.assertRaises(StateError):
            validate_state(state)
        state = create_state(80, 5, 2441, old_sector_count=19)
        state["maps"]["SECOND_HOME"]["systems"][0]["controller"] = "CE_FACTION_UNKNOWN"
        with self.assertRaises(StateError):
            validate_state(state)

    def test_war_apart_pirates_arrive_only_after_player_opens_the_route(self):
        state = create_state(80, 5, 2441, old_sector_count=19)
        self.assertEqual(state["pirate_migration"]["status"], "LOCKED")
        self.assertFalse(any(
            system["controller"] == "CE_FACTION_PIRATES"
            for system in state["maps"]["SECOND_HOME"]["systems"]
        ))
        self.store.save(state)
        state = begin_transit(self.store)
        self.assertEqual(state["pirate_migration"]["status"], "SCOUTING")
        self.assertEqual(state["pirate_migration"]["arrival_day"], 30)
        simulate_days(state, 29)
        self.assertEqual(state["pirate_migration"]["status"], "SCOUTING")
        self.assertFalse(any(
            system["controller"] == "CE_FACTION_PIRATES"
            for system in state["maps"]["SECOND_HOME"]["systems"]
        ))
        self.store.save(state)
        state = self.store.load()
        validate_state(state)
        simulate_days(state, 1)
        self.assertEqual(state["pirate_migration"]["status"], "ESTABLISHED")
        pirate_systems = {
            system["id"] for system in state["maps"]["SECOND_HOME"]["systems"]
            if system["controller"] == "CE_FACTION_PIRATES"
        }
        self.assertEqual(
            pirate_systems,
            set(state["pirate_migration"]["converted_system_ids"]),
        )
        self.assertTrue(1 <= len(pirate_systems) <= 3)
        self.store.save(state)
        validate_state(self.store.load())

    def test_war_apart_pirates_cannot_exist_before_the_first_passage(self):
        state = create_state(80, 5, 2441, old_sector_count=19)
        corsair = next(
            system for system in state["maps"]["SECOND_HOME"]["systems"]
            if system["controller"] == "CE_LOCAL_ASH_CORSAIRS"
        )
        corsair["controller"] = "CE_FACTION_PIRATES"
        with self.assertRaises(StateError):
            validate_state(state)

    def test_destroyed_or_unknown_war_apart_clan_never_migrates(self):
        for outcome, expected_status in (
            ("COALITION_VICTORY", "EXTINCT"),
            ("UNKNOWN", "UNRESOLVED"),
        ):
            with self.subTest(outcome=outcome):
                path = Path(self.temp.name) / f"{outcome}.json"
                store = StateStore(path)
                store.save(create_state(
                    80, 5, 2441, old_sector_count=19,
                    war_apart_state=outcome,
                ))
                state = begin_transit(store)
                simulate_days(state, 200)
                self.assertEqual(state["pirate_migration"]["status"], expected_status)
                self.assertFalse(any(
                    system["controller"] == "CE_FACTION_PIRATES"
                    for system in state["maps"]["SECOND_HOME"]["systems"]
                ))

    def test_player_pirate_brings_the_clan_through_immediately(self):
        state = create_state(
            80, 5, 2441, old_sector_count=19,
            war_apart_state="PLAYER_PIRATE",
        )
        self.store.save(state)
        state = begin_transit(self.store)
        self.assertEqual(state["pirate_migration"]["status"], "ESTABLISHED")
        self.assertEqual(state["pirate_migration"]["first_passage_day"], 0)
        self.assertEqual(state["pirate_migration"]["arrival_day"], 0)
        self.assertTrue(any(
            system["controller"] == "CE_FACTION_PIRATES"
            for system in state["maps"]["SECOND_HOME"]["systems"]
        ))

    def test_each_living_dominator_boss_finds_its_own_route(self):
        state = create_state(80, 5, 2441, old_sector_count=19)
        self.store.save(state)
        state = begin_transit(self.store)
        self.assertEqual(
            {series: branch["status"] for series, branch in state["dominator_invasions"].items()},
            {"BLAZER": "TRACKING", "KELLER": "TRACKING", "TERRON": "TRACKING"},
        )
        simulate_days(state, 6)
        self.assertFalse(any(
            system["controller"].startswith("CE_DOMINATOR_")
            for system in state["maps"]["SECOND_HOME"]["systems"]
        ))
        simulate_days(state, 1)
        self.assertEqual(state["dominator_invasions"]["KELLER"]["status"], "ESTABLISHED")
        self.assertEqual(state["dominator_invasions"]["BLAZER"]["status"], "TRACKING")
        simulate_days(state, 8)
        self.assertEqual(state["dominator_invasions"]["BLAZER"]["status"], "ESTABLISHED")
        self.assertEqual(state["dominator_invasions"]["TERRON"]["status"], "TRACKING")
        self.store.save(state)
        state = self.store.load()
        validate_state(state)
        simulate_days(state, 15)
        self.assertTrue(all(
            branch["status"] == "ESTABLISHED"
            for branch in state["dominator_invasions"].values()
        ))
        expected_fronts = {
            "CE_DOMINATOR_BLAZEROIDS": {"ASH_BORDER"},
            "CE_DOMINATOR_KELLEROIDS": {"KLISSAN_SCAR"},
            "CE_DOMINATOR_TERRONOIDS": {"MEDIUM", "INTELL"},
        }
        for controller, allowed_archetypes in expected_fronts.items():
            foothold_archetypes = {
                system["archetype"]
                for system in state["maps"]["SECOND_HOME"]["systems"]
                if system["controller"] == controller
            }
            self.assertTrue(foothold_archetypes)
            self.assertTrue(foothold_archetypes <= allowed_archetypes)

    def test_interarm_arrivals_do_not_depend_on_simulation_tick_size(self):
        self.store.save(create_state(80, 5, 2400, old_sector_count=19))
        initial = begin_transit(self.store)
        one_jump = json.loads(json.dumps(initial))
        daily = json.loads(json.dumps(initial))
        simulate_days(one_jump, 30)
        for _ in range(30):
            simulate_days(daily, 1)
        self.assertEqual(one_jump["pirate_migration"], daily["pirate_migration"])
        self.assertEqual(one_jump["dominator_invasions"], daily["dominator_invasions"])
        self.assertEqual(
            [system["controller"] for system in one_jump["maps"]["SECOND_HOME"]["systems"]],
            [system["controller"] for system in daily["maps"]["SECOND_HOME"]["systems"]],
        )
        self.assertEqual(
            [event for event in one_jump["history"] if "ARRIVE" in event["type"]],
            [event for event in daily["history"] if "ARRIVE" in event["type"]],
        )

    def test_dominator_footholds_are_randomized_by_seed_and_never_overlap(self):
        locations_by_seed = []
        for seed in (2441, 2442):
            state = create_state(80, 5, seed, old_sector_count=19)
            self.store.save(state)
            state = begin_transit(self.store)
            simulate_days(state, 30)
            locations = {
                series: tuple(branch["converted_system_ids"])
                for series, branch in state["dominator_invasions"].items()
            }
            flattened = [system_id for ids in locations.values() for system_id in ids]
            self.assertEqual(len(flattened), len(set(flattened)))
            locations_by_seed.append(locations)
        self.assertNotEqual(locations_by_seed[0], locations_by_seed[1])

    def test_each_eliminated_boss_forbids_only_its_own_series(self):
        controllers_by_series = {
            "BLAZER": "CE_DOMINATOR_BLAZEROIDS",
            "KELLER": "CE_DOMINATOR_KELLEROIDS",
            "TERRON": "CE_DOMINATOR_TERRONOIDS",
        }
        for eliminated_series in controllers_by_series:
            with self.subTest(eliminated_series=eliminated_series):
                state = create_state(
                    80, 5, 2441, old_sector_count=19,
                    dominator_boss_states={
                        series: (
                            "ELIMINATED" if series == eliminated_series else "ACTIVE"
                        )
                        for series in controllers_by_series
                    },
                )
                self.store.save(state)
                state = begin_transit(self.store)
                simulate_days(state, 100)
                controllers = {
                    system["controller"]
                    for system in state["maps"]["SECOND_HOME"]["systems"]
                }
                for series, controller in controllers_by_series.items():
                    expected_status = (
                        "EXTINCT" if series == eliminated_series else "ESTABLISHED"
                    )
                    self.assertEqual(
                        state["dominator_invasions"][series]["status"],
                        expected_status,
                    )
                    if series == eliminated_series:
                        self.assertNotIn(controller, controllers)
                    else:
                        self.assertIn(controller, controllers)

    def test_eliminated_dominator_series_cannot_be_injected(self):
        state = create_state(
            80, 5, 2441, old_sector_count=19,
            dominator_boss_states={
                "BLAZER": "ELIMINATED",
                "KELLER": "ELIMINATED",
                "TERRON": "ELIMINATED",
            },
        )
        state["maps"]["SECOND_HOME"]["systems"][0]["controller"] = (
            "CE_DOMINATOR_BLAZEROIDS"
        )
        with self.assertRaises(StateError):
            validate_state(state)

    def test_system_layout_is_deterministic_for_same_seed(self):
        left = create_state(80, 5, 2441, old_sector_count=19)
        right = create_state(80, 5, 2441, old_sector_count=19)
        self.assertEqual(
            left["maps"]["SECOND_HOME"]["systems"],
            right["maps"]["SECOND_HOME"]["systems"],
        )

    def test_story_nodes_choose_different_hosts_for_different_seeds(self):
        left = create_state(80, 5, 2441, old_sector_count=19)
        right = create_state(80, 5, 2442, old_sector_count=19)
        left_hosts = {
            system["story_node"]: system["id"]
            for system in left["maps"]["SECOND_HOME"]["systems"]
            if system["story_node"]
        }
        right_hosts = {
            system["story_node"]: system["id"]
            for system in right["maps"]["SECOND_HOME"]["systems"]
            if system["story_node"]
        }
        self.assertNotEqual(left_hosts, right_hosts)

    def test_first_story_nodes_are_open_and_later_nodes_start_hidden(self):
        state = create_state(80, 5, 2441, old_sector_count=19)
        second = state["maps"]["SECOND_HOME"]
        starting_sector_ids = {
            sector["id"] for sector in second["sectors"]
            if sector["discovery_tier"] == "STARTING"
        }
        self.assertEqual(len(starting_sector_ids), 3)
        story_sectors = {
            system["story_node"]: system["sector_id"]
            for system in second["systems"] if system["story_node"]
        }
        self.assertTrue(all(
            story_sectors[node_id] in starting_sector_ids
            for node_id in EARLY_STORY_NODES
        ))
        self.assertTrue(all(
            story_sectors[node_id] not in starting_sector_ids
            for node_id in set(REQUIRED_NODES) - set(EARLY_STORY_NODES)
        ))
        sector_depths = {
            sector["id"]: sector["discovery_depth"]
            for sector in second["sectors"]
        }
        self.assertTrue(all(
            sector_depths[story_sectors[node_id]] >= 2
            for node_id in set(REQUIRED_NODES) - set(EARLY_STORY_NODES)
        ))

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

    def test_layout_rejects_a_galaxy_too_sparse_for_map_offices(self):
        with self.assertRaises(StateError):
            create_state(11, 2, 77, old_sector_count=10)

    def test_vanilla_border_map_purchases_reveal_the_whole_galaxy(self):
        state = create_state(80, 5, 2441, old_sector_count=19)
        state["current_arm"] = "SECOND_HOME"
        second = state["maps"]["SECOND_HOME"]
        self.assertEqual(len(second["known_sector_ids"]), 3)
        credits_before = state["credits"]

        while len(second["known_sector_ids"]) < second["sector_count"]:
            known = set(second["known_sector_ids"])
            purchase = None
            for system in second["systems"]:
                if system["sector_id"] not in known or not system["government_map_office"]:
                    continue
                offers = available_sector_maps(state, system["id"])
                if offers:
                    purchase = (system["id"], offers[0])
                    break
            self.assertIsNotNone(purchase)
            office_id, offer = purchase
            purchase_sector_map(state, office_id, offer["sector_id"])
            if len(second["known_sector_ids"]) == 10:
                self.store.save(state)
                state = self.store.load()
                validate_state(state)
                second = state["maps"]["SECOND_HOME"]

        self.assertEqual(len(second["known_sector_ids"]), 19)
        self.assertLess(state["credits"], credits_before)
        self.assertEqual(
            len([event for event in state["history"] if event["type"] == "CE_SECTOR_MAP_PURCHASED"]),
            16,
        )

    def test_sector_map_requires_border_government_and_enough_credits(self):
        state = create_state(80, 5, 2441, old_sector_count=19)
        state["current_arm"] = "SECOND_HOME"
        second = state["maps"]["SECOND_HOME"]
        known = set(second["known_sector_ids"])
        office = next(
            system for system in second["systems"]
            if system["sector_id"] in known and system["government_map_office"] and
            available_sector_maps(state, system["id"])
        )
        offer = available_sector_maps(state, office["id"])[0]
        state["credits"] = offer["price"] - 1
        with self.assertRaises(StateError):
            purchase_sector_map(state, office["id"], offer["sector_id"])
        non_office = next(
            system for system in second["systems"]
            if system["sector_id"] in known and not system["government_map_office"]
        )
        with self.assertRaises(StateError):
            available_sector_maps(state, non_office["id"])

    def test_locked_story_brief_names_sector_without_dry_ui_wording(self):
        state = create_state(80, 5, 2441, old_sector_count=19)
        later_node = next(
            node for node in REQUIRED_NODES if node not in EARLY_STORY_NODES
        )
        brief = story_sector_brief(state, later_node)
        self.assertFalse(brief["sector_known"])
        self.assertIsNone(brief["system_name"])
        self.assertIn(brief["sector_name"], brief["instruction"])
        self.assertNotIn("Для продолжения", brief["instruction"])
        self.assertNotIn("Откройте сектор", brief["instruction"])


if __name__ == "__main__":
    unittest.main()
