"""Dump pyfa's tank / capacitor / fitting results as the reference for the C++ engine.

Runs pyfa's own code (needs pyfa's Python environment, including wxPython) on a
*temporary copy* of saveddata.db, so your fits and settings are never touched.
In the copy, every fit is normalized to what the C++ engine v1 supports:

  - character "All 5", implants/boosters removed (fit and character implants),
    unless --with-implants
  - projected fits/modules/drones/fighters and command fits removed
  - damage profile: builtin Uniform
  - reload factored into cycle times (pyfa's "useGlobalForceReload")

Fits are loaded through pyfa's normal service path (the same as the GUI), which
matters: hand-built fits skip some 'early' runtime effects.

Output: one JSON object per line per fit, see dump_fit().

Usage (from anywhere, with pyfa's python):
    python cpp/tools/pyfa_reference.py out.jsonl [--fits ID ID ...] [--limit N]
                                       [--save path/to/saveddata.db] [--with-implants]
                                       [--keep-db normalized.db]

--keep-db saves the normalized copy, so the C++ engine can evaluate exactly the
same fits:  PyfaReader --save normalized.db --engine-dump engine.tsv
"""
import argparse
import json
import math
import os
import shutil
import sqlite3
import sys
import tempfile

PYFA_ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))

SHIP_ATTRS = (
    'shieldCapacity', 'armorHP', 'hp',
    'shieldEmDamageResonance', 'shieldThermalDamageResonance',
    'shieldKineticDamageResonance', 'shieldExplosiveDamageResonance',
    'armorEmDamageResonance', 'armorThermalDamageResonance',
    'armorKineticDamageResonance', 'armorExplosiveDamageResonance',
    'emDamageResonance', 'thermalDamageResonance', 'kineticDamageResonance', 'explosiveDamageResonance',
    'capacitorCapacity', 'rechargeRate',
    'cpuOutput', 'powerOutput', 'upgradeCapacity',
    'shieldRepair', 'armorRepair', 'hullRepair',  # fit.extraAttributes == ship attributes in pyfa
)

MODULE_ATTRS = (
    'cpu', 'power', 'upgradeCost',
    'capacitorNeed', 'duration', 'speed', 'durationHighisGood', 'moduleReactivationDelay',
    'shieldBonus', 'armorDamageAmount', 'structureDamageAmount', 'chargedArmorDamageMultiplier',
    'reloadTime', 'capacity', 'chargeRate',
)

CHARGE_ATTRS = ('volume', 'capacitorBonus')


def normalize_save_db(path, keep_implants=False):
    """Strip everything the C++ engine doesn't model from the copied database."""
    db = sqlite3.connect(path)
    all5 = db.execute("SELECT ID FROM characters WHERE name = 'All 5'").fetchone()
    if not keep_implants:
        db.executescript("""
            DELETE FROM fitImplants;
            DELETE FROM boosters;
            DELETE FROM boosterSideEffects;
            UPDATE fits SET implantLocation = 0;
        """)
    db.executescript("""
        DELETE FROM projectedFits;
        DELETE FROM commandFits;
        DELETE FROM modules WHERE projected = 1;
        DELETE FROM drones WHERE projected = 1;
        DELETE FROM fighters WHERE projected = 1;
        UPDATE fits SET damagePatternID = NULL, builtinDamagePatternID = -1;
    """)
    # All 5 is created by pyfa on demand; fits pointing elsewhere get it after load.
    if all5:
        db.execute('UPDATE fits SET characterID = ?', all5)
    db.commit()
    db.close()


def num(v):
    if v is None:
        return None
    v = float(v)
    return None if math.isnan(v) else (str(v) if math.isinf(v) else v)


def dump_fit(fit, svc_fit):
    from eos.const import FittingModuleState
    from eos.utils.spoolSupport import SpoolOptions, SpoolType
    from eos.utils.stats import RRTypes

    ship = fit.ship
    remote = RRTypes(0, 0, 0, 0)
    for mod in fit.modules:  # modules only, as Fit.getRemoteReps without drones
        remote += mod.getRemoteReps(spoolOptions=SpoolOptions(SpoolType.SPOOL_SCALE, 1, True))
    out = {
        'fitID': fit.ID,
        'name': fit.name,
        'shipTypeID': ship.item.ID,
        'modeTypeID': fit.mode.item.ID if fit.mode else None,
        'character': fit.character.name if fit.character else None,
        'factorReload': fit.factorReload,
        'ship': {a: num(ship.getModifiedItemAttr(a)) for a in SHIP_ATTRS},
        'used': {'cpu': num(fit.cpuUsed), 'power': num(fit.pgUsed), 'calibration': num(fit.calibrationUsed)},
        'hp': {k: num(v) for k, v in fit.hp.items()},
        'ehp': {k: num(v) for k, v in fit.ehp.items()},
        'cap': {
            'peakRecharge': num(fit.calculateCapRecharge()),
            'recharge': num(fit.capRecharge),  # peak recharge + injectors
            'used': num(fit.capUsed),
            'stable': bool(fit.capStable),
        },
        'sustainable': {k: num(v) for k, v in fit.sustainableTank.items()},
        'implants': [{'typeID': i.item.ID, 'active': bool(i.active)} for i in fit.appliedImplants],
        'boosters': [{'typeID': b.item.ID, 'active': bool(b.active),
                      'sideEffects': [se.effectID for se in b.sideEffects if se.active]} for b in fit.boosters],
        'effectiveSustainable': {k: num(v) for k, v in fit.effectiveSustainableTank.items()},
        # Outgoing remote repair of the modules (HP/s), full spool-up as pyfa's default; reload factored.
        'remote': {k: num(getattr(remote, k)) for k in ('shield', 'armor')},
        'modules': [],
    }
    for pos, mod in enumerate(fit.modules):
        if mod.isEmpty:
            continue
        cycle = mod.getCycleParameters()
        entry = {
            'position': pos,
            'typeID': mod.item.ID,
            'baseTypeID': mod.baseItem.ID if mod.isMutated else None,
            'mutaplasmidID': mod.mutaplasmid.ID if mod.isMutated else None,
            'chargeID': mod.charge.ID if mod.charge else None,
            'state': int(mod.state),
            'slot': int(mod.slot) if mod.slot is not None else None,
            'attrs': {a: num(mod.getModifiedItemAttr(a)) for a in MODULE_ATTRS
                      if mod.getModifiedItemAttr(a) is not None},
            'rawCycleTime': num(mod.rawCycleTime),
            'reactivationDelay': num(mod.reactivationDelay),
            'numShots': mod.numShots,
            'moduleReloadTime': num(mod.reloadTime),
            'averageCycleTime': num(cycle.averageTime) if cycle is not None else None,
            'capUse': num(mod.capUse) if mod.state >= FittingModuleState.ACTIVE else 0.0,
        }
        if mod.charge:
            entry['chargeAttrs'] = {a: num(mod.getModifiedChargeAttr(a)) for a in CHARGE_ATTRS
                                    if mod.getModifiedChargeAttr(a) is not None}
        out['modules'].append(entry)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('output')
    ap.add_argument('--fits', type=int, nargs='*', help='only these fit IDs')
    ap.add_argument('--limit', type=int, help='stop after this many fits')
    ap.add_argument('--with-implants', action='store_true', help='keep implants, boosters and side effects')
    ap.add_argument('--keep-db', help='also save the normalized database here')
    ap.add_argument('--save', default=os.path.expanduser(os.path.join('~', '.pyfa', 'saveddata.db')),
                    help='saveddata.db to copy (default: %(default)s)')
    args = ap.parse_args()

    tmp = tempfile.mkdtemp(prefix='pyfa_reference_')
    try:
        shutil.copy2(args.save, os.path.join(tmp, 'saveddata.db'))
        normalize_save_db(os.path.join(tmp, 'saveddata.db'), keep_implants=args.with_implants)
        if args.keep_db:
            shutil.copy2(os.path.join(tmp, 'saveddata.db'), args.keep_db)

        sys.path.insert(0, PYFA_ROOT)
        os.chdir(PYFA_ROOT)
        import config
        config.defPaths(customSavePath=tmp)

        # pyfa's services expect a wx app and its normal import order.
        import wx
        app = wx.App(False)  # noqa: F841
        import eos.db
        import eos.events  # noqa: F401
        import service.prefetch  # noqa: F401
        import gui.mainFrame  # noqa: F401
        from service.port.eft import exportEft  # noqa: F401  (breaks a circular import)
        from service.fit import Fit as SvcFit
        from eos.saveddata.character import Character

        svc_fit = SvcFit.getInstance()
        svc_fit.serviceFittingOptions['useGlobalForceReload'] = True
        all5 = Character.getAll5()

        ids = args.fits or [f.ID for f in eos.db.getFitList()]
        if args.limit:
            ids = ids[:args.limit]

        written = failed = 0
        with open(args.output, 'w', encoding='utf-8', newline='\n') as f:
            for fit_id in ids:
                try:
                    fit = svc_fit.getFit(fit_id)
                    if fit is None:
                        continue
                    if fit.character is None or fit.character.name != 'All 5':
                        fit.character = all5
                        svc_fit.recalc(fit)
                    f.write(json.dumps(dump_fit(fit, svc_fit), ensure_ascii=False) + '\n')
                    written += 1
                except Exception as e:  # keep going; report at the end
                    failed += 1
                    print(f'fit {fit_id}: {type(e).__name__}: {e}', file=sys.stderr)
        print(f'{written} fits written to {args.output}, {failed} failed')
    finally:
        try:
            eos.db.saveddata_session.close()  # noqa: F821  (release the copy before deleting)
        except Exception:
            pass
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    main()
