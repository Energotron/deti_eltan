import json
import tempfile
import unittest
from pathlib import Path

from tools.second_map_spike import (
    EARLY_STORY_NODES,
    MISSION_BY_ID,
    REQUIRED_NODES,
    SECTOR_ARCHETYPES,
    SimulatedCrash,
    StateError,
    StateStore,
    available_sector_maps,
    begin_transit,
    complete_story_target,
    complete_current_mission,
    create_state,
    current_story_target,
    current_mission,
    debug_open_second_home,
    defeat_capital,
    defeat_dominator_boss,
    destroy_pirate_clan,
    devastate_system,
    donate_gate_material,
    configure_coalition_research,
    can_transfer_orphan_to_intells,
    occupy_devastated_system,
    record_scout_outcome,
    resolve_orphan,
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

    def test_debug_entrance_opens_second_home_without_anchor_quest_or_cell_cost(self):
        state = create_state(80, 0, 2441, old_sector_count=19)
        self.store.save(state)
        state = debug_open_second_home(self.store)
        self.assertEqual(state["current_arm"], "SECOND_HOME")
        self.assertEqual(state["cargo"]["CE_Item_ResonanceCell"], 0)
        self.assertEqual(
            sum(event["type"] == "CE_DEBUG_SECOND_HOME_OPENED" for event in state["history"]),
            1,
        )
        self.assertTrue(all(
            branch["status"] == "TRACKING"
            for branch in state["dominator_invasions"].values()
        ))
        state = debug_open_second_home(self.store)
        self.assertEqual(
            sum(event["type"] == "CE_DEBUG_SECOND_HOME_OPENED" for event in state["history"]),
            1,
        )
        self.assertEqual(
            sum(event["type"] == "CE_TRANSIT_COMPLETE" for event in state["history"]),
            0,
        )

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
                self.assertEqual(len(recovered_again["history"]), 2)
                self.assertEqual(
                    len([event for event in recovered_again["history"]
                         if event["type"] == "CE_UNION_INVASION_STARTED"]), 1,
                )
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
        self.assertEqual(state["pirate_migration"]["status"], "INELIGIBLE")
        self.assertFalse(any(
            system["controller"] == "CE_FACTION_PIRATES"
            for system in state["maps"]["SECOND_HOME"]["systems"]
        ))
        self.store.save(state)
        state = begin_transit(self.store)
        self.assertEqual(state["pirate_migration"]["status"], "STEALING_TECH")
        arrival_day = state["pirate_migration"]["arrival_day"]
        self.assertTrue(25 <= arrival_day <= 40)
        simulate_days(state, arrival_day - 1)
        self.assertEqual(state["pirate_migration"]["status"], "STEALING_TECH")
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

    def test_pirate_arrival_delay_varies_by_seed_inside_its_window(self):
        delays = set()
        for seed in range(2441, 2451):
            store = StateStore(Path(self.temp.name) / f"pirate-{seed}.json")
            store.save(create_state(80, 5, seed, old_sector_count=19))
            state = begin_transit(store)
            delay = state["pirate_migration"]["arrival_day"]
            self.assertTrue(25 <= delay <= 40)
            delays.add(delay)
        self.assertGreater(len(delays), 1)

    def test_destroyed_or_unknown_war_apart_clan_never_migrates(self):
        for outcome, expected_status in (
            ("COALITION_VICTORY", "INELIGIBLE"),
            ("UNKNOWN", "INELIGIBLE"),
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

    def test_player_pirate_status_does_not_resurrect_destroyed_clan(self):
        state = create_state(
            80, 5, 2441, old_sector_count=19,
            war_apart_state="COALITION_VICTORY",
            pirate_clan_exists_at_corridor="NO",
            player_pirate=True,
        )
        self.store.save(state)
        state = begin_transit(self.store)
        simulate_days(state, 100)
        self.assertFalse(state["pirate_migration"]["eligible"])
        self.assertEqual(state["pirate_migration"]["status"], "INELIGIBLE")
        self.assertFalse(any(
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
        arrival_days = {
            series: branch["arrival_day"]
            for series, branch in state["dominator_invasions"].items()
        }
        self.assertTrue(5 <= arrival_days["KELLER"] <= 10)
        self.assertTrue(12 <= arrival_days["BLAZER"] <= 20)
        self.assertTrue(24 <= arrival_days["TERRON"] <= 40)
        simulate_days(state, arrival_days["KELLER"] - 1)
        self.assertFalse(any(
            system["controller"].startswith("CE_DOMINATOR_")
            for system in state["maps"]["SECOND_HOME"]["systems"]
        ))
        simulate_days(state, 1)
        self.assertEqual(state["dominator_invasions"]["KELLER"]["status"], "ESTABLISHED")
        self.assertEqual(state["dominator_invasions"]["BLAZER"]["status"], "TRACKING")
        simulate_days(state, arrival_days["BLAZER"] - state["current_day"])
        self.assertEqual(state["dominator_invasions"]["BLAZER"]["status"], "ESTABLISHED")
        self.assertEqual(state["dominator_invasions"]["TERRON"]["status"], "TRACKING")
        self.store.save(state)
        state = self.store.load()
        validate_state(state)
        simulate_days(state, arrival_days["TERRON"] - state["current_day"])
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
        simulate_days(one_jump, 50)
        for _ in range(50):
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

    def test_dominator_footholds_and_delays_are_randomized_by_seed(self):
        locations_by_seed = []
        delays_by_seed = []
        for seed in (2441, 2442):
            state = create_state(80, 5, seed, old_sector_count=19)
            self.store.save(state)
            state = begin_transit(self.store)
            delays_by_seed.append({
                series: branch["arrival_day"]
                for series, branch in state["dominator_invasions"].items()
            })
            simulate_days(state, 50)
            locations = {
                series: tuple(branch["converted_system_ids"])
                for series, branch in state["dominator_invasions"].items()
            }
            flattened = [system_id for ids in locations.values() for system_id in ids]
            self.assertEqual(len(flattened), len(set(flattened)))
            locations_by_seed.append(locations)
        self.assertNotEqual(locations_by_seed[0], locations_by_seed[1])
        self.assertNotEqual(delays_by_seed[0], delays_by_seed[1])

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
                        "INELIGIBLE" if series == eliminated_series else "ESTABLISHED"
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

    def test_story_deck_randomizes_within_causal_stages_and_survives_load(self):
        left = create_state(80, 5, 2441, old_sector_count=19)
        right = create_state(80, 5, 2442, old_sector_count=19)
        left_order = left["story"]["node_order"]
        right_order = right["story"]["node_order"]
        self.assertNotEqual(left_order, right_order)
        self.assertEqual(set(left_order[:3]), set(EARLY_STORY_NODES))
        self.assertEqual(left_order[-1], "CE_SYS_SECOND_GATE")

        left["current_arm"] = "SECOND_HOME"
        for _ in range(3):
            target = current_story_target(left)
            self.assertTrue(target["sector_known"])
            complete_story_target(left, target["story_node_id"])

        target = current_story_target(left)
        self.assertFalse(target["sector_known"])
        with self.assertRaises(StateError):
            complete_story_target(left, target["story_node_id"])

        second = left["maps"]["SECOND_HOME"]
        while target["sector_id"] not in second["known_sector_ids"]:
            known = set(second["known_sector_ids"])
            purchase = next(
                (
                    (system["id"], offer)
                    for system in second["systems"]
                    if system["sector_id"] in known and system["government_map_office"]
                    for offer in available_sector_maps(left, system["id"])
                ),
                None,
            )
            self.assertIsNotNone(purchase)
            office_id, offer = purchase
            purchase_sector_map(left, office_id, offer["sector_id"])

        complete_story_target(left, target["story_node_id"])
        self.store.save(left)
        loaded = self.store.load()
        self.assertEqual(loaded["story"], left["story"])
        self.assertEqual(
            current_story_target(loaded)["story_node_id"],
            left_order[4],
        )

    def test_mission_deck_is_randomized_but_preserves_every_prerequisite(self):
        left = create_state(80, 5, 2441, old_sector_count=19)
        right = create_state(80, 5, 2442, old_sector_count=19)
        left_order = left["missions"]["order"]
        right_order = right["missions"]["order"]
        self.assertNotEqual(left_order, right_order)
        self.assertEqual(left_order[0], "CE-P00")
        positions = {mission_id: index for index, mission_id in enumerate(left_order)}
        for mission_id, mission in MISSION_BY_ID.items():
            self.assertTrue(all(
                positions[requirement] < positions[mission_id]
                for requirement in mission.get("prerequisites", ())
            ))

        with self.assertRaises(StateError):
            complete_current_mission(left, left_order[1])
        for mission_id in left_order[:5]:
            self.assertEqual(current_mission(left)["mission_id"], mission_id)
            complete_current_mission(left, mission_id)
        self.store.save(left)
        loaded = self.store.load()
        self.assertEqual(loaded["missions"], left["missions"])
        self.assertEqual(current_mission(loaded)["mission_id"], left_order[5])

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

    def test_scout_memory_profiles_survive_save_load(self):
        cases = {
            "MERCIFUL": ["SPARED", "SPARED"],
            "HUNTER": ["DESTROYED", "DESTROYED"],
            "INTRUSIVE": ["FOLLOWED", "COERCED"],
            "UNKNOWN": ["SPARED", "DESTROYED"],
        }
        for expected, outcomes in cases.items():
            with self.subTest(expected=expected):
                state = create_state(80, 5, 2441, old_sector_count=19)
                for outcome in outcomes:
                    record_scout_outcome(state, outcome)
                self.assertEqual(state["scouts"]["arrival_profile"], expected)
                self.store.save(state)
                self.assertEqual(
                    self.store.load()["scouts"]["arrival_profile"], expected
                )

    def test_capital_defeat_stops_race_and_cleanup_eliminates_it(self):
        state = create_state(80, 5, 2441, old_sector_count=19)
        faction = "CE_FACTION_STRONG"
        defeat_capital(
            state, "STRONG", defenders_cleared=False, producers_disabled=True
        )
        self.assertEqual(state["capital_boss_states"]["STRONG"], "ENGAGED")
        self.assertEqual(state["race_states"]["STRONG"], "ACTIVE")
        defeat_capital(
            state, "STRONG", defenders_cleared=True, producers_disabled=True
        )
        self.assertEqual(state["race_states"]["STRONG"], "DECAPITATED")
        strong_systems = [
            item for item in state["maps"]["SECOND_HOME"]["systems"]
            if item["owner"] == faction
        ]
        self.assertTrue(strong_systems)
        self.assertTrue(all(
            not item["military_production_enabled"] and
            not item["strategic_expansion_enabled"]
            for item in strong_systems
        ))
        for system in list(strong_systems):
            devastate_system(state, system["id"])
        self.assertEqual(state["race_states"]["STRONG"], "ELIMINATED")
        self.store.save(state)
        validate_state(self.store.load())

    def test_boss_killed_while_tracking_never_arrives(self):
        self.store.save(create_state(80, 5, 2441, old_sector_count=19))
        state = begin_transit(self.store)
        defeat_dominator_boss(state, "KELLER")
        simulate_days(state, 100)
        branch = state["dominator_invasions"]["KELLER"]
        self.assertEqual(branch["status"], "INELIGIBLE")
        self.assertFalse(any(
            item["controller"] == "CE_DOMINATOR_KELLEROIDS"
            for item in state["maps"]["SECOND_HOME"]["systems"]
        ))

    def test_boss_killed_after_arrival_stalls_until_cleanup(self):
        self.store.save(create_state(80, 5, 2441, old_sector_count=19))
        state = begin_transit(self.store)
        arrival = state["dominator_invasions"]["KELLER"]["arrival_day"]
        simulate_days(state, arrival)
        defeat_dominator_boss(state, "KELLER")
        branch = state["dominator_invasions"]["KELLER"]
        self.assertEqual(branch["status"], "STALLED")
        for system_id in list(branch["converted_system_ids"]):
            devastate_system(state, system_id)
        self.assertEqual(branch["status"], "ELIMINATED")
        self.store.save(state)
        validate_state(self.store.load())

    def test_pirate_clan_destroyed_after_arrival_splinters_then_dies(self):
        state = create_state(
            80, 5, 2441, old_sector_count=19,
            war_apart_state="PLAYER_PIRATE",
        )
        self.store.save(state)
        state = begin_transit(self.store)
        destroy_pirate_clan(state)
        migration = state["pirate_migration"]
        self.assertEqual(migration["status"], "SPLINTERED")
        for system_id in list(migration["converted_system_ids"]):
            devastate_system(state, system_id)
        self.assertEqual(migration["status"], "ELIMINATED")

    def test_orphan_recipient_disappears_after_intell_decapitation(self):
        state = create_state(80, 5, 2441, old_sector_count=19)
        self.assertTrue(can_transfer_orphan_to_intells(state))
        resolve_orphan(state, "TRANSFER_TO_INTELLS")
        defeat_capital(
            state, "INTELL", defenders_cleared=True, producers_disabled=True
        )
        self.assertFalse(can_transfer_orphan_to_intells(state))
        with self.assertRaises(StateError):
            resolve_orphan(state, "TRANSFER_TO_INTELLS")

    def test_destroyed_orphan_stops_klissan_spread(self):
        active = create_state(80, 5, 2441, old_sector_count=19)
        stopped = json.loads(json.dumps(active))
        for state in (active, stopped):
            candidate = next(
                item for item in state["maps"]["SECOND_HOME"]["systems"]
                if item["condition"] == "INHABITED" and not item["government_map_office"]
            )
            devastate_system(state, candidate["id"])
        active["orphan_state"] = "ACTIVE"
        resolve_orphan(stopped, "DESTROY")
        simulate_days(active, 30)
        simulate_days(stopped, 30)
        self.assertTrue(any(
            item["system_state"] == "KLISSAN_INFESTED"
            for item in active["maps"]["SECOND_HOME"]["systems"]
        ))
        self.assertFalse(any(
            item["system_state"] == "KLISSAN_INFESTED" and
            item["last_capture_day"] > 0
            for item in stopped["maps"]["SECOND_HOME"]["systems"]
        ))

    def test_coalition_cannot_capture_before_gate_but_can_after(self):
        state = create_state(80, 5, 2441, old_sector_count=19)
        candidate = next(
            item for item in state["maps"]["SECOND_HOME"]["systems"]
            if item["condition"] == "INHABITED" and not item["government_map_office"]
        )
        devastate_system(state, candidate["id"])
        with self.assertRaises(StateError):
            occupy_devastated_system(state, candidate["id"], "CE_FACTION_COALITION")
        self.store.save(state)
        state = debug_open_second_home(self.store)
        donate_gate_material(state, 1000)
        simulate_days(state, 100)
        self.assertEqual(state["transit_access_mode"], "COALITION_MASS")
        after_gate = next(
            item for item in state["maps"]["SECOND_HOME"]["systems"]
            if item["condition"] == "INHABITED" and not item["government_map_office"]
        )
        devastate_system(state, after_gate["id"])
        occupy_devastated_system(state, after_gate["id"], "CE_FACTION_COALITION")
        self.assertEqual(after_gate["owner"], "CE_FACTION_COALITION")

    def test_gate_pause_parallel_penalty_peace_and_failure(self):
        self.store.save(create_state(80, 5, 2441, old_sector_count=19))
        state = debug_open_second_home(self.store)
        simulate_days(state, 10)
        normal = state["coalition_gate"]["progress"]

        parallel = create_state(80, 5, 2441, old_sector_count=19)
        self.store.save(parallel)
        parallel = debug_open_second_home(self.store)
        configure_coalition_research(parallel, anti_dominator_active=True)
        simulate_days(parallel, 10)
        self.assertEqual(parallel["coalition_gate"]["progress"], int(normal * 0.6))

        configure_coalition_research(parallel, science_bases=0)
        paused = parallel["coalition_gate"]["progress"]
        simulate_days(parallel, 20)
        self.assertEqual(parallel["coalition_gate"]["progress"], paused)
        configure_coalition_research(parallel, science_bases=1)
        simulate_days(parallel, 1)
        self.assertGreater(parallel["coalition_gate"]["progress"], paused)

        peaceful = create_state(80, 5, 2441, old_sector_count=19)
        self.store.save(peaceful)
        peaceful = debug_open_second_home(self.store)
        configure_coalition_research(peaceful, peace_reached=True)
        donate_gate_material(peaceful, 1000)
        simulate_days(peaceful, 100)
        self.assertEqual(peaceful["transit_access_mode"], "PEACEFUL_MASS")

        configure_coalition_research(peaceful, coalition_alive=False)
        self.assertEqual(peaceful["coalition_gate"]["status"], "FAILED")
        self.assertFalse(peaceful["coalition_gate"]["mass_transit"])
        self.assertEqual(peaceful["transit_access_mode"], "PLAYER_ONLY")

    def test_gate_completion_day_does_not_depend_on_tick_size(self):
        base = create_state(80, 5, 2441, old_sector_count=19)
        self.store.save(base)
        base = debug_open_second_home(self.store)
        donate_gate_material(base, 1000)
        jump = json.loads(json.dumps(base))
        daily = json.loads(json.dumps(base))
        simulate_days(jump, 120)
        for _ in range(120):
            simulate_days(daily, 1)
        jump_event = next(
            event for event in jump["history"]
            if event["type"] == "CE_COALITION_GATE_ACTIVATED"
        )
        daily_event = next(
            event for event in daily["history"]
            if event["type"] == "CE_COALITION_GATE_ACTIVATED"
        )
        self.assertEqual(jump_event, daily_event)


if __name__ == "__main__":
    unittest.main()
