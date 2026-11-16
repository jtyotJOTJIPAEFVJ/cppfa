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
Module optimizer: runs the C++ optimizer (cpp/bin/Release/PyfaReader.exe) on a fit.

The request/response format is described in cpp/OPTIMIZER_CLI.md. This module has no
GUI code: it builds the request from a Fit, runs the exe in a subprocess and parses its
response. gui.moduleOptimizerDialog shows it.
"""

import json
import os
import platform
import subprocess
import sys
import threading
import time

from logbook import Logger

import config
from eos.const import FittingModuleState, FittingSlot

pyfalog = Logger(__name__)

REQUEST_VERSION = 1

# Goal names of the request, in the order the dialog shows them
GOALS = ('shieldPassive', 'armorPassive', 'hullPassive', 'shieldActive', 'armorActive', 'shieldResists',
         'armorResists', 'shieldRemote', 'armorRemote', 'localShieldRemoteShield', 'localShieldRemoteArmor',
         'localArmorRemoteShield', 'localArmorRemoteArmor', 'appliedDps')


def isDamageGoal(goal):
    """Damage per second applied to the fit's target (the Target tab), see OPTIMIZER_CLI.md."""
    return goal == 'appliedDps'


def isBalancedGoal(goal):
    """Local tank and remote repair together (the harmonic mean of the two, see OPTIMIZER_CLI.md)."""
    return goal.startswith('local')

# Only these slots can be optimized (high slots: remote repairers, weapons for the damage goal)
OPTIMIZABLE_SLOTS = (FittingSlot.HIGH, FittingSlot.MED, FittingSlot.LOW, FittingSlot.RIG)

SLOT_NAMES = {
    FittingSlot.LOW: 'low',
    FittingSlot.MED: 'med',
    FittingSlot.HIGH: 'high',
    FittingSlot.RIG: 'rig',
    FittingSlot.SUBSYSTEM: 'subsystem',
    FittingSlot.SERVICE: 'service'}
SLOTS_BY_NAME = {name: slot for slot, name in SLOT_NAMES.items()}
SLOTS_BY_NAME['mid'] = FittingSlot.MED

STATE_NAMES = {
    FittingModuleState.OFFLINE: 'offline',
    FittingModuleState.ONLINE: 'online',
    FittingModuleState.ACTIVE: 'active',
    FittingModuleState.OVERHEATED: 'overheated'}
STATES_BY_NAME = {name: state for state, name in STATE_NAMES.items()}


class OptimizerError(Exception):
    """Something the user should be told: the message is shown as it is."""


class OptimizerCancelled(Exception):
    pass


# -----------------------------------------------------------------------------
# Request
# -----------------------------------------------------------------------------

def isOptimizable(mod):
    return mod is not None and getattr(mod, 'slot', None) in OPTIMIZABLE_SLOTS


def buildCharacter(char):
    """The request's character: None (all skills at V) for All 5, otherwise every skill."""
    if char is None:
        return None
    name = getattr(char, 'savedName', None)
    if name == 'All 5':
        return None
    if name == 'All 0':
        return {'defaultLevel': 0, 'skills': []}
    from eos.saveddata.character import Character
    skills = []
    # Every skill there is: the character fills in unknown ones with its default level
    for skillID in sorted(Character.getSkillIDMap()):
        level = char.getSkill(skillID).level
        if level:
            skills.append({'skillID': skillID, 'level': int(level)})
    return {'defaultLevel': 0, 'skills': skills}


def buildModuleEntry(position, mod, optimize):
    slotName = SLOT_NAMES.get(mod.slot)
    if slotName is None:
        return None
    entry = {'position': position, 'slot': slotName}
    if mod.isEmpty:
        entry['typeID'] = 0
    elif mod.item is None or mod.isInvalid:
        # Like the engine: an unknown item is left out
        return None
    else:
        entry['typeID'] = mod.itemID
        entry['chargeID'] = mod.chargeID or 0
        entry['state'] = STATE_NAMES.get(mod.state, 'online')
        if mod.isMutated:
            entry['baseTypeID'] = mod.baseItemID
            entry['mutaplasmidID'] = mod.mutaplasmidID
        mutators = {str(m.attrID): m.value for m in mod.mutators.values()}
        if mutators:
            entry['mutators'] = mutators
        # The module's own spool-up setting (right-click > Spoolup); none = pyfa's default
        if getattr(mod, 'spoolType', None) is not None:
            entry['spoolType'] = int(mod.spoolType)
            entry['spoolAmount'] = float(mod.spoolAmount or 0)
    entry['optimize'] = bool(optimize)
    return entry


def buildDrones(fit):
    """The request's drones: the fit's drones in space (amountActive)."""
    drones = []
    for drone in fit.drones:
        if drone.item is None or drone.amountActive <= 0:
            continue
        entry = {'typeID': drone.itemID, 'amount': int(drone.amountActive)}
        if getattr(drone, 'isMutated', False):
            entry['baseTypeID'] = drone.baseItemID
            mutators = {str(m.attrID): m.value for m in drone.mutators.values()}
            if mutators:
                entry['mutators'] = mutators
        drones.append(entry)
    return drones


def buildTarget(target, distance, speedFactor=1.0):
    """The request's damage target from service.target.getActiveTarget's dict."""
    out = {'em': float(target['em']), 'thermal': float(target['thermal']), 'kinetic': float(target['kinetic']),
           'explosive': float(target['explosive']), 'maxVelocity': float(target.get('maxVelocity') or 0),
           'radius': float(target.get('radius') or 0), 'distance': max(0.0, float(distance or 0)),
           'speedFactor': min(1.0, max(0.0, float(speedFactor)))}
    # No signature (infinite, e.g. the ideal target) is left out: JSON has no infinity
    for key in ('signatureRadius', 'signatureRadiusScrammed'):
        sig = target.get(key)
        if sig is not None and sig > 0 and sig != float('inf'):
            out[key] = float(sig)
    if target.get('maxVelocityScrammed') is not None:
        out['maxVelocityScrammed'] = float(target['maxVelocityScrammed'])
    for key in ('webResistance', 'painterResistance'):
        if target.get(key) is not None:
            out[key] = float(target[key])
    out['immune'] = bool(target.get('immune', False))
    return out


def buildRequest(fit, positions, goal, priceCap=0, allowOfficer=False, results=10, threads=0,
                 prices=None, allSkillsV=False, timeBudget=60, allowAncillary=True, allowCapBoosters=True, maxReps=0,
                 requireCapStable=False, minCapRecharge=0, remoteWeight=4, target=None, distance=0, speedFactor=1.0,
                 excludePolarized=False):
    """
    The request dict for fit, with the modules (or empty slots) at positions (indices in
    fit.modules) open. prices: {typeID: ISK}; None reads pyfa's price cache.
    timeBudget: seconds the optimizer may spend improving its results, 0 for no limit.
    allSkillsV sends no character (all skills at V), for tests.
    Constraints: allowAncillary False leaves out ancillary repairers, allowCapBoosters False
    capacitor boosters; maxReps limits the
    goal's repairers in the fit (active and remote goals, 0 for no limit);
    requireCapStable and minCapRecharge (GJ/s, peak recharge plus cap boosters) are
    capacitor requirements. An option that misses one doesn't count as fitting.
    remoteWeight: balanced goals, the EHP/s one remote HP/s is worth (the rep target's resists).
    target: the damage goal's target, a service.target.getActiveTarget dict; distance (m,
    surface to surface) and speedFactor (share of its speed it moves at, fully transversal)
    the engagement; excludePolarized leaves polarized weapons out of the candidates.
    """
    if goal not in GOALS:
        raise ValueError('Unknown goal {}'.format(goal))
    if isDamageGoal(goal) and target is None:
        raise ValueError('The applied damage goal needs a target (enable one in the Target tab)')
    positions = set(positions)
    request = {'version': REQUEST_VERSION, 'ship': fit.ship.item.ID}
    if fit.mode is not None and fit.mode.item is not None:
        request['mode'] = fit.mode.item.ID
    character = None if allSkillsV else buildCharacter(fit.character)
    if character is not None:
        request['character'] = character
    request['implants'] = [{'typeID': i.itemID, 'active': bool(i.active)} for i in fit.appliedImplants]
    request['boosters'] = [
        {'typeID': b.itemID, 'active': bool(b.active), 'sideEffects': [e.ID for e in b.activeSideEffectEffects]}
        for b in fit.boosters]
    request['drones'] = buildDrones(fit)
    # pyfa's reload setting (Fit.factorReload, the global "factor in reload time") and default
    # spool-up (preferences > engine): the optimizer computes cycles, capacitor, repair and damage so
    import eos.config
    request['factorReload'] = bool(fit.factorReload)
    request['spoolup'] = min(1.0, max(0.0, float(eos.config.settings['globalDefaultSpoolupPercentage'])))
    modules = []
    for position, mod in enumerate(fit.modules):
        entry = buildModuleEntry(position, mod, position in positions)
        if entry is not None:
            modules.append(entry)
    request['modules'] = modules
    request['goal'] = goal
    request['priceCap'] = float(priceCap or 0)
    request['allowOfficer'] = bool(allowOfficer)
    request['allowAncillary'] = bool(allowAncillary)
    request['allowCapBoosters'] = bool(allowCapBoosters)
    request['excludePolarized'] = bool(excludePolarized)
    request['maxReps'] = max(0, int(maxReps or 0))
    request['requireCapStable'] = bool(requireCapStable)
    request['minCapRecharge'] = max(0.0, float(minCapRecharge or 0))
    request['remoteWeight'] = max(0.1, float(remoteWeight or 4))
    if target is not None:
        request['target'] = buildTarget(target, distance, speedFactor)
    request['results'] = int(results)
    request['threads'] = int(threads)
    request['timeBudget'] = max(0.0, float(timeBudget or 0))
    if prices is None:
        prices = readPrices()
    # Always sent, so the exe never reads saveddata itself
    request['prices'] = {str(typeID): price for typeID, price in prices.items()}
    return request


def readPrices():
    """pyfa's cached prices: {typeID: ISK}, for every type with a known price."""
    import eos.db
    from eos.db.saveddata.price import prices_table
    from sqlalchemy import select
    try:
        with eos.db.sd_lock:
            rows = eos.db.saveddata_session.execute(
                select(prices_table.c.typeID, prices_table.c.price).where(prices_table.c.price > 0)).fetchall()
    except Exception:
        pyfalog.exception('Failed to read cached prices')
        return {}
    return {int(typeID): float(price) for typeID, price in rows}


# -----------------------------------------------------------------------------
# Price catalog
# -----------------------------------------------------------------------------

# Prices older than this are fetched again by updatePrices (a week)
PRICE_MAX_AGE = 7 * 24 * 60 * 60
# Types per price request (the market sources put them in one URL)
PRICE_BATCH = 200


def catalogTypeIDs():
    """Published modules, charges and drones on the market: what the optimizer can propose."""
    import eos.db
    from sqlalchemy import text
    rows = eos.db.gamedata_session.execute(text(
        "SELECT t.typeID FROM invtypes t JOIN invgroups g ON g.groupID = t.groupID "
        "JOIN invcategories c ON c.categoryID = g.categoryID "
        "WHERE t.published = 1 AND t.marketGroupID IS NOT NULL AND c.name IN ('Module', 'Charge', 'Drone')")).fetchall()
    return [int(r[0]) for r in rows]


def readPriceRows():
    """pyfa's cached prices: {typeID: (price, time, status)}."""
    import eos.db
    from eos.db.saveddata.price import prices_table
    from sqlalchemy import select
    try:
        with eos.db.sd_lock:
            rows = eos.db.saveddata_session.execute(select(
                prices_table.c.typeID, prices_table.c.price, prices_table.c.time, prices_table.c.status)).fetchall()
    except Exception:
        pyfalog.exception('Failed to read cached prices')
        return {}
    return {int(t): (float(p or 0), float(tm or 0), int(st or 0)) for t, p, tm, st in rows}


def priceSummary(now=None):
    """How fresh the catalog's prices are: {'types', 'priced', 'medianAge', 'oldestAge', 'stale'}
    (ages in seconds, None without prices; stale: types updatePrices would fetch)."""
    from eos.saveddata.price import PriceStatus
    now = time.time() if now is None else now
    rows = readPriceRows()
    types = catalogTypeIDs()
    ages = sorted(now - rows[t][1] for t in types
                  if t in rows and rows[t][2] == PriceStatus.fetchSuccess and rows[t][0] > 0)
    return {'types': len(types), 'priced': len(ages),
            'medianAge': ages[len(ages) // 2] if ages else None, 'oldestAge': ages[-1] if ages else None,
            'stale': len(stalePriceTypes(rows=rows, types=types, now=now))}


def stalePriceTypes(maxAge=PRICE_MAX_AGE, rows=None, types=None, now=None):
    """Catalog types without a price fetched (or tried and not found) in the last maxAge seconds."""
    from eos.saveddata.price import PriceStatus
    now = time.time() if now is None else now
    rows = readPriceRows() if rows is None else rows
    types = catalogTypeIDs() if types is None else types
    stale = []
    for typeID in types:
        row = rows.get(typeID)
        if row is None or row[2] in (PriceStatus.initialized, PriceStatus.notSupported) or now - row[1] > maxAge:
            stale.append(typeID)
    return stale


class PriceUpdate:
    """
    Fetches pyfa's prices for typeIDs in batches, through pyfa's price service (its source
    and system settings), one batch after another. progress(done, total) and finished(cancelled)
    are called on the GUI thread (the price service calls back with wx.CallAfter).
    """

    def __init__(self, typeIDs, progress=None, finished=None, maxAge=PRICE_MAX_AGE, batch=PRICE_BATCH,
                 fetchTimeout=60):
        self.typeIDs = list(typeIDs)
        self.progress = progress
        self.finished = finished
        self.maxAge = maxAge
        self.batch = batch
        self.fetchTimeout = fetchTimeout
        self.done = 0
        self.cancelled = False
        self.running = False

    def start(self):
        self.running = True
        self._next()

    def cancel(self):
        self.cancelled = True

    def _next(self):
        if self.cancelled or self.done >= len(self.typeIDs):
            self.running = False
            if self.finished is not None:
                self.finished(self.cancelled)
            return
        from service.price import Price
        batch = self.typeIDs[self.done:self.done + self.batch]

        def onBatch(requests):
            self.done += len(batch)
            if self.progress is not None:
                self.progress(self.done, len(self.typeIDs))
            self._next()

        Price.getInstance().getPrices(batch, onBatch, fetchTimeout=self.fetchTimeout, validityOverride=self.maxAge)


def optionPrice(option, prices):
    """An option's ISK as the optimizer counts it (each module and its charge once, unknown = 0)."""
    total = 0.0
    for choice in option.slots:
        if choice.isEmpty:
            continue
        total += prices.get(choice.typeID, 0) or 0
        if choice.chargeID:
            total += prices.get(choice.chargeID, 0) or 0
    return total


def ignoredProjections(fit):
    """What pyfa applies to the fit that the optimizer leaves out: the Projected tab (fits,
    modules, drones, fighters) and command fits. A list of short descriptions, empty if none."""
    out = []
    counts = (
        (len(fit.projectedFits), 'projected fit', 'projected fits'),
        (len(fit.projectedModules), 'projected module', 'projected modules'),
        (sum(1 for d in fit.projectedDrones if getattr(d, 'amountActive', 1)), 'projected drone stack',
         'projected drone stacks'),
        (len(fit.projectedFighters), 'projected fighter squadron', 'projected fighter squadrons'),
        (len(fit.commandFits), 'command fit', 'command fits'))
    for count, one, many in counts:
        if count:
            out.append('{} {}'.format(count, one if count == 1 else many))
    return out


def fitMatchesRequest(fit, request):
    """Whether the fit's modules outside the open slots are still as they were in request."""
    if fit is None or fit.ship is None or fit.ship.item.ID != request.get('ship'):
        return False
    for entry in request['modules']:
        position = entry['position']
        if position >= len(fit.modules):
            return False
        mod = fit.modules[position]
        if entry['optimize']:
            if SLOT_NAMES.get(mod.slot) != entry['slot']:
                return False
            continue
        current = buildModuleEntry(position, mod, False)
        if current is None or current.get('typeID') != entry.get('typeID') \
                or current.get('chargeID', 0) != entry.get('chargeID', 0) \
                or current.get('state') != entry.get('state'):
            return False
    return True


# -----------------------------------------------------------------------------
# Response
# -----------------------------------------------------------------------------

def parseState(value):
    if value is None:
        return None
    if isinstance(value, int):
        return FittingModuleState(value)
    return STATES_BY_NAME.get(value, FittingModuleState.ONLINE)


class SlotChoice:
    """What an option puts in one requested slot. typeID 0 leaves it empty."""

    def __init__(self, data):
        self.position = data['position']
        self.slot = SLOTS_BY_NAME.get(data.get('slot'))
        self.typeID = data.get('typeID') or 0
        self.chargeID = data.get('chargeID') or None
        self.state = parseState(data.get('state'))
        self.name = data.get('name') or ''
        self.chargeName = data.get('chargeName') or ''
        self.unchanged = bool(data.get('unchanged'))
        # An owned mutated (abyssal) module from an opened slot: its base type and mutations
        self.baseTypeID = data.get('baseTypeID') or None
        self.mutators = {int(k): v for k, v in (data.get('mutators') or {}).items()}

    @property
    def isEmpty(self):
        return not self.typeID

    def __repr__(self):
        return 'SlotChoice({}: {} {} {}{})'.format(
            self.position, self.typeID, self.chargeID, self.state, ' unchanged' if self.unchanged else '')


class FitStats:

    def __init__(self, data):
        data = data or {}
        self.goal = data.get('goal', 0.0)
        self.cpuUsed = data.get('cpuUsed', 0.0)
        self.cpuOutput = data.get('cpuOutput', 0.0)
        self.powerUsed = data.get('powerUsed', 0.0)
        self.powerOutput = data.get('powerOutput', 0.0)
        self.calibrationUsed = data.get('calibrationUsed', 0.0)
        self.calibrationOutput = data.get('calibrationOutput', 0.0)
        self.capUsed = data.get('capUsed', 0.0)
        self.capRecharge = data.get('capRecharge', 0.0)
        self.capStable = bool(data.get('capStable', True))
        # Balanced goals: local tank (EHP/s) and remote repair (HP/s), each in its own test
        self.localRep = data.get('localRep')
        self.remoteRep = data.get('remoteRep')
        # Damage goal: raw damage per second (no resists, no application)
        self.dps = data.get('dps')
        self.fits = bool(data.get('fits', True))
        self.price = data.get('price')


class Option:

    def __init__(self, data):
        self.rank = data.get('rank', 0)
        self.goal = data.get('goal', 0.0)
        self.gain = data.get('gain', 0.0)
        self.fits = bool(data.get('fits', True))
        self.price = data.get('price', 0.0)
        self.priceDelta = data.get('priceDelta', 0.0)
        self.changedSlots = data.get('changedSlots', 0)
        self.stats = FitStats(data.get('stats'))
        self.slots = [SlotChoice(s) for s in data.get('slots', ())]

    def moduleNames(self, changedOnly=False, counted=False):
        """Names of the modules (with their charges); counted: "2x Name" for repeats."""
        names = []
        for choice in self.slots:
            if changedOnly and choice.unchanged:
                continue
            if choice.isEmpty:
                continue
            name = choice.name
            if choice.chargeName:
                name = '{} ({})'.format(name, choice.chargeName)
            names.append(name)
        if not counted:
            return names
        counts = {}
        for name in names:
            counts[name] = counts.get(name, 0) + 1
        return [name if count == 1 else '{}x {}'.format(count, name) for name, count in counts.items()]


class Shortfall:
    """One resource the pinned part (the modules outside the selection) is over on."""

    def __init__(self, data):
        data = data or {}
        self.resource = data.get('resource', '')  # 'cpu', 'power' or 'calibration'
        self.pinnedUsed = data.get('pinnedUsed', 0.0)
        self.output = data.get('output', 0.0)
        self.bestPinnedUsed = data.get('bestPinnedUsed', 0.0)
        self.bestOutput = data.get('bestOutput', 0.0)
        self.shortfall = data.get('shortfall', 0.0)
        self.message = data.get('message', '')


class Infeasibility:
    """Why no option can fit (response status "infeasible")."""

    def __init__(self, data):
        data = data or {}
        self.reason = data.get('reason', '')  # 'calibration', 'outputs', 'search' or 'capacitor'
        self.message = data.get('message', '')
        self.resources = [Shortfall(r) for r in data.get('resources', ())]
        # Pinned modules named in the message: {position, slot, typeID, name, cost}
        self.modules = list(data.get('modules', ()))


class OptimizerResult:

    def __init__(self, data):
        self.raw = data
        self.status = data.get('status', 'ok')
        self.message = data.get('message', '')
        # Infeasibility when no option can fit (then there are no options), else None.
        self.infeasible = None
        if self.status == 'infeasible':
            self.infeasible = Infeasibility(data.get('infeasible'))
            if not self.infeasible.message:
                self.infeasible.message = self.message
        self.goal = data.get('goal')
        self.openSlots = data.get('openSlots', {})
        self.start = FitStats(data.get('start'))
        self.clampedModules = list(data.get('clampedModules', ()))
        self.options = [Option(o) for o in data.get('options', ())]
        self.warnings = list(data.get('warnings', ()))
        self.stats = data.get('stats', {})


def parseResponse(data):
    """OptimizerResult from the response dict; OptimizerError for an error response.

    Status "infeasible" (no choice for the selected slots can fit) is a result, not an
    error: OptimizerResult.infeasible says why, and there are no options.
    """
    if not isinstance(data, dict):
        raise OptimizerError('The optimizer returned no response.')
    if data.get('status') not in ('ok', 'infeasible'):
        raise OptimizerError(data.get('message') or 'The optimizer failed.')
    return OptimizerResult(data)


# -----------------------------------------------------------------------------
# Running the exe
# -----------------------------------------------------------------------------

def defaultExePath():
    return os.path.join(config.pyfaPath or config.getPyfaRoot(), 'cpp', 'bin', 'Release', 'PyfaReader.exe')


def exePath(setting=None):
    """The configured exe path, or the default one."""
    if setting is None:
        from service.settings import ModuleOptimizerSettings
        setting = ModuleOptimizerSettings.getInstance().get('exePath')
    return setting or defaultExePath()


def isWsl():
    return sys.platform.startswith('linux') and 'microsoft' in platform.uname().release.lower()


def exeFilePath(path, exe):
    """A path as the exe sees it: under WSL a Windows exe needs Windows paths."""
    if isWsl() and exe.lower().endswith('.exe'):
        try:
            return subprocess.run(['wslpath', '-w', path], capture_output=True, text=True, check=True).stdout.strip()
        except (OSError, subprocess.CalledProcessError):
            pyfalog.warning('wslpath failed for {}', path)
    return path


class OptimizerRun:
    """
    One run of the exe. run() blocks (call it in a worker thread) and returns the
    OptimizerResult or raises OptimizerError / OptimizerCancelled. progress(fraction, phase)
    is called from the worker thread for every progress line. cancel() may be called from
    any thread.
    """

    def __init__(self, request, exe=None, gameDB=None, threads=None, timeout=None, progress=None):
        self.request = request
        self.exe = exe or exePath()
        self.gameDB = gameDB or config.gameDB
        self.threads = threads
        self.timeout = timeout
        self.progress = progress
        self.process = None
        self.cancelled = False
        self.timedOut = False
        self.log = []
        self.seconds = None
        self._lock = threading.Lock()

    def command(self):
        cmd = [self.exe]
        if self.gameDB:
            if isWsl() and self.exe.lower().endswith('.exe') and not os.path.abspath(self.gameDB).startswith('/mnt/'):
                # SQLite can't open a database inside the WSL file system from Windows (\\wsl.localhost\...
                # is "locked"); the exe then uses the eve.db it finds itself (next to it or above)
                pyfalog.warning('eve.db {} is not on a Windows drive; the optimizer uses its own', self.gameDB)
            else:
                cmd += ['--game', exeFilePath(self.gameDB, self.exe)]
        cmd += ['--optimize-request', '-', '--out', '-']
        if self.threads:
            cmd += ['--threads', str(int(self.threads))]
        return cmd

    def cancel(self):
        with self._lock:
            self.cancelled = True
            self._kill()

    def _kill(self):
        process = self.process
        if process is not None and process.poll() is None:
            try:
                process.kill()
            except OSError:
                pass

    def _onTimeout(self):
        with self._lock:
            self.timedOut = True
            self._kill()

    def run(self):
        if not os.path.isfile(self.exe):
            raise OptimizerError('The optimizer was not found at {}.\nBuild it (cpp/PyFaReader.sln, Release x64) '
                                 'or set its path.'.format(self.exe))
        cmd = self.command()
        pyfalog.info('Running module optimizer: {}', ' '.join(cmd))
        kwargs = {}
        if sys.platform == 'win32':
            kwargs['creationflags'] = getattr(subprocess, 'CREATE_NO_WINDOW', 0x08000000)
        start = time.monotonic()
        with self._lock:
            if self.cancelled:
                raise OptimizerCancelled()
            try:
                self.process = subprocess.Popen(
                    cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    cwd=os.path.dirname(os.path.abspath(self.exe)), **kwargs)
            except OSError as e:
                raise OptimizerError('Could not start the optimizer ({}):\n{}'.format(self.exe, e))
        process = self.process
        timer = None
        if self.timeout:
            timer = threading.Timer(self.timeout, self._onTimeout)
            timer.daemon = True
            timer.start()
        output = []

        def writeInput():
            try:
                process.stdin.write(json.dumps(self.request).encode('utf-8'))
                process.stdin.close()
            except OSError:
                pass

        def readOutput():
            output.append(process.stdout.read())

        writer = threading.Thread(target=writeInput, daemon=True)
        reader = threading.Thread(target=readOutput, daemon=True)
        writer.start()
        reader.start()
        try:
            for raw in process.stderr:
                line = raw.decode('utf-8', errors='replace').strip()
                if line.startswith('progress '):
                    parts = line.split(' ', 2)
                    try:
                        fraction = float(parts[1])
                    except (IndexError, ValueError):
                        continue
                    if self.progress is not None:
                        self.progress(fraction, parts[2] if len(parts) > 2 else '')
                elif line:
                    self.log.append(line)
            process.wait()
            reader.join()
            writer.join()
        finally:
            if timer is not None:
                timer.cancel()
        self.seconds = time.monotonic() - start
        if self.cancelled:
            raise OptimizerCancelled()
        if self.timedOut:
            raise OptimizerError('The optimizer took longer than {} s and was stopped.'.format(self.timeout))
        text = output[0].decode('utf-8', errors='replace') if output and output[0] else ''
        data = None
        if text.strip():
            try:
                data = json.loads(text)
            except ValueError:
                pyfalog.error('Optimizer output is not JSON: {}', text[:500])
        if data is None:
            tail = '\n'.join(self.log[-5:])
            raise OptimizerError('The optimizer failed (exit code {}).{}'.format(
                process.returncode, '\n' + tail if tail else ''))
        if process.returncode not in (0, 1) and data.get('status') == 'ok':
            raise OptimizerError('The optimizer failed (exit code {}).'.format(process.returncode))
        pyfalog.info('Module optimizer finished in {:.2f} s', self.seconds)
        return parseResponse(data)
