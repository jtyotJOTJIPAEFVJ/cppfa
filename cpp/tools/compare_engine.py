"""Compare the C++ fit engine (PyfaReader --engine-dump) against pyfa (pyfa_reference.py).

Usage:
    python compare_engine.py reference.jsonl engine.tsv [--eve path/to/eve.db] [--show N] [--key PREFIX]

The engine computes in float: values match when |a - b| <= 1e-4 * |b| + 1e-6,
or within 0.01 for cpu/power, which pyfa rounds to 2 decimals (a float can land on
the other side of the rounding boundary), 0.1 for the fit's cpu/power totals. Fits with active command
bursts are counted separately too; structures (citadels) are out of scope.
"""
import argparse
import collections
import json
import os
import sqlite3

HERE = os.path.dirname(os.path.abspath(__file__))
REL_TOL = 1e-4
ABS_TOL = 1e-6
ROUNDED_KEYS = {'module.cpu', 'module.power', 'ship.cpuOutput', 'ship.powerOutput'}
# Sums of per-module rounded values: each module can be 0.01 off.
ROUNDED_SUM_KEYS = {'used.cpu', 'used.power'}

# Reference keys compared: (section, key in reference, key in engine dump)
TOP = [
    ('used', k, f'used.{k}') for k in ('cpu', 'power', 'calibration')
] + [
    ('hp', k, f'hp.{k}') for k in ('shield', 'armor', 'hull')
] + [
    ('ehp', k, f'ehp.{k}') for k in ('shield', 'armor', 'hull')
] + [
    ('cap', k, f'cap.{k}') for k in ('peakRecharge', 'recharge', 'used')
] + [
    ('sustainable', k, f'sustainable.{k}') for k in ('shieldRepair', 'armorRepair', 'hullRepair')
] + [
    ('effectiveSustainable', k, f'effectiveSustainable.{k}') for k in ('shieldRepair', 'armorRepair', 'hullRepair')
]
# Outgoing remote repair: the engine's is sustained by the capacitor (makeRemoteProfile),
# pyfa's isn't, so it's compared on cap-stable fits only.
REMOTE = [('remote', k, f'remote.{k}') for k in ('shield', 'armor')]
MODULE_SCALARS = ('rawCycleTime', 'reactivationDelay', 'moduleReloadTime', 'averageCycleTime', 'capUse', 'numShots')


def close(a, b, key=''):
    if key in ROUNDED_KEYS and abs(a - b) <= 0.0101:
        return True
    if key in ROUNDED_SUM_KEYS and abs(a - b) <= 0.1:
        return True
    return abs(a - b) <= REL_TOL * abs(b) + ABS_TOL


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('reference')
    ap.add_argument('engine')
    ap.add_argument('--eve', default=os.path.normpath(os.path.join(HERE, '..', '..', 'eve.db')))
    ap.add_argument('--show', type=int, default=15, help='mismatches to print')
    ap.add_argument('--key', default='', help='only show mismatches whose key starts with this')
    args = ap.parse_args()

    engine = collections.defaultdict(dict)
    with open(args.engine, encoding='utf-8') as f:
        for line in f:
            fid, key, val = line.rstrip('\n').split('\t')
            engine[int(fid)][key] = float(val)

    db = sqlite3.connect(args.eve)
    group_of = dict(db.execute('SELECT t.typeID, g.name FROM invtypes t JOIN invgroups g USING(groupID)'))
    category_of = dict(db.execute('SELECT t.typeID, c.name FROM invtypes t JOIN invgroups g USING(groupID) '
                                  'JOIN invcategories c USING(categoryID)'))
    name_of = dict(db.execute('SELECT typeID, typeName FROM invtypes'))

    stats = collections.Counter()      # key -> mismatches
    compared = collections.Counter()   # key -> comparisons
    missing = collections.Counter()
    bad_fits, burst_fits, burst_ok, structure_fits, ok_fits = [], 0, 0, 0, 0
    examples = []
    for line in open(args.reference, encoding='utf-8'):
        ref = json.loads(line)
        fid = ref['fitID']
        got = engine.get(fid)
        has_burst = any(group_of.get(m['typeID']) == 'Command Burst' and m['state'] >= 1 for m in ref['modules'])
        if category_of.get(ref['shipTypeID']) == 'Structure':
            structure_fits += 1  # citadels: pyfa uses structure-only rules, out of scope
            continue
        if got is None:
            missing['<fit>'] += 1
            continue
        pairs = []
        for attr, v in ref['ship'].items():
            if f'ship.{attr}' in got:
                pairs.append((f'ship.{attr}', v, got[f'ship.{attr}']))
        for section, k, ek in TOP:
            if ref[section].get(k) is not None:
                pairs.append((ek, ref[section][k], got.get(ek)))
        if ref.get('remote') and ref['cap']['used'] is not None and ref['cap']['used'] <= ref['cap']['recharge']:
            for section, k, ek in REMOTE:
                if ref[section].get(k) is not None:
                    pairs.append((ek, ref[section][k], got.get(ek)))
        for m in ref['modules']:
            p = f"mod.{m['position']}."
            for attr, v in m['attrs'].items():
                ek = p + 'attrs.' + attr
                if ek in got:
                    pairs.append((f'module.{attr}', v, got[ek], m))
            for k in MODULE_SCALARS:
                if m.get(k) is not None:
                    pairs.append((f'module.{k}', m[k], got.get(p + k), m))

        fit_bad = False
        for entry in pairs:
            key, want, have = entry[0], entry[1], entry[2]
            if have is None:
                missing[key] += 1
                continue
            if isinstance(want, str):  # 'inf'
                want = float(want)
            compared[key] += 1
            if not close(have, want, key):
                fit_bad = True
                stats[key] += 1
                if key.startswith(args.key) and len(examples) < args.show:
                    mod = entry[3] if len(entry) > 3 else None
                    where = f" {name_of.get(mod['typeID'])} (state {mod['state']})" if mod else ''
                    examples.append(f"fit {fid} {ref['name']!r} [{name_of.get(ref['shipTypeID'])}]{where}: "
                                    f"{key} pyfa {want!r} engine {have!r}")
        if has_burst:
            burst_fits += 1
            burst_ok += not fit_bad
        if fit_bad:
            bad_fits.append(fid)
        else:
            ok_fits += 1

    total = ok_fits + len(bad_fits)
    print(f'fits: {ok_fits}/{total} fully match ({len(bad_fits)} differ), '
          f'of which with active command bursts: {burst_ok}/{burst_fits}; '
          f'{structure_fits} structures not counted')
    print(f'values: {sum(compared.values()) - sum(stats.values())}/{sum(compared.values())} match')
    if stats:
        print('mismatches by key:')
        for k, n in stats.most_common():
            print(f'  {k}: {n} of {compared[k]}')
    if missing:
        print('missing in engine output:', dict(missing.most_common(10)))
    for e in examples:
        print('  ' + e)


if __name__ == '__main__':
    main()
