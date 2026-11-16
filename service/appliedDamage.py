# =============================================================================
# This file is part of pyfa.
#
# pyfa is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# pyfa is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with pyfa.  If not, see <http://www.gnu.org/licenses/>.
# =============================================================================

"""
Applied damage of a fit against its enabled target (the Target tab), at the Target tab's
engagement: the distance and the share of its speed the target moves at, fully across
(the attacker stands still). It uses the damage graph's code
(graphs/data/fitDamageStats): the fit's damage per item, its webs, target painters and
scramblers on the target, and the application of each weapon, drone and fighter ability;
lock range and drone control range count. The module optimizer's applied damage goal
models the same engagement (cpp/FIT_ENGINE.md, "Damage").
"""

from contextlib import contextmanager
from copy import copy

from logbook import Logger

pyfalog = Logger(__name__)


def engagement():
    """(distance in m, target speed factor 0..1) as set in the Target tab."""
    from service.settings import ModuleOptimizerSettings
    settings = ModuleOptimizerSettings.getInstance()
    distance = max(0.0, float(settings.get('distance') or 0))
    speedFactor = min(1.0, max(0.0, float(settings.get('targetSpeed') or 0) / 100.0))
    return distance, speedFactor


def setEngagement(distance=None, speedFactor=None):
    from service.settings import ModuleOptimizerSettings
    settings = ModuleOptimizerSettings.getInstance()
    if distance is not None:
        settings.set('distance', max(0.0, float(distance)))
    if speedFactor is not None:
        settings.set('targetSpeed', min(100.0, max(0.0, float(speedFactor) * 100)))


@contextmanager
def _graphRules():
    """The graph settings the application code reads, as the optimizer models them (lock
    range and drone control range limit damage, drones keep up with slower targets),
    restored afterwards so the graph window keeps the user's choices."""
    from service.const import GraphDpsDroneMode
    from service.settings import GraphSettings
    settings = GraphSettings.getInstance()
    wanted = {'ignoreLockRange': False, 'ignoreDCR': False, 'mobileDroneMode': GraphDpsDroneMode.auto}
    saved = {key: settings.get(key) for key in wanted}
    try:
        for key, value in wanted.items():
            settings.set(key, value)
        yield
    finally:
        for key, value in saved.items():
            settings.set(key, value)


class Part:
    """Damage per second of a group of items: raw (no resists, no application), after the
    target's resists, and applied (resists and application)."""

    def __init__(self):
        self.raw = 0.0
        self.resisted = 0.0
        self.applied = 0.0

    @property
    def resistShare(self):
        """Share of the raw damage left after the target's resists (0..1)."""
        return self.resisted / self.raw if self.raw else 0.0

    @property
    def applicationShare(self):
        """Share of the resisted damage that applies (0..1)."""
        return self.applied / self.resisted if self.resisted else 0.0


class AppliedDamage:
    """weapons (modules), drones (drones and fighters) and total, Parts; target: the target's
    name; distance (m), speedFactor; targetSpeed, targetSigRadius: the target as tackled."""

    def __init__(self):
        self.weapons = Part()
        self.drones = Part()
        self.total = Part()
        self.target = ''
        self.distance = 0.0
        self.speedFactor = 1.0
        self.targetSpeed = 0.0
        self.targetSigRadius = 0.0


def getAppliedDamage(fit, distance=None, speedFactor=None):
    """AppliedDamage of fit against its enabled target, None without one (or on failure)."""
    from eos.saveddata.module import Module
    from eos.saveddata.targetProfile import TargetProfile
    from graphs.data.fitDamageStats.cache.projected import ProjectedDataCache
    from graphs.data.fitDamageStats.calc.application import getApplicationPerKey
    from graphs.data.fitDamageStats.calc.projected import (
        getScramRange, getScrammables, getSigRadiusMult, getTackledSpeed)
    from graphs.data.fitDamageStats.getter import YDpsMixin
    from graphs.wrapper import SourceWrapper, TargetWrapper
    from service.const import TargetResistMode
    from service.target import KIND_FIT, getActiveFitTarget, getTargetFit

    if fit is None:
        return None
    fitTarget = getActiveFitTarget(fit)
    if fitTarget is None:
        return None
    item = fitTarget.target
    if fitTarget.kind == KIND_FIT:
        item = getTargetFit(item)
    if item is None:
        return None
    if distance is None or speedFactor is None:
        d, s = engagement()
        distance = d if distance is None else distance
        speedFactor = s if speedFactor is None else speedFactor
    try:
        with _graphRules():
            src = SourceWrapper(item=fit, colorID=None)
            tgt = TargetWrapper(item=item, lightnessID=None, lineStyleID=None)
            tgt.resistMode = TargetResistMode.auto
            cache = ProjectedDataCache()
            webMods, tpMods = cache.getProjModData(src)
            webDrones, tpDrones = cache.getProjDroneData(src)
            webFighters, tpFighters = cache.getProjFighterData(src)
            scramRange = getScramRange(src=src)
            scrammables = getScrammables(tgt=tgt)
            speed = (tgt.getMaxVelocity() or 0) * speedFactor
            tgtSpeed = getTackledSpeed(
                src=src, tgt=tgt, currentUntackledSpeed=speed, srcScramRange=scramRange,
                tgtScrammables=scrammables, webMods=webMods, webDrones=webDrones, webFighters=webFighters,
                distance=distance)
            tgtSig = tgt.getSigRadius() * getSigRadiusMult(
                src=src, tgt=tgt, tgtSpeed=tgtSpeed, srcScramRange=scramRange, tgtScrammables=scrammables,
                tpMods=tpMods, tpDrones=tpDrones, tpFighters=tpFighters, distance=distance)
            application = getApplicationPerKey(
                src=src, tgt=tgt, atkSpeed=0, atkAngle=0, distance=distance, tgtSpeed=tgtSpeed, tgtAngle=90,
                tgtSigRadius=tgtSig)
            dpsMap = YDpsMixin._getDamagePerKey(None, src=src, time=None)
            em, thermal, kinetic, explosive = tgt.getResists()
            profile = TargetProfile(emAmount=em, thermalAmount=thermal, kineticAmount=kinetic,
                                    explosiveAmount=explosive, hp=tgt.getFullHp())
            out = AppliedDamage()
            for key, dps in dpsMap.items():
                part = out.weapons if isinstance(key, Module) else out.drones
                raw = dps.total
                # On a copy: some damage objects are pyfa's cached ones
                resistedDps = copy(dps)
                resistedDps.profile = profile
                resisted = resistedDps.total
                mult = application.get(key, 0)
                for p in (part, out.total):
                    p.raw += raw
                    p.resisted += resisted
                    p.applied += resisted * mult
    except Exception:
        pyfalog.exception('Failed to compute applied damage')
        return None
    out.target = tgt.name
    out.distance = distance
    out.speedFactor = speedFactor
    out.targetSpeed = tgtSpeed
    out.targetSigRadius = tgtSig
    return out
