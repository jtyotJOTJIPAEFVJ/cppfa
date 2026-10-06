"""Generate the compact fit-engine schema (C++ header) from eve.db + effect_modifiers.tsv.

The fit engine only stores attributes that the supported goals (tank, fitting,
capacitor; see goal_closure.py) depend on. This script decides which ones:

  Attr enum      every module/charge attribute in any goal's closure, plus the
                 ship attributes that can change per fit (goal attributes and
                 anything module/charge effects modify or read on the ship).
                 Ship bonus amounts (shipBonusMC2, ...) are deliberately left
                 out: they are constant for a ship and get folded in at setup.
  ShipAttrs      float per ship Attr.
  <Group>Attrs   one float-only struct per module/charge group, holding the
                 Attrs that group's items have or that relevant modifiers can
                 target on them. Groups with identical layouts share a struct.

Each struct also gets a constexpr field list (Attr per field, in order), so
generic engine code can address fields by Attr while hand-written code uses
names (booster.shieldBonus).

Usage: python gen_fit_schema.py [eve.db] [effect_modifiers.tsv] [output.h]
"""
import collections
import csv
import json
import os
import re
import sqlite3
import sys

import goal_closure

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_DB = os.path.normpath(os.path.join(HERE, '..', '..', 'eve.db'))
DEFAULT_TSV = os.path.normpath(os.path.join(HERE, '..', 'PyfaReader', 'data', 'effect_modifiers.tsv'))
DEFAULT_OUT = os.path.normpath(os.path.join(HERE, '..', 'PyfaReader', 'src', 'generated', 'FitSchema.generated.h'))

MODULE_CATEGORIES = ('Module', 'Subsystem')
CHARGE_CATEGORIES = ('Charge',)
# Ship-mode items (T3 destroyers) live in category Ship Modifiers; they act like modules.
MODE_CATEGORIES = ('Ship Modifiers',)


def identifier(name):
    """'Shield Booster' -> 'ShieldBooster', '125mm ...' -> 'G125mm...'."""
    s = re.sub(r'[^0-9A-Za-z]+', ' ', name).title().replace(' ', '')
    return s if s and not s[0].isdigit() else 'G' + s


def load_rows(path):
    return list(csv.DictReader(open(path, encoding='utf-8'), delimiter='\t'))


def module_owned(owner):
    return owner in ('module', 'charge')


def filter_can_match(r, item, skills_of, group_name_of, attrs_of, owner_skill_ids):
    """Could a modifier row's filter select this item (as module, or its charge)?"""
    text = r['filter']
    if not text:
        return True
    if text.startswith('unparsed:'):
        return True  # be generous: keep the field
    for clause in text.split('&'):
        ok = False
        for term in clause.split('|'):
            if term.startswith('charge.'):
                ok = True  # depends on the loaded charge, decided per fit
                break
            kind, _, value = term.partition(':')
            if kind == 'skill' and value in skills_of(item):
                ok = True
            elif kind == 'ownerSkill' and owner_skill_ids & skills_of(item, ids=True):
                ok = True
            elif kind == 'group' and group_name_of(item) == value:
                ok = True
            elif kind == 'hasAttr' and value in attrs_of(item):
                ok = True
            if ok:
                break
        if not ok:
            return False
    return True


def main():
    db_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_DB
    tsv_path = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_TSV
    out_path = sys.argv[3] if len(sys.argv) > 3 else DEFAULT_OUT

    rows = load_rows(tsv_path)
    db = sqlite3.connect(db_path)

    # --- which attributes the goals depend on --------------------------------
    needed = set()
    used = []
    for roots in goal_closure.GOALS.values():
        attrs, u = goal_closure.closure(rows, roots + goal_closure.FITTING + goal_closure.BURSTS)
        needed |= attrs
        used += u

    # Ship attributes that can differ between fits of the same ship.
    ship_dynamic = {a for k, a in needed if k == 'ship' and (k, a) in
                    {r for roots in goal_closure.GOALS.values() for r in roots + goal_closure.FITTING}}
    for r, owner in used:
        if not module_owned(owner):
            continue
        if goal_closure.target_kind(r, owner) == 'ship':
            ship_dynamic.add(r['targetAttr'])
        if r['sourceAttr'] and goal_closure.source_kind(r, owner) == 'ship':
            ship_dynamic.add(r['sourceAttr'])
    module_attrs = {a for k, a in needed if k == 'module'}
    charge_attrs = {a for k, a in needed if k == 'charge'}

    # --- eve.db lookups --------------------------------------------------------
    attr_ids = dict(db.execute('SELECT attributeName, attributeID FROM dgmattribs'))
    type_rows = db.execute("""
        SELECT t.typeID, t.typeName, g.groupID, g.name, c.name, IFNULL(t.reqskills, '')
        FROM invtypes t JOIN invgroups g USING(groupID) JOIN invcategories c USING(categoryID)
        WHERE c.name IN ({})""".format(','.join('?' * len(MODULE_CATEGORIES + CHARGE_CATEGORIES + MODE_CATEGORIES))),
                           MODULE_CATEGORIES + CHARGE_CATEGORIES + MODE_CATEGORIES).fetchall()
    type_attrs = collections.defaultdict(set)
    for tid, name in db.execute('SELECT v.typeID, a.attributeName FROM dgmtypeattribs v JOIN dgmattribs a USING(attributeID)'):
        type_attrs[tid].add(name)
    skill_names = dict(db.execute("""SELECT t.typeID, t.typeName FROM invtypes t JOIN invgroups g USING(groupID)
                                     JOIN invcategories c USING(categoryID) WHERE c.name = 'Skill'"""))
    skill_ids_by_name = {v: k for k, v in skill_names.items()}

    req = {tid: {int(k) for k in json.loads(rs)} if rs else set() for tid, _, _, _, _, rs in type_rows}
    group_of = {tid: gname for tid, _, _, gname, _, _ in type_rows}
    type_effects = collections.defaultdict(set)
    effect_owners = collections.defaultdict(set)
    for tid, eid in db.execute('SELECT typeID, effectID FROM dgmtypeeffects'):
        type_effects[tid].add(eid)
        effect_owners[eid].add(tid)

    def skills_of(tid, ids=False):
        return req[tid] if ids else {skill_names.get(s) for s in req[tid]}

    # Module-owned modifiers targeting module attributes, with the owner's skill ids
    # (for ownerSkill filters the owner is a skill: any skill could match, keep it).
    module_targeting = [(r, owner) for r, owner in used
                        if goal_closure.target_kind(r, owner) in ('module', 'charge')]

    groups = collections.OrderedDict()  # (kind, groupID) -> (group name, set of types)
    for tid, tname, gid, gname, cname, _ in type_rows:
        kind = 'charge' if cname in CHARGE_CATEGORIES else 'module'
        groups.setdefault((kind, gid), (gname, []))[1].append(tid)

    layouts = collections.OrderedDict()  # (kind, groupID) -> tuple of attrs
    for (kind, gid), (gname, tids) in groups.items():
        relevant = module_attrs if kind == 'module' else charge_attrs
        fields = set()
        for tid in tids:
            fields |= type_attrs[tid] & relevant
        for r, owner in module_targeting:
            a = r['targetAttr']
            if a not in relevant or a in fields:
                continue
            if goal_closure.target_kind(r, owner) != kind:
                continue
            eid = int(r['effectID'])
            if r['domain'] == 'self':
                # Only the item owning the effect (or, for charge effects, the module holding the charge).
                if owner == 'module':
                    candidates = [t for t in tids if eid in type_effects[t]]
                else:
                    candidates = [t for t in tids if 'chargeGroup1' in type_attrs[t]]
                if candidates:
                    fields.add(a)
                continue
            owner_skills = effect_owners[eid] & set(skill_names) if owner == 'skill' else set()
            if any(filter_can_match(r, t, skills_of, group_of.get, type_attrs.__getitem__, owner_skills)
                   for t in tids):
                fields.add(a)
        if fields:
            layouts[(kind, gid)] = tuple(sorted(fields))

    # --- Attr enum ---------------------------------------------------------------
    all_attrs = sorted(ship_dynamic | {a for f in layouts.values() for a in f})
    missing = [a for a in all_attrs if a not in attr_ids]
    all_attrs = [a for a in all_attrs if a in attr_ids]

    # --- emit ---------------------------------------------------------------------
    shapes = collections.OrderedDict()  # layout -> struct name
    group_struct = {}
    for (kind, gid), fields in layouts.items():
        gname = groups[(kind, gid)][0]
        if fields not in shapes:
            shapes[fields] = identifier(gname) + ('ChargeAttrs' if kind == 'charge' else 'Attrs')
        group_struct[(kind, gid)] = shapes[fields]

    ship_fields = tuple(sorted(a for a in ship_dynamic if a in attr_ids))
    lines = []
    w = lines.append
    w('// GENERATED by cpp/tools/gen_fit_schema.py from eve.db and effect_modifiers.tsv - do not edit.')
    w('// Regenerate after updating pyfa (game data or effects).')
    w('#pragma once')
    w('')
    w('#include <cstddef>')
    w('#include <cstdint>')
    w('#include <iterator>')
    w('')
    w('namespace fitschema')
    w('{')
    w('// Attributes the fit engine stores, dense IDs.')
    w('enum class Attr : uint16_t')
    w('{')
    for a in all_attrs:
        w(f'    {a},')
    w('};')
    w(f'inline constexpr size_t kAttrCount = {len(all_attrs)};')
    w('')
    w('// eve.db attributeID per Attr.')
    w('inline constexpr uint16_t kAttrEveID[kAttrCount] = {')
    for i in range(0, len(all_attrs), 12):
        w('    ' + ' '.join(f'{attr_ids[a]},' for a in all_attrs[i:i + 12]))
    w('};')
    w('')
    w('inline constexpr const char* kAttrName[kAttrCount] = {')
    for a in all_attrs:
        w(f'    "{a}",')
    w('};')
    w('')

    def emit_struct(name, fields, comment):
        w(f'// {comment}')
        w(f'struct {name}')
        w('{')
        for a in fields:
            w(f'    float {a} = 0.0f;')
        w('')
        w(f'    static constexpr Attr kFields[{len(fields)}] = {{')
        for i in range(0, len(fields), 4):
            w('        ' + ' '.join(f'Attr::{a},' for a in fields[i:i + 4]))
        w('    };')
        w('};')
        w(f'static_assert(sizeof({name}) == {len(fields)} * sizeof(float));')
        w('')

    emit_struct('ShipAttrs', ship_fields, 'Ship attributes that can change per fit.')
    by_struct = collections.defaultdict(list)
    for key, name in group_struct.items():
        by_struct[name].append(groups[key][0])
    for fields, name in shapes.items():
        emit_struct(name, fields, 'Groups: ' + ', '.join(sorted(by_struct[name])))

    w('// Group -> layout. Generic code uses fields/fieldCount; hand-written code casts to the struct.')
    w('struct GroupLayout')
    w('{')
    w('    uint32_t groupID;')
    w('    bool isCharge;')
    w('    const Attr* fields;')
    w('    uint8_t fieldCount;')
    w('};')
    w('inline constexpr GroupLayout kGroupLayouts[] = {')
    for (kind, gid), name in sorted(group_struct.items(), key=lambda kv: kv[0][1]):
        w(f'    {{{gid}, {"true" if kind == "charge" else "false"}, {name}::kFields, '
          f'static_cast<uint8_t>(std::size({name}::kFields))}},  // {groups[(kind, gid)][0]}')
    w('};')
    w('}  // namespace fitschema')
    w('')

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines))

    widths = [len(f) for f in layouts.values()]
    print(f'{len(all_attrs)} attrs ({len(ship_fields)} ship), {len(layouts)} groups -> {len(shapes)} structs, '
          f'fields per group: max {max(widths)}, avg {sum(widths) / len(widths):.1f} -> {out_path}')
    if missing:
        print('attributes not in eve.db (skipped):', ', '.join(missing))


if __name__ == '__main__':
    main()
