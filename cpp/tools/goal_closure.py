"""Which attributes and modifier rows a goal depends on (analysis helper).

Walks the effect-modifier table backwards from a goal's ship/module attributes:
any modifier targeting a needed attribute makes its source attribute needed too.

Usage: python goal_closure.py [effect_modifiers.tsv] [--unresolved]
"""
import collections
import csv
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TSV = os.path.join(HERE, '..', 'PyfaReader', 'data', 'effect_modifiers.tsv')

RES = ('Em', 'Thermal', 'Kinetic', 'Explosive')
CAP = [('ship', 'capacitorCapacity'), ('ship', 'rechargeRate')] + \
      [('module', a) for a in ('capacitorNeed', 'duration', 'durationHighisGood', 'speed', 'moduleReactivationDelay',
                               'reloadTime', 'capacity', 'chargeRate')] + \
      [('charge', 'volume'), ('charge', 'capacitorBonus')]
# Command bursts: the buff value is the burst module's warfareBuff{1..4}Value
# (pyfa addCommandBonus), modified by skills, ship bonuses and the charge.
BURSTS = [('module', f'warfareBuff{x}Value') for x in range(1, 5)]
# Reactive Armor Hardener: pyfa simulates it in code (effect 4928), so its rows
# are unresolved and the backward pass can't find these on its own.
RAH = [('module', 'resistanceShiftAmount')] + [('module', f'armor{x}DamageResonance')
                                               for x in ('Em', 'Thermal', 'Kinetic', 'Explosive')]
# Mutadaptive remote armor repairers: pyfa spools them up (default: fully) per cycle.
SPOOL = [('module', 'repairMultiplierBonusMax'), ('module', 'repairMultiplierBonusPerCycle')]
FITTING = [('ship', a) for a in ('cpuOutput', 'powerOutput', 'upgradeCapacity',
                                  # slot / hardpoint counts: T3 subsystems change them
                                  'hiSlots', 'medSlots', 'lowSlots', 'rigSlots',
                                  'turretSlotsLeft', 'launcherSlotsLeft')] + \
          [('module', a) for a in ('cpu', 'power', 'upgradeCost')]
GOALS = {
    'shieldPassive': [('ship', 'shieldCapacity')] + [('ship', f'shield{x}DamageResonance') for x in RES],
    'armorPassive': [('ship', 'armorHP')] + [('ship', f'armor{x}DamageResonance') for x in RES] + RAH,
    'hullPassive': [('ship', 'hp')] + [('ship', f'{x.lower()}DamageResonance') for x in RES],
    'shieldActive': [('ship', f'shield{x}DamageResonance') for x in RES] + [('module', 'shieldBonus')] + CAP,
    'armorActive': [('ship', f'armor{x}DamageResonance') for x in RES] +
                   [('module', 'armorDamageAmount'), ('module', 'chargedArmorDamageMultiplier')] + CAP + RAH,
    # Outgoing remote repair (pyfa Module.getRemoteReps), mutadaptive spool-up included.
    'shieldRemote': [('module', 'shieldBonus')] + CAP + SPOOL,
    'armorRemote': [('module', 'armorDamageAmount'), ('module', 'chargedArmorDamageMultiplier')] + CAP + SPOOL,
    # Not an optimization goal, but pyfa reports hull rep/s; cheap to keep it exact.
    'hullActive': [('ship', f'{x.lower()}DamageResonance') for x in RES] + [('module', 'structureDamageAmount')] + CAP,
}
# Owner kinds in v1 and the context pyfa runs their effects in.
OWNERS = {'ship': 'ship', 'skill': 'skill', 'module': 'module', 'charge': 'moduleCharge',
          'implant': 'implant', 'booster': 'booster'}


def unresolved(r):
    return bool(r['note'] or r['valueExpr'] or r['filter'].startswith('unparsed') or 'unparsed' in r['condition'])


def target_kind(r, owner):
    d = r['domain']
    if d in ('ship', 'extraAttributes'):
        return 'ship'
    if d == 'modules':
        return 'charge' if r['target'] == 'charge' else 'module'
    if d == 'appliedImplants':
        return 'implant'
    if d == 'boosters':
        return 'booster'
    if d == 'self':
        if owner in ('module', 'charge'):  # charge effects run with the module as owner
            return 'charge' if r['target'] == 'charge' else 'module'
        return owner
    return None


def source_kind(r, owner):
    s = r['sourceOf']
    if s == 'ship':
        return 'ship'
    if s.startswith('skill:') or owner == 'skill':
        return 'skill'
    if s == 'charge':
        return 'charge'
    return 'module' if owner == 'charge' else owner  # 'owner' of a charge effect is its module


def closure(rows, roots):
    edges = collections.defaultdict(list)
    for r in rows:
        for owner, ctx in OWNERS.items():
            if r['contexts'] != '*' and ctx not in r['contexts'].split('|'):
                continue
            k = target_kind(r, owner)
            if k and r['targetAttr']:
                edges[(k, r['targetAttr'])].append((r, owner))
    seen, stack, used = set(roots), list(roots), []
    while stack:
        for r, owner in edges.get(stack.pop(), []):
            used.append((r, owner))
            if r['sourceAttr']:
                nk = (source_kind(r, owner), r['sourceAttr'])
                if nk[0] != 'skill' and nk not in seen:
                    seen.add(nk)
                    stack.append(nk)
    return seen, used


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    rows = list(csv.DictReader(open(args[0] if args else TSV, encoding='utf-8'), delimiter='\t'))
    for goal, roots in GOALS.items():
        attrs, used = closure(rows, roots + FITTING + BURSTS)
        un = {(r['effectID'], r['effectName'], owner, r['targetAttr'], (r['note'] or r['valueExpr'] or r['filter']
               or r['condition'])[:90]) for r, owner in used if unresolved(r)}
        print(f"{goal}: attrs {dict(collections.Counter(k for k, _ in attrs))}, rows {len(used)}, unresolved {len(un)}")
        if '--unresolved' in sys.argv:
            for u in sorted(un):
                print('   ', *u)


if __name__ == '__main__':
    main()
