"""Dump pyfa's tank / capacitor / fitting / damage results as the reference for the C++ engine.

Runs pyfa's own code (needs pyfa's Python environment, including wxPython) on a
*temporary copy* of saveddata.db, so your fits and settings are never touched.
In the copy, every fit is normalized to what the C++ engine v1 supports:

  - character "All 5", implants/boosters removed (fit and character implants),
    unless --with-implants
  - projected fits/modules/drones/fighters and command fits removed
  - damage profile: builtin Uniform
  - reload factored into cycle times (pyfa's "useGlobalForceReload"), unless --no-reload
  - spool-up: pyfa's default (globalDefaultSpoolupPercentage) set to --spool (default 1);
    modules with their own spool setting keep it

Fits are loaded through pyfa's normal service path (the same as the GUI), which
matters: hand-built fits skip some 'early' runtime effects.

Output: one JSON object per line per fit, see dump_fit().

Usage (from anywhere, with pyfa's python):
    python cpp/tools/pyfa_reference.py out.jsonl [--fits ID ID ...] [--limit N]
                                       [--save path/to/saveddata.db] [--with-implants]
                                       [--keep-db normalized.db] [--no-reload] [--spool FRACTION]

--keep-db saves the normalized copy, so the C++ engine can evaluate exactly the
same fits:  PyfaReader --save normalized.db --engine-dump engine.tsv [--no-reload] [--spool FRACTION]
(with the same --no-reload / --spool as here)
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

# Applied damage is reported against these, the same as the engine's referenceTargets()
# (src/main.cpp): name, resists em/thermal/kinetic/explosive, signature, speed, radius, distance.
REFERENCE_TARGETS = (
    ('frigate', (0.3, 0.4, 0.5, 0.6), 35, 3000, 40, 2000),
    ('cruiser', (0.5, 0.45, 0.4, 0.35), 125, 900, 120, 15000),
    ('battleship', (0.25, 0.35, 0.45, 0.55), 420, 120, 300, 45000),
)


def dump_damage(fit):
    """Raw and applied damage per second of the modules and drones (fighters left out).

    Applied: pyfa's damage graph (getApplicationPerKey) with the attacker stationary, the
    target moving at its speed fully transversal, lock range and drone control range
    checked, the fit's webs, target painters and scramblers applied to the target as with
    the graph's "apply projected" (getTackledSpeed, getSigRadiusMult; fighters left out);
    spool-up as spoolOptions()."""
    from eos.saveddata.targetProfile import TargetProfile
    from graphs.data.fitDamageStats.calc.application import getApplicationPerKey
    from graphs.data.fitDamageStats.calc.projected import getScramRange, getScrammables, getSigRadiusMult, getTackledSpeed
    from graphs.data.fitDamageStats.cache.projected import ProjectedDataCache
    from graphs.wrapper import SourceWrapper, TargetWrapper
    from service.settings import GraphSettings
    from service.const import GraphDpsDroneMode

    settings = GraphSettings.getInstance()
    settings.set('ignoreLockRange', False)
    settings.set('ignoreDCR', False)
    settings.set('mobileDroneMode', GraphDpsDroneMode.auto)
    spool = spoolOptions()
    mods = [m for m in fit.modules if not m.isEmpty]
    drones = [d for d in fit.drones if d.amountActive > 0]
    out = {
        'dps': {'modules': sum(m.getDps(spoolOptions=spool).total for m in mods),
                'drones': sum(d.getDps().total for d in drones)},
        'applied': {},
        'moduleDps': {},
        'moduleApplied': {},
    }
    for pos, mod in enumerate(fit.modules):
        if not mod.isEmpty:
            out['moduleDps'][pos] = mod.getDps(spoolOptions=spool).total
    src = SourceWrapper(fit, 0)
    cache = ProjectedDataCache()
    webMods, tpMods = cache.getProjModData(src)
    webDrones, tpDrones = cache.getProjDroneData(src)
    scramRange = getScramRange(src=src)
    for name, (em, th, ki, ex), sig, speed, radius, distance in REFERENCE_TARGETS:
        profile = TargetProfile(em, th, ki, ex, maxVelocity=speed, signatureRadius=sig, radius=radius)
        tgt = TargetWrapper(profile, 0, 0)
        scrammables = getScrammables(tgt=tgt)
        tgtSpeed = getTackledSpeed(src=src, tgt=tgt, currentUntackledSpeed=speed, srcScramRange=scramRange,
                                   tgtScrammables=scrammables, webMods=webMods, webDrones=webDrones,
                                   webFighters=[], distance=distance)
        tgtSig = tgt.getSigRadius() * getSigRadiusMult(
            src=src, tgt=tgt, tgtSpeed=tgtSpeed, srcScramRange=scramRange, tgtScrammables=scrammables,
            tpMods=tpMods, tpDrones=tpDrones, tpFighters=[], distance=distance)
        app = getApplicationPerKey(src=src, tgt=tgt, atkSpeed=0, atkAngle=0, distance=distance, tgtSpeed=tgtSpeed,
                                   tgtAngle=90, tgtSigRadius=tgtSig)

        def resisted(dps):
            # getDps(targetProfile=...) drops the profile while summing; pyfa's fit totals set it afterwards
            dps.profile = profile
            return dps.total

        modTotal = 0
        for pos, mod in enumerate(fit.modules):
            if mod.isEmpty:
                continue
            value = resisted(mod.getDps(spoolOptions=spool)) * app.get(mod, 0)
            modTotal += value
            if out['moduleDps'][pos] > 0:
                out['moduleApplied'].setdefault(pos, {})[name] = value
        droneTotal = sum(resisted(d.getDps()) * app.get(d, 0) for d in drones)
        out['applied'][name] = {'modules': modTotal, 'drones': droneTotal}
    return out


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


SPOOL = 1.0  # main(): --spool


def spoolOptions():
    """pyfa's default spool-up, not forced: a module's own spool setting wins (as pyfa's stats panels)."""
    from eos.utils.spoolSupport import SpoolOptions, SpoolType
    return SpoolOptions(SpoolType.SPOOL_SCALE, SPOOL, False)


def dump_fit(fit, svc_fit):
    from eos.const import FittingModuleState
    from eos.utils.stats import RRTypes

    ship = fit.ship
    remote = RRTypes(0, 0, 0, 0)
    for mod in fit.modules:  # modules only, as Fit.getRemoteReps without drones
        remote += mod.getRemoteReps(spoolOptions=spoolOptions())
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
        # Outgoing remote repair of the modules (HP/s), spool-up as spoolOptions().
        'remote': {k: num(getattr(remote, k)) for k in ('shield', 'armor')},
        'modules': [],
        'damage': dump_damage(fit),
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
    ap.add_argument('--no-reload', action='store_true', help="don't factor reload into cycle times")
    ap.add_argument('--spool', type=float, default=1.0, help="pyfa's default spool-up, 0..1 (default 1)")
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
        svc_fit.serviceFittingOptions['useGlobalForceReload'] = not args.no_reload
        global SPOOL
        SPOOL = args.spool
        import eos.config
        eos.config.settings['globalDefaultSpoolupPercentage'] = args.spool
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
