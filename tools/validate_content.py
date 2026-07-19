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
    for v in variables:
        if v['type']=='int' and not v['min'] <= v['default'] <= v['max']:
            raise SystemExit(f"bad range {v['id']}")
        if v['type']=='enum' and v['default'] not in v['values']:
            raise SystemExit(f"bad enum {v['id']}")

    required_vars={'vCE_WarApartState','vCE_CurrentArm','vCE_AnchorState','vCE_AnchorInCargo','vCE_ResonanceCells','vCE_SecondHomeSeed'}
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
    system_names=display_names.get('systems',{})
    archetype_names=display_names.get('sector_archetypes',{})
    if set(system_names)!=set(second['required_nodes']):
        raise SystemExit('every required Second Home system must have a canonical display name')
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
    visible_names=list(system_names.values())+sector_names
    if any(not isinstance(name,str) or not name.strip() for name in visible_names):
        raise SystemExit('Second Home display names must be non-empty strings')
    if len(visible_names)!=len(set(visible_names)):
        raise SystemExit('Second Home system and sector display names must be unique')
    forbidden_name_fragments=('sector_','system_','todo','placeholder','test')
    if any(any(fragment in name.lower() for fragment in forbidden_name_fragments) for name in visible_names):
        raise SystemExit('technical placeholder leaked into a Second Home display name')

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
        f'{len(all_ship_ids)} unique racial ship classes, {len(all_base_ids)} unique racial bases'
    )

if __name__=='__main__': main()
