# ===============================================================================
# Copyright (C) 2010 Diego Duclos
#
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
# ===============================================================================

# A fit's targets (the Target tab): other fits and target profiles, at most one of
# them enabled. Changes go through the fit commands (gui.fitCommands, Gui*FitTarget*),
# which post GE.FitChanged for the fit whose target list changed.

import eos.db
from eos.saveddata.fitTarget import KIND_FIT, KIND_PROFILE
from eos.saveddata.targetProfile import TargetProfile as es_TargetProfile
from graphs.wrapper import TargetWrapper
from service.const import TargetResistMode
from service.fit import Fit


def getTargetKey(target):
    """Key of a fit or target profile as used by FitTarget.key and the fit commands"""
    if isinstance(target, es_TargetProfile):
        return KIND_PROFILE, target.ID
    return KIND_FIT, target.ID


def resolveTargetKey(key):
    """Fit or target profile for a target key, None if it doesn't exist (any more)"""
    kind, targetID = key
    if kind == KIND_FIT:
        fit = Fit.getInstance().getFit(targetID, basic=True)
        if fit is None or fit.isInvalid:
            return None
        return fit
    if kind == KIND_PROFILE:
        if targetID == es_TargetProfile.getIdeal().ID:
            return es_TargetProfile.getIdeal()
        if targetID < 0:
            return es_TargetProfile.getBuiltinById(targetID)
        return eos.db.getTargetProfile(targetID)
    return None


def getTargetFit(targetFit):
    """The target fit, calculated, so its attributes can be read"""
    sFit = Fit.getInstance()
    fit = sFit.getFit(targetFit.ID)
    if fit is not None and not fit.calculated:
        sFit.recalc(fit)
        sFit.fill(fit)
    return fit


def getActiveFitTarget(fit):
    """The fit's enabled FitTarget, or None. fit may be a fit or a fit ID"""
    if fit is None:
        return None
    if isinstance(fit, int):
        fit = Fit.getInstance().getFit(fit, basic=True)
        if fit is None:
            return None
    return fit.activeTarget


def getActiveTarget(fit):
    """
    The fit's enabled target as a dict, or None if no target is enabled. fit may be a
    fit or a fit ID.

        {'name': str, 'kind': 'profile' | 'fit',
         'em', 'thermal', 'kinetic', 'explosive': resists as fractions 0..1,
         'signatureRadius': m, 'maxVelocity': m/s, 'radius': m,
         'maxVelocityScrammed', 'signatureRadiusScrammed': the same with its MWD / MJD off,
         'webResistance', 'painterResistance': 1 = none, 'immune': bool}

    For a target fit the values are the fit's as pyfa calculates it (the fit is
    calculated first if needed); its resists are those of the layer pyfa's graphs pick
    in their default 'auto' resist mode. A profile without a signature radius has an
    infinite one (math.inf), as TargetProfile.signatureRadius returns.
    """
    fitTarget = getActiveFitTarget(fit)
    if fitTarget is None:
        return None
    target = fitTarget.target
    if fitTarget.kind == KIND_FIT:
        target = getTargetFit(target)
        if target is None:
            return None
    wrapper = TargetWrapper(item=target, lightnessID=None, lineStyleID=None)
    wrapper.resistMode = TargetResistMode.auto
    em, thermal, kinetic, explosive = wrapper.getResists()
    tackle = _tackleData(wrapper)
    return {
        'name': wrapper.name,
        'kind': fitTarget.kind,
        'em': float(em),
        'thermal': float(thermal),
        'kinetic': float(kinetic),
        'explosive': float(explosive),
        'signatureRadius': float(wrapper.getSigRadius()),
        'maxVelocity': float(wrapper.getMaxVelocity()),
        'radius': float(wrapper.getRadius() or 0),
        **tackle}


def _tackleData(wrapper):
    """How the attacker's tackle acts on the target, as pyfa's damage graph applies it
    (graphs/data/fitDamageStats/calc/projected.py): the speed and signature with its MWD / MJD
    off (what a scrambler in range leaves), its resistance to webs and target painters (1 =
    none, 0 = immune) and whether it is immune to offensive modules. Profiles: no MWD, no
    resistances."""
    from graphs.data.fitDamageStats.calc.projected import getScrammables
    data = {'maxVelocityScrammed': float(wrapper.getMaxVelocity()),
            'signatureRadiusScrammed': float(wrapper.getSigRadius()),
            'webResistance': 1.0, 'painterResistance': 1.0, 'immune': False}
    if not wrapper.isFit:
        return data
    ship = wrapper.item.ship
    scrammables = getScrammables(tgt=wrapper)
    if scrammables:
        data['maxVelocityScrammed'] = float(wrapper.getMaxVelocity(ignoreAfflictors=scrammables))
        data['signatureRadiusScrammed'] = float(wrapper.getSigRadius(ignoreAfflictors=scrammables))

    def resistance(name):
        # pyfa ModifiedAttributeDict: a missing resistance attribute means none
        value = ship.itemModifiedAttributes[name]
        return 1.0 if value is None else float(value)
    data['webResistance'] = resistance('stasisWebifierResistance')
    data['painterResistance'] = resistance('targetPainterResistance')
    data['immune'] = bool(ship.getModifiedItemAttr('disallowOffensiveModifiers'))
    return data
