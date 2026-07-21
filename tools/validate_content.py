#!/usr/bin/env python3
from pathlib import Path
import json
ROOT = Path(__file__).resolve().parents[1]

def load(name):
    return json.loads((ROOT/'data'/name).read_text(encoding='utf-8'))

def unique(items, label, prefixes=('CE_','CE-','vCE_')):
    ids=[]
    for item in items:
        ident=item.get('id')
        if not ident: raise SystemExit(f'{label}: missing id')
        if prefixes and not ident.startswith(prefixes): raise SystemExit(f'{label}: bad prefix {ident}')
        ids.append(ident)
    if len(ids)!=len(set(ids)): raise SystemExit(f'{label}: duplicate ids')
    return set(ids)

def main():
    factions=load('factions.json')
    chars=load('characters.json')
    missions=load('missions.json')
    variables=load('game_state.schema.json')['variables']
    pirate=load('pirate_states.json')
    second=load('second_home_map.schema.json')
    items=load('transit_items.json')

    fids=unique(factions,'factions')
    unique(chars,'characters')
    mids=unique(missions,'missions')
    vids=unique(variables,'variables')
    item_ids=unique(items,'transit items')

    allowed={'CE_FACTION_COALITION','CE_FACTION_MENZOL'}
    for c in chars:
        if c['faction_id'] not in fids|allowed:
            raise SystemExit(f"unknown faction {c['faction_id']}")
    for m in missions:
        for req in m.get('prerequisites',[]):
            if req not in mids:
                raise SystemExit(f"unknown mission prerequisite {req} in {m['id']}")
    unresolved={mission['id']: set(mission.get('prerequisites',[])) for mission in missions}
    resolved=set()
    while unresolved:
        available={mission_id for mission_id,requirements in unresolved.items()
                   if requirements<=resolved}
        if not available:
            raise SystemExit('mission prerequisite graph contains a cycle')
        resolved|=available
        for mission_id in available:
            del unresolved[mission_id]
    mission_by_id={mission['id']: mission for mission in missions}
    scout_outcomes={'SPARE','DESTROY','COERCE','FOLLOW'}
    for scout_id in ('CE-P00','CE-SCOUT-RANDOM'):
        scout=mission_by_id.get(scout_id,{})
        if set(scout.get('outcomes',[]))!=scout_outcomes or \
                set(scout.get('effects',[]))!={
                    'RECORD_SCOUT_ENCOUNTER','RECOMPUTE_SCOUT_ARRIVAL_PROFILE'
                }:
            raise SystemExit(f'{scout_id} scout outcome contract is incomplete')
    coalition_gate=mission_by_id.get('CE-GATE-COALITION',{})
    peaceful_gate=mission_by_id.get('CE-GATE-PEACEFUL',{})
    if 'vCE_CoalitionState == ACTIVE' not in coalition_gate.get('condition','') or \
            coalition_gate.get('effects')!=['ENABLE_COALITION_MASS'] or \
            'vCE_CoalitionState == PEACE' not in peaceful_gate.get('condition','') or \
            'DISABLE_UNION_INVASION' not in peaceful_gate.get('effects',[]):
        raise SystemExit('Coalition and peaceful gate outcomes are not separated')
    for v in variables:
        if v['type']=='int' and not v['min'] <= v['default'] <= v['max']:
            raise SystemExit(f"bad range {v['id']}")
        if v['type']=='enum' and v['default'] not in v['values']:
            raise SystemExit(f"bad enum {v['id']}")

    required_vars={
        'vCE_WarApartState','vCE_CurrentArm','vCE_AnchorState','vCE_AnchorInCargo',
        'vCE_ResonanceCells','vCE_SecondHomeSeed','vCE_ScoutsSpared',
        'vCE_ScoutsDestroyed','vCE_ScoutsCoerced','vCE_ScoutsFollowed',
        'vCE_ScoutArrivalProfile','vCE_StrongState','vCE_AgillState',
        'vCE_MediumState','vCE_IntellState','vCE_StrongCapitalBoss',
        'vCE_AgillCapitalBoss','vCE_MediumCapitalBoss','vCE_IntellCapitalBoss',
        'vCE_BlazerSecondHome','vCE_KellerSecondHome','vCE_TerronSecondHome',
        'vCE_PirateSecondHome','vCE_CoalitionGateProject',
        'vCE_CoalitionGateProgress','vCE_CoalitionGateMaterialTons',
        'vCE_CoalitionMassTransit','vCE_ResearchParallelMultiplier',
        'vCE_TransitAccessMode','vCE_OrphanSampleTaken','vCE_CoalitionState',
        'vCE_PirateClanExistsAtCorridor','vCE_OldArmBlazerBoss',
        'vCE_OldArmKellerBoss','vCE_OldArmTerronBoss','vCE_OldArmBlazerKiller',
        'vCE_OldArmKellerKiller','vCE_OldArmTerronKiller',
    }
    missing=required_vars-vids
    if missing: raise SystemExit(f'missing required variables: {sorted(missing)}')

    anchor=next(x for x in items if x['id']=='CE_Item_TwinHomeAnchor')
    if anchor['uses_artifact_slot'] or anchor['uses_new_slot'] or anchor['kind']!='quest_cargo':
        raise SystemExit('Twin Home Anchor must be quest cargo and use no equipment slot')
    if anchor['consumed_on_transit']:
        raise SystemExit('Twin Home Anchor must not be consumed')
    if 'CE_Item_ResonanceCell' not in item_ids:
        raise SystemExit('missing resonance cell')

    deviation=second['scale_rule']['allowed_deviation_percent']
    if deviation>10:
        raise SystemExit('Second Home scale deviation exceeds product requirement')
    sector_deviation=second['sector_scale_rule']['allowed_deviation_percent']
    if sector_deviation>10 or second['sector_scale_rule']['target']!='mirror_old_arm_sector_count':
        raise SystemExit('Second Home sector count must mirror the old arm within 10 percent')
    if set(second['maps'])!={'OLD_ARM','SECOND_HOME'}:
        raise SystemExit('map schema must define both arms')

    display_names=second.get('display_names',{})
    story_node_titles=display_names.get('story_nodes',{})
    archetype_names=display_names.get('sector_archetypes',{})
    if set(story_node_titles)!=set(second['required_nodes']):
        raise SystemExit('every Second Home story node must have a canonical title')
    required_archetypes=second.get('story_node_preferred_archetypes',{})
    if set(required_archetypes)!=set(second['required_nodes']):
        raise SystemExit('every required Second Home system must have an archetype')
    if any(value not in second['sector_archetypes'] for value in required_archetypes.values()):
        raise SystemExit('required Second Home system has an unknown archetype')
    if second.get('capital_story_nodes')!={
        'STRONG':'CE_SYS_KARH_FORTRESS','AGILL':'CE_SYS_FACELESS_NODE',
        'MEDIUM':'CE_SYS_LUMEN','INTELL':'CE_SYS_UNITY_PRISM'
    }:
        raise SystemExit('Second Home capital story-node mapping is incomplete')
    progression=second.get('story_progression',{})
    early_nodes=progression.get('early_story_nodes',[])
    if progression.get('starting_open_sector_count')!=3 or len(early_nodes)!=3:
        raise SystemExit('Second Home must open with three sectors and three early story nodes')
    if progression.get('minimum_late_story_map_purchases',0)<2:
        raise SystemExit('later Second Home story must require at least two map purchases')
    if len(early_nodes)!=len(set(early_nodes)) or not set(early_nodes)<=set(second['required_nodes']):
        raise SystemExit('early Second Home story nodes are invalid or duplicated')
    if not progression.get('remaining_sectors_require_discovery') or \
            not progression.get('later_story_nodes_start_hidden'):
        raise SystemExit('later Second Home sectors and story nodes must start hidden')
    if progression.get('story_completion_reveals_sectors') is not False:
        raise SystemExit('Second Home story must not reveal sectors automatically')
    deck_stages=progression.get('deck_stages',[])
    deck_stage_ids=[stage.get('id') for stage in deck_stages]
    deck_nodes=[node for stage in deck_stages for node in stage.get('node_ids',[])]
    if deck_stage_ids!=['ARRIVAL','FOUR_VOICES','ASH_SECRETS','GATE'] or \
            any(not stage.get('node_ids') for stage in deck_stages) or \
            len(deck_nodes)!=len(set(deck_nodes)) or \
            set(deck_nodes)!=set(second['required_nodes']):
        raise SystemExit('Second Home randomized story deck is incomplete')
    if set(deck_stages[0]['node_ids'])!=set(early_nodes) or \
            deck_stages[-1]['node_ids']!=['CE_SYS_SECOND_GATE']:
        raise SystemExit('Second Home story deck breaks arrival or gate causality')
    discovery=second.get('sector_discovery_rule',{})
    if discovery.get('method')!='buy_star_map_from_government' or \
            not discovery.get('requires_border_adjacency'):
        raise SystemExit('Second Home sector discovery must follow vanilla map purchases')
    later_nodes=set(second['required_nodes'])-set(early_nodes)
    briefings=second.get('locked_sector_briefings',{})
    if set(briefings)!=later_nodes or any('{sector}' not in text for text in briefings.values()):
        raise SystemExit('every later story node needs an atmospheric locked-sector briefing')
    if set(archetype_names)!=set(second['sector_archetypes']):
        raise SystemExit('every Second Home sector archetype must have a display name')
    sector_pool=second.get('sector_name_pool',[])
    if len(sector_pool)<second['sector_scale_rule']['reference_medium_game_sectors']:
        raise SystemExit('sector name pool is too small for the reference medium galaxy')
    sector_ids=unique(sector_pool,'Second Home sector names',prefixes=('CE_SEC_',))
    if len(sector_ids)!=len(sector_pool):
        raise SystemExit('Second Home sector ids must be unique')
    if any(item.get('archetype') not in second['sector_archetypes'] for item in sector_pool):
        raise SystemExit('Second Home sector name has an unknown archetype')
    if set(item['archetype'] for item in sector_pool)!=set(second['sector_archetypes']):
        raise SystemExit('sector name pool must cover every archetype')
    sector_names=[item.get('display_name') for item in sector_pool]
    system_pool=second.get('system_name_pool',[])
    unique(system_pool,'Second Home system names',prefixes=('CE_SYS_',))
    if any(item.get('archetype') not in second['sector_archetypes'] for item in system_pool):
        raise SystemExit('Second Home system name has an unknown archetype')
    if set(item['archetype'] for item in system_pool)!=set(second['sector_archetypes']):
        raise SystemExit('system name pool must cover every archetype')
    reference_stars=second.get('reference_medium_game_stars',0)
    if len(system_pool)<reference_stars:
        raise SystemExit('system name pool is too small for the reference medium galaxy')
    system_pool_names=[item.get('display_name') for item in system_pool]
    visible_names=sector_names+system_pool_names
    if any(not isinstance(title,str) or not title.strip() for title in story_node_titles.values()):
        raise SystemExit('Second Home story node titles must be non-empty strings')
    if any(not isinstance(name,str) or not name.strip() for name in visible_names):
        raise SystemExit('Second Home display names must be non-empty strings')
    if len(visible_names)!=len(set(visible_names)):
        raise SystemExit('Second Home system and sector display names must be unique')
    forbidden_name_fragments=('sector_','system_','todo','placeholder','test')
    if any(any(fragment in name.lower() for fragment in forbidden_name_fragments) for name in visible_names):
        raise SystemExit('technical placeholder leaked into a Second Home display name')

    population_rule=second.get('system_population_rule',{})
    controllers=set(population_rule.get('controllers',[]))
    expected_controllers={
        'CE_FACTION_STRONG','CE_FACTION_AGILL','CE_FACTION_MEDIUM','CE_FACTION_INTELL',
        'CE_FACTION_PIRATES','CE_FACTION_COALITION','CE_LOCAL_ASH_CORSAIRS','CE_HOSTILE_KLISSAN','CE_UNCLAIMED',
        'CE_DOMINATOR_BLAZEROIDS','CE_DOMINATOR_KELLEROIDS','CE_DOMINATOR_TERRONOIDS'
    }
    if controllers!=expected_controllers:
        raise SystemExit(f'Second Home controller set mismatch: {controllers ^ expected_controllers}')
    if not controllers & fids == {
        'CE_FACTION_STRONG','CE_FACTION_AGILL','CE_FACTION_MEDIUM','CE_FACTION_INTELL',
        'CE_FACTION_PIRATES'
    }:
        raise SystemExit('Second Home controller factions are missing from factions.json')
    specializations=population_rule.get('specializations',{})
    if set(specializations)!=set(second['sector_archetypes']) or any(
        not values or len(values)!=len(set(values)) for values in specializations.values()
    ):
        raise SystemExit('Second Home specialization tables are incomplete or duplicated')
    if not population_rule.get('government_map_office_requires_inhabited_planet'):
        raise SystemExit('government map offices must require inhabited planets')
    for range_name in ('economy_range','security_range','population_thousands_range'):
        bounds=population_rule.get(range_name,[])
        if len(bounds)!=2 or bounds[0]<0 or bounds[0]>=bounds[1]:
            raise SystemExit(f'invalid Second Home {range_name}')
    strategic=second.get('strategic_system_rule',{})
    if set(strategic.get('required_fields',[]))!={
        'owner','military_production_enabled','strategic_expansion_enabled',
        'system_state','last_capture_day'
    } or set(strategic.get('system_states',[]))!={
        'CONTROLLED','CONTESTED','DEVASTATED','KLISSAN_INFESTED'
    } or strategic.get('coalition_capture_requires_transit_mode')!='COALITION_MASS':
        raise SystemExit('Second Home strategic system state model is incomplete')
    if set(strategic.get('pre_coalition_occupiers',[])) != expected_controllers-{
        'CE_UNCLAIMED','CE_FACTION_COALITION'
    }:
        raise SystemExit('Second Home pre-Coalition occupation matrix is incomplete')
    migration=second.get('interarm_pirate_migration',{})
    pirate_delay_range=migration.get('arrival_delay_days_range',[])
    if migration.get('eligibility_state_variable')!='vCE_PirateClanExistsAtCorridor' or \
            set(migration.get('eligible_values',[]))!={'YES'} or \
            set(migration.get('destroyed_values',[]))!={'NO'} or \
            migration.get('unknown_value_behavior')!='block_migration' or \
            migration.get('player_membership_variable')!='vCE_PlayerPirateStatus' or \
            migration.get('player_membership_changes_eligibility') is not False or \
            migration.get('eligible_initial_status')!='INELIGIBLE' or \
            migration.get('destroyed_initial_status')!='INELIGIBLE' or \
            migration.get('unknown_initial_status')!='INELIGIBLE' or \
            migration.get('trigger')!='first_union_invasion_after_stable_corridor' or \
            len(pirate_delay_range)!=2 or pirate_delay_range[0]<1 or \
            pirate_delay_range[0]>=pirate_delay_range[1] or \
            migration.get('player_pirate_arrival_delay_days')!=0 or \
            migration.get('preferred_archetype')!='ASH_BORDER':
        raise SystemExit('War Apart pirate migration outcome matrix is invalid')
    if migration.get('route_explanation')!='pirates_steal_agill_scout_calibration_technology':
        raise SystemExit('War Apart pirate arrival route is not justified')
    war_model=second.get('war_state_model',{})
    expected_enums={
        'transit_access_modes': {'PLAYER_ONLY','COALITION_MASS','PEACEFUL_MASS'},
        'race_states': {'ACTIVE','DECAPITATED','ELIMINATED'},
        'capital_boss_states': {'ALIVE','ENGAGED','DESTROYED'},
        'dominator_states': {'INELIGIBLE','TRACKING','ESTABLISHED','STALLED','ELIMINATED'},
        'pirate_states': {'INELIGIBLE','STEALING_TECH','ESTABLISHED','SPLINTERED','ELIMINATED'},
        'orphan_states': {'UNKNOWN','ACTIVE','HIDDEN','INTELL_CONTROLLED','PACIFIED','DESTROYED','FREE'},
        'scout_arrival_profiles': {'UNKNOWN','MERCIFUL','INTRUSIVE','HUNTER'},
        'coalition_gate_states': {'LOCKED','RESEARCHING','PAUSED','COMPLETE','ACTIVE','PEACEFUL','FAILED'},
    }
    if war_model.get('war_begins_on')!='first_stable_corridor_calibration' or \
            war_model.get('pre_corridor_union_presence')!='covert_only' or any(
                set(war_model.get(key,[]))!=values for key,values in expected_enums.items()
            ):
        raise SystemExit('Second Home war state enums or chronology are incomplete')
    profile=war_model.get('scout_profile_resolution',{})
    if profile.get('minimum_resolved_encounters')!=2 or \
            [rule.get('profile') for rule in profile.get('precedence',[])] != [
                'INTRUSIVE','MERCIFUL','HUNTER','UNKNOWN'
            ]:
        raise SystemExit('Scout arrival profile resolution is incomplete')
    research=war_model.get('coalition_gate_research',{})
    if research.get('progress_max')!=10000 or \
            research.get('parallel_program_multiplier_basis_points')!=6000 or \
            not research.get('progress_pauses_without_science_base'):
        raise SystemExit('Coalition gate research model is incomplete')
    pirate_faction=next(faction for faction in factions if faction['id']=='CE_FACTION_PIRATES')
    if 'Пепельные корсары' in pirate_faction.get('subfactions',[]) or \
            'Пепельные каперы' not in pirate_faction.get('subfactions',[]):
        raise SystemExit('local Ash corsairs must remain separate from War Apart pirate branches')
    dominators=second.get('interarm_dominator_invasions',{})
    expected_series={
        'BLAZER': ('CE_DOMINATOR_BLAZEROIDS',(12,20),'OUTER_ASH_APPROACH',{'ASH_BORDER'}),
        'KELLER': ('CE_DOMINATOR_KELLEROIDS',(5,10),'BLACK_HOLE_SCAR',{'KLISSAN_SCAR'}),
        'TERRON': ('CE_DOMINATOR_TERRONOIDS',(24,40),'INNER_FREIGHT_NETWORK',{'MEDIUM','INTELL'}),
    }
    if dominators.get('trigger')!='first_stable_corridor_calibration' or \
            set(dominators.get('boss_snapshot_state_values',[]))!={'UNKNOWN','ALIVE','DESTROYED'} or \
            dominators.get('eligible_snapshot_state')!='ALIVE' or \
            dominators.get('unknown_snapshot_behavior')!='block_that_series' or \
            set(dominators.get('series',{}))!=set(expected_series):
        raise SystemExit('Second Home dominator invasion matrix is incomplete')
    invasion_fronts=[]
    for series,(controller,delay_range,front,archetypes) in expected_series.items():
        rule=dominators['series'][series]
        if rule.get('controller')!=controller or \
                tuple(rule.get('arrival_delay_days_range',[]))!=delay_range or \
                rule.get('entry_front')!=front or \
                set(rule.get('preferred_archetypes',[]))!=archetypes or not rule.get('route'):
            raise SystemExit(f'invalid Second Home {series} invasion rule')
        prefix=series.title()
        if rule.get('boss_snapshot_variable')!=f'vCE_OldArm{prefix}Boss' or \
                rule.get('killer_snapshot_variable')!=f'vCE_OldArm{prefix}Killer':
            raise SystemExit(f'invalid Second Home {series} threat snapshot binding')
        invasion_fronts.append(archetypes)
    if any(left & right for index,left in enumerate(invasion_fronts)
           for right in invasion_fronts[index+1:]):
        raise SystemExit('Dominator series must enter Second Home from separate fronts')

    pirate_ids={x['id'] for x in pirate['states']}
    expected={'UNKNOWN','NOT_STARTED','CLAN_ACTIVE','COALITION_VICTORY','PIRATE_VICTORY','PLAYER_PIRATE'}
    if pirate_ids!=expected:
        raise SystemExit(f'pirate state set mismatch: {pirate_ids ^ expected}')

    ship_roster=load('second_home_ship_roles.json')
    base_roster=load('second_home_bases.json')

    all_ship_ids=[]
    required_ship_roles={x['id'] for x in ship_roster['roles']}
    for faction in ship_roster['factions']:
        ships=faction['ships']
        roles={x['role'] for x in ships}
        if roles!=required_ship_roles:
            raise SystemExit(f"ship role mismatch for {faction['faction_id']}: {roles ^ required_ship_roles}")
        all_ship_ids.extend(x['ship_id'] for x in ships)
    if len(all_ship_ids)<40 or len(all_ship_ids)!=len(set(all_ship_ids)):
        raise SystemExit('Second Home must define at least 40 unique racial ship hulls')

    all_base_ids=[]
    required_base_roles={x['id'] for x in base_roster['roles']}
    for faction in base_roster['factions']:
        bases=faction['bases']
        roles={x['role'] for x in bases}
        if roles!=required_base_roles:
            raise SystemExit(f"base role mismatch for {faction['faction_id']}: {roles ^ required_base_roles}")
        all_base_ids.extend(x['base_id'] for x in bases)
    if len(all_base_ids)<24 or len(all_base_ids)!=len(set(all_base_ids)):
        raise SystemExit('Second Home must define at least 24 unique racial bases')

    print(
        f'OK: {len(factions)} factions, {len(chars)} characters, {len(missions)} missions, '
        f'{len(variables)} variables, {len(items)} transit items, '
        f'{len(second["required_nodes"])} required Second Home nodes, '
        f'{len(sector_pool)} canonical sector names, '
        f'{len(system_pool)} procedural system names, '
        f'{len(all_ship_ids)} unique racial ship classes, {len(all_base_ids)} unique racial bases'
    )

if __name__=='__main__': main()
