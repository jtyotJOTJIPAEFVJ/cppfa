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
Optimize modules: runs the C++ module optimizer (service.moduleOptimizer) on the selected
slots of a fit and lists the options it finds. Double-clicking an option fits it.
"""

import os
import threading

# noinspection PyPackageRequirements
import wx
from logbook import Logger

import gui.fitCommands as cmd
from eos.const import FittingModuleState, FittingSlot
from gui.fitCommands.helpers import ModuleInfo
from gui.utils.dark import warningTextColor
from gui.utils.numberFormatter import formatAmount
from service import moduleOptimizer as mo
from service.fit import Fit
from service.settings import ModuleOptimizerSettings

pyfalog = Logger(__name__)
_t = wx.GetTranslation


def goalLabels():
    return {
        'shieldPassive': _t('Shield EHP (passive)'),
        'armorPassive': _t('Armor EHP (passive)'),
        'hullPassive': _t('Hull EHP (passive)'),
        'shieldActive': _t('Shield repair, sustained (EHP/s)'),
        'armorActive': _t('Armor repair, sustained (EHP/s)'),
        'shieldResists': _t('Shield resists only'),
        'armorResists': _t('Armor resists only'),
        'shieldRemote': _t('Remote shield repair, sustained (HP/s)'),
        'armorRemote': _t('Remote armor repair, sustained (HP/s)')}


def slotLabels():
    return {
        FittingSlot.LOW: _t('Low'),
        FittingSlot.MED: _t('Mid'),
        FittingSlot.HIGH: _t('High'),
        FittingSlot.RIG: _t('Rig'),
        FittingSlot.SUBSYSTEM: _t('Subsystem'),
        FittingSlot.SERVICE: _t('Service')}


def slotLabel(fit, position):
    """E.g. "Mid 2": the slot's rack and its number within the rack."""
    mod = fit.modules[position]
    index = sum(1 for m in list(fit.modules)[:position] if m.slot == mod.slot)
    return '{} {}'.format(slotLabels().get(mod.slot, '?'), index + 1)


def isActiveGoal(goal):
    """Repair per second: local (active) or remote."""
    return goal.endswith('Active') or goal.endswith('Remote')


def isResistGoal(goal):
    return goal.endswith('Resists')


def averageResist(value):
    """Resists-only goal value (EHP per HP, 1 / mean resonance) as the mean resist, 0..1."""
    return 1 - 1 / value if value > 0 else 0


def formatGoal(goal, value):
    if isResistGoal(goal):
        return '{:.2f}%'.format(averageResist(value) * 100)
    return formatAmount(value, 4 if isActiveGoal(goal) else 5, 0, 0)


def formatGain(goal, option, start):
    """Resists-only goals: the change of the average resist, in percentage points."""
    if isResistGoal(goal):
        return '{:+.2f}%'.format((averageResist(option.goal) - averageResist(start.goal)) * 100)
    return formatGoal(goal, option.gain)


def formatIsk(value, signed=False):
    if value is None:
        return ''
    return formatAmount(value, 3, 3, 9, currency=True, forceSign=signed)


def formatUse(used, output):
    return '{} / {}'.format(formatAmount(used, 4, 0, 0), formatAmount(output, 4, 0, 0))


# Columns: (label, sort key of an option row, higher first when sorted)
COLUMNS = (
    ('#', lambda o: o.rank, False),
    ('Goal', lambda o: o.goal, True),
    ('Gain', lambda o: o.gain, True),
    ('Price', lambda o: o.price, False),
    ('Price change', lambda o: o.priceDelta, False),
    ('CPU', lambda o: o.stats.cpuUsed / o.stats.cpuOutput if o.stats.cpuOutput else 0, False),
    ('PG', lambda o: o.stats.powerUsed / o.stats.powerOutput if o.stats.powerOutput else 0, False),
    ('Calibration', lambda o: o.stats.calibrationUsed, False),
    ('Cap', lambda o: o.stats.capRecharge - o.stats.capUsed, True),
    ('Fits', lambda o: o.fits, True),
    ('Modules', None, False))


def columnLabels():
    return {
        '#': '#', 'Goal': _t('Goal'), 'Gain': _t('Gain'), 'Price': _t('Price'),
        'Price change': _t('Price change'), 'CPU': _t('CPU'), 'PG': _t('PG'),
        'Calibration': _t('Calibration'), 'Cap': _t('Cap'), 'Fits': _t('Fits'),
        'Modules': _t('Modules (changed slots)')}


class ModuleOptimizerDialog(wx.Frame):
    """One window at a time; open() replaces the previous one."""

    _instance = None

    @classmethod
    def open(cls, parent, fitID, positions):
        if cls._instance:
            cls._instance.Close()
        cls._instance = cls(parent, fitID, positions)
        cls._instance.Show()
        return cls._instance

    def __init__(self, parent, fitID, positions):
        super().__init__(parent, title=_t('Optimize Modules'),
                         style=wx.DEFAULT_FRAME_STYLE | wx.FRAME_FLOAT_ON_PARENT | wx.FRAME_TOOL_WINDOW)
        self.mainFrame = parent
        self.fitID = fitID
        self.positions = sorted(positions)
        self.settings = ModuleOptimizerSettings.getInstance()
        self.run = None
        self.request = None
        self.originals = {}
        self.result = None
        self.rows = []
        self.sortColumn = None
        self.appliedRank = None

        panel = wx.Panel(self)
        self.panel = panel
        sizer = wx.BoxSizer(wx.VERTICAL)

        self.slotsText = wx.StaticText(panel, label='')
        sizer.Add(self.slotsText, 0, wx.EXPAND | wx.ALL, 6)

        # Options
        options = wx.BoxSizer(wx.HORIZONTAL)
        options.Add(wx.StaticText(panel, label=_t('Goal:')), 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 4)
        labels = goalLabels()
        self.goalChoice = wx.Choice(panel, choices=[labels[g] for g in mo.GOALS])
        goal = self.settings.get('goal')
        self.goalChoice.SetSelection(mo.GOALS.index(goal) if goal in mo.GOALS else 0)
        self.goalChoice.SetToolTip(_t('Passive: effective HP for uniform damage. Active: effective HP per second '
                                      'repaired, sustained by the capacitor. Resists only: the average resist of '
                                      'the layer (uniform damage), ignoring its HP. Remote: HP per second the '
                                      'remote repairers give, sustained by the capacitor (select high slots).'))
        options.Add(self.goalChoice, 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 10)

        priceLabel = wx.StaticText(panel, label=_t('Max price per module (M ISK):'))
        options.Add(priceLabel, 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 4)
        self.priceCap = wx.SpinCtrlDouble(panel, min=0, max=1e6, inc=10, initial=self.settings.get('priceCap') / 1e6)
        self.priceCap.SetDigits(1)
        tip = _t('0 for no limit. With a limit, modules without a known price are skipped.')
        priceLabel.SetToolTip(tip)
        self.priceCap.SetToolTip(tip)
        options.Add(self.priceCap, 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 10)

        self.allowOfficer = wx.CheckBox(panel, label=_t('Officer modules'))
        self.allowOfficer.SetValue(self.settings.get('allowOfficer'))
        options.Add(self.allowOfficer, 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 10)

        options.Add(wx.StaticText(panel, label=_t('Results:')), 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 4)
        self.resultCount = wx.SpinCtrl(panel, min=1, max=100, initial=self.settings.get('results'))
        options.Add(self.resultCount, 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 10)

        budgetLabel = wx.StaticText(panel, label=_t('Max time (s):'))
        options.Add(budgetLabel, 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 4)
        self.timeBudget = wx.SpinCtrl(panel, min=0, max=3600, initial=int(self.settings.get('timeBudget')))
        tip = _t('How long the optimizer may keep improving its results. Small selections finish much sooner. '
                 '0 for no limit.')
        budgetLabel.SetToolTip(tip)
        self.timeBudget.SetToolTip(tip)
        options.Add(self.timeBudget, 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 10)

        options.AddStretchSpacer()
        self.runButton = wx.Button(panel, label=_t('Run'))
        self.runButton.SetDefault()
        options.Add(self.runButton, 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 4)
        self.cancelButton = wx.Button(panel, label=_t('Cancel'))
        self.cancelButton.Disable()
        options.Add(self.cancelButton, 0, wx.ALIGN_CENTER_VERTICAL)
        sizer.Add(options, 0, wx.EXPAND | wx.LEFT | wx.RIGHT | wx.BOTTOM, 6)

        # Constraints
        constraints = wx.BoxSizer(wx.HORIZONTAL)
        self.allowAncillary = wx.CheckBox(panel, label=_t('Ancillary repairers'))
        self.allowAncillary.SetValue(self.settings.get('allowAncillary'))
        self.allowAncillary.SetToolTip(_t('Allow ancillary shield boosters and armor repairers (local and remote). '
                                          'Their sustained repair counts the reload.'))
        constraints.Add(self.allowAncillary, 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 10)

        repsLabel = wx.StaticText(panel, label=_t('Max repairers:'))
        constraints.Add(repsLabel, 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 4)
        self.maxReps = wx.SpinCtrl(panel, min=0, max=16, initial=int(self.settings.get('maxReps')))
        tip = _t('Most repairers of the goal in the fit, those outside the selection included: local ones for '
                 'active goals, remote ones for remote goals. 0 for no limit.')
        repsLabel.SetToolTip(tip)
        self.maxReps.SetToolTip(tip)
        constraints.Add(self.maxReps, 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 10)

        self.requireCapStable = wx.CheckBox(panel, label=_t('Cap stable'))
        self.requireCapStable.SetValue(self.settings.get('requireCapStable'))
        self.requireCapStable.SetToolTip(_t('Only options whose capacitor use is within its recharge '
                                            '(as the Cap column shows).'))
        constraints.Add(self.requireCapStable, 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 10)

        capLabel = wx.StaticText(panel, label=_t('Min cap recharge (GJ/s):'))
        constraints.Add(capLabel, 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 4)
        self.minCapRecharge = wx.SpinCtrlDouble(panel, min=0, max=1e5, inc=10,
                                                initial=float(self.settings.get('minCapRecharge')))
        self.minCapRecharge.SetDigits(1)
        tip = _t('Only options whose capacitor recharge (peak recharge plus cap boosters, the second number in the '
                 'Cap column) is at least this, e.g. to run remote repairers fitted elsewhere. 0 for none.')
        capLabel.SetToolTip(tip)
        self.minCapRecharge.SetToolTip(tip)
        constraints.Add(self.minCapRecharge, 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 10)
        sizer.Add(constraints, 0, wx.EXPAND | wx.LEFT | wx.RIGHT | wx.BOTTOM, 6)

        # Progress
        progress = wx.BoxSizer(wx.HORIZONTAL)
        self.gauge = wx.Gauge(panel, range=1000, size=(200, -1))
        progress.Add(self.gauge, 1, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 8)
        self.phaseText = wx.StaticText(panel, label='', size=(260, -1), style=wx.ST_NO_AUTORESIZE)
        progress.Add(self.phaseText, 0, wx.ALIGN_CENTER_VERTICAL)
        sizer.Add(progress, 0, wx.EXPAND | wx.LEFT | wx.RIGHT | wx.BOTTOM, 6)

        # Warnings
        warnings = wx.BoxSizer(wx.HORIZONTAL)
        self.warningText = wx.StaticText(panel, label='')
        self.warningText.SetForegroundColour(warningTextColor())
        warnings.Add(self.warningText, 1, wx.ALIGN_CENTER_VERTICAL)
        self.onlineButton = wx.Button(panel, label=_t('Set them online'))
        self.onlineButton.SetToolTip(_t('Set the modules the optimizer evaluated online to online in the fit'))
        self.onlineButton.Hide()
        warnings.Add(self.onlineButton, 0, wx.ALIGN_CENTER_VERTICAL | wx.LEFT, 8)
        sizer.Add(warnings, 0, wx.EXPAND | wx.LEFT | wx.RIGHT | wx.BOTTOM, 6)

        # Results
        self.list = wx.ListCtrl(panel, style=wx.LC_REPORT | wx.LC_SINGLE_SEL, size=(-1, 260))
        colLabels = columnLabels()
        for i, (label, _, _) in enumerate(COLUMNS):
            align = wx.LIST_FORMAT_LEFT if label in ('Modules', 'Fits') else wx.LIST_FORMAT_RIGHT
            self.list.InsertColumn(i, colLabels[label], align)
        sizer.Add(self.list, 1, wx.EXPAND | wx.LEFT | wx.RIGHT, 6)

        self.detailText = wx.TextCtrl(panel, style=wx.TE_MULTILINE | wx.TE_READONLY | wx.TE_DONTWRAP, size=(-1, 110))
        sizer.Add(self.detailText, 0, wx.EXPAND | wx.ALL, 6)

        bottom = wx.BoxSizer(wx.HORIZONTAL)
        hint = wx.StaticText(panel, label=_t(
            'Double-click an option (or press Enter) to fit it; the first row puts the original modules back. '
            'Undo works as usual. Numbers are the optimizer\'s; pyfa shows its own after fitting.'))
        hint.SetForegroundColour(wx.SystemSettings.GetColour(wx.SYS_COLOUR_GRAYTEXT))
        hint.Wrap(800)
        bottom.Add(hint, 1, wx.ALIGN_CENTER_VERTICAL)
        self.exeButton = wx.Button(panel, label=_t('Optimizer path...'))
        bottom.Add(self.exeButton, 0, wx.ALIGN_CENTER_VERTICAL | wx.LEFT, 8)
        sizer.Add(bottom, 0, wx.EXPAND | wx.LEFT | wx.RIGHT | wx.BOTTOM, 6)

        panel.SetSizer(sizer)
        self.updateSlotsText()
        self.autoSizeColumns()
        self.SetClientSize(wx.Size(max(sizer.GetMinSize().width, 980), sizer.GetMinSize().height + 40))
        self.CenterOnParent()

        self.runButton.Bind(wx.EVT_BUTTON, self.onRun)
        self.cancelButton.Bind(wx.EVT_BUTTON, self.onCancel)
        self.onlineButton.Bind(wx.EVT_BUTTON, self.onSetOnline)
        self.exeButton.Bind(wx.EVT_BUTTON, self.onChooseExe)
        self.list.Bind(wx.EVT_LIST_ITEM_ACTIVATED, self.onActivate)
        self.list.Bind(wx.EVT_LIST_ITEM_SELECTED, self.onSelect)
        self.list.Bind(wx.EVT_LIST_COL_CLICK, self.onColumnClick)
        self.Bind(wx.EVT_CHAR_HOOK, self.onCharHook)
        self.Bind(wx.EVT_CLOSE, self.onClose)

    # -------------------------------------------------------------------------
    # Fit
    # -------------------------------------------------------------------------

    def getFit(self):
        return Fit.getInstance().getFit(self.fitID)

    def updateSlotsText(self):
        fit = self.getFit()
        if fit is None:
            return
        labels = []
        details = []
        for position in self.positions:
            if position >= len(fit.modules):
                continue
            mod = fit.modules[position]
            name = _t('empty') if mod.isEmpty else mod.item.name
            labels.append(slotLabel(fit, position))
            details.append('{}: {}'.format(labels[-1], name))
        self.slotsText.SetLabel(_t('{} ({}) - slots to optimize: {}').format(
            fit.name, fit.ship.item.name, ', '.join(labels)))
        self.slotsText.Wrap(940)
        self.slotsText.SetToolTip('\n'.join(details))
        self.panel.Layout()

    def saveOptions(self):
        goal = mo.GOALS[max(0, self.goalChoice.GetSelection())]
        self.settings.set('goal', goal)
        self.settings.set('priceCap', max(0.0, self.priceCap.GetValue()) * 1e6)
        self.settings.set('allowOfficer', self.allowOfficer.GetValue())
        self.settings.set('allowAncillary', self.allowAncillary.GetValue())
        self.settings.set('maxReps', self.maxReps.GetValue())
        self.settings.set('requireCapStable', self.requireCapStable.GetValue())
        self.settings.set('minCapRecharge', max(0.0, self.minCapRecharge.GetValue()))
        self.settings.set('results', self.resultCount.GetValue())
        self.settings.set('timeBudget', self.timeBudget.GetValue())
        return goal

    # -------------------------------------------------------------------------
    # Running
    # -------------------------------------------------------------------------

    def onRun(self, event):
        if self.run is not None:
            return
        fit = self.getFit()
        if fit is None:
            return
        exe = mo.exePath()
        if not os.path.isfile(exe):
            self.askForExe(exe)
            return
        goal = self.saveOptions()
        positions = [p for p in self.positions if p < len(fit.modules) and mo.isOptimizable(fit.modules[p])]
        if not positions:
            wx.MessageBox(_t('None of the selected slots can be optimized: only high, mid, low and rig slots can.'),
                          _t('Optimize Modules'), wx.OK | wx.ICON_INFORMATION, self)
            return
        try:
            self.request = mo.buildRequest(
                fit, positions, goal, priceCap=self.settings.get('priceCap'),
                allowOfficer=self.settings.get('allowOfficer'), results=self.settings.get('results'),
                threads=self.settings.get('threads'), timeBudget=self.settings.get('timeBudget'),
                allowAncillary=self.settings.get('allowAncillary'), maxReps=self.settings.get('maxReps'),
                requireCapStable=self.settings.get('requireCapStable'),
                minCapRecharge=self.settings.get('minCapRecharge'))
        except Exception as e:
            pyfalog.exception('Failed to build the optimizer request')
            wx.MessageBox(_t('Could not describe the fit to the optimizer:\n{}').format(e),
                          _t('Optimize Modules'), wx.OK | wx.ICON_ERROR, self)
            return
        # The modules as they are now, to put back the ones an option leaves unchanged
        self.originals = {p: None if fit.modules[p].isEmpty else ModuleInfo.fromModule(fit.modules[p])
                          for p in positions}
        self.updateSlotsText()
        self.setResult(None)
        self.setRunning(True)
        self.gauge.SetValue(0)
        self.phaseText.SetLabel(_t('Starting...'))
        # The run timeout must leave room for the time budget (and the search before it)
        timeout = self.settings.get('timeout') or None
        budget = self.settings.get('timeBudget') or 0
        if timeout and budget:
            timeout = max(timeout, budget + 120)
        elif not budget:
            timeout = None  # no time limit: the optimizer stops when its results stop improving
        self.run = mo.OptimizerRun(
            self.request, exe=exe, timeout=timeout,
            progress=lambda fraction, phase: wx.CallAfter(self.onProgress, fraction, phase))
        thread = threading.Thread(target=self.worker, args=(self.run,), name='ModuleOptimizer', daemon=True)
        thread.start()

    def worker(self, run):
        result = error = None
        cancelled = False
        try:
            result = run.run()
        except mo.OptimizerCancelled:
            cancelled = True
        except mo.OptimizerError as e:
            error = str(e)
        except Exception as e:
            pyfalog.exception('Module optimizer failed')
            error = repr(e)
        wx.CallAfter(self.onFinished, run, result, error, cancelled)

    def onProgress(self, fraction, phase):
        if not self or self.run is None:
            return
        self.gauge.SetValue(int(max(0.0, min(1.0, fraction)) * 1000))
        self.phaseText.SetLabel(phase)

    def onFinished(self, run, result, error, cancelled):
        if not self or run is not self.run:
            return
        self.run = None
        self.setRunning(False)
        if cancelled:
            self.gauge.SetValue(0)
            self.phaseText.SetLabel(_t('Cancelled'))
            return
        if error is not None:
            self.gauge.SetValue(0)
            self.phaseText.SetLabel(_t('Failed'))
            wx.MessageBox(error, _t('Optimize Modules'), wx.OK | wx.ICON_ERROR, self)
            return
        self.gauge.SetValue(1000)
        if result.infeasible is not None and result.infeasible.reason == 'capacitor':
            self.phaseText.SetLabel(_t('Done in {:.1f} s: no option meets the capacitor requirement').format(
                run.seconds or 0))
        elif result.infeasible is not None:
            self.phaseText.SetLabel(_t('Done in {:.1f} s: no option can fit').format(run.seconds or 0))
        else:
            self.phaseText.SetLabel(_t('Done in {:.1f} s: {} options').format(run.seconds or 0, len(result.options)))
        self.setResult(result)

    def onCancel(self, event):
        if self.run is not None:
            self.run.cancel()

    def setRunning(self, running):
        self.runButton.Enable(not running)
        self.cancelButton.Enable(running)
        for control in (self.goalChoice, self.priceCap, self.allowOfficer, self.resultCount, self.timeBudget,
                        self.allowAncillary, self.maxReps, self.requireCapStable, self.minCapRecharge,
                        self.exeButton):
            control.Enable(not running)

    # -------------------------------------------------------------------------
    # Results
    # -------------------------------------------------------------------------

    def setResult(self, result):
        self.result = result
        self.appliedRank = None
        self.sortColumn = None
        self.rows = list(result.options) if result is not None else []
        self.updateWarnings()
        self.populate()

    def updateWarnings(self):
        result = self.result
        lines = []
        if result is not None:
            if result.infeasible is not None:
                # The optimizer's diagnosis: which resource, by how much, what to change.
                if result.infeasible.reason == 'capacitor':
                    lines.append(result.infeasible.message)
                else:
                    lines.append(_t('No option can fit.') + ' ' + result.infeasible.message)
            elif not result.options:
                lines.append(_t('No options found.'))
            lines.extend(result.warnings)
        self.warningText.SetLabel('\n'.join(lines))
        self.warningText.Wrap(800)
        self.onlineButton.Show(bool(result is not None and result.clampedModules))
        self.panel.Layout()

    def populate(self):
        self.list.DeleteAllItems()
        result = self.result
        if result is None:
            self.detailText.SetValue('')
            return
        goal = result.goal
        start = result.start
        # Row 0: the fit as it was
        startRow = self.list.InsertItem(0, _t('now'))
        values = [formatGoal(goal, start.goal), '', formatIsk(start.price), '',
                  formatUse(start.cpuUsed, start.cpuOutput), formatUse(start.powerUsed, start.powerOutput),
                  formatUse(start.calibrationUsed, start.calibrationOutput), self.capText(start),
                  _t('yes') if start.fits else _t('no'), _t('(current modules)')]
        for col, value in enumerate(values, start=1):
            self.list.SetItem(startRow, col, value)
        self.list.SetItemTextColour(startRow, wx.SystemSettings.GetColour(wx.SYS_COLOUR_GRAYTEXT))
        for option in self.rows:
            row = self.list.InsertItem(self.list.GetItemCount(), str(option.rank))
            stats = option.stats
            values = [formatGoal(goal, option.goal), formatGain(goal, option, start), formatIsk(option.price),
                      formatIsk(option.priceDelta, signed=True),
                      formatUse(stats.cpuUsed, stats.cpuOutput), formatUse(stats.powerUsed, stats.powerOutput),
                      formatUse(stats.calibrationUsed, stats.calibrationOutput), self.capText(stats),
                      _t('yes') if option.fits else _t('no'),
                      ', '.join(option.moduleNames(changedOnly=True, counted=True)) or _t('(no change)')]
            for col, value in enumerate(values, start=1):
                self.list.SetItem(row, col, value)
            if not option.fits:
                self.list.SetItemTextColour(row, warningTextColor())
            if option.rank == self.appliedRank:
                font = self.list.GetFont()
                font.SetWeight(wx.FONTWEIGHT_BOLD)
                self.list.SetItemFont(row, font)
        self.autoSizeColumns()
        if self.rows:
            self.list.Select(1)
            self.list.Focus(1)

    @staticmethod
    def capText(stats):
        text = '{} / {}'.format(formatAmount(stats.capUsed, 3, 0, 0), formatAmount(stats.capRecharge, 3, 0, 0))
        return (_t('stable') if stats.capStable else _t('unstable')) + ' ' + text

    def autoSizeColumns(self):
        for col in range(self.list.GetColumnCount()):
            if self.list.GetItemCount():
                self.list.SetColumnWidth(col, wx.LIST_AUTOSIZE)
                width = self.list.GetColumnWidth(col)
                self.list.SetColumnWidth(col, wx.LIST_AUTOSIZE_USEHEADER)
                self.list.SetColumnWidth(col, max(width, self.list.GetColumnWidth(col)))
            else:
                self.list.SetColumnWidth(col, wx.LIST_AUTOSIZE_USEHEADER)

    def rowOption(self, row):
        """The option of a list row; None for the first row (the current modules)."""
        if row <= 0 or row - 1 >= len(self.rows):
            return None
        return self.rows[row - 1]

    def onSelect(self, event):
        self.detailText.SetValue(self.detailFor(self.rowOption(event.GetIndex())))

    def detailFor(self, option):
        fit = self.getFit()
        if fit is None or self.request is None:
            return ''
        lines = []
        if option is None:
            for position, info in sorted(self.originals.items()):
                lines.append('{}: {}'.format(self.slotName(fit, position), self.infoName(info)))
            return '\n'.join(lines)
        for choice in option.slots:
            if choice.isEmpty:
                name = _t('(empty)')
            else:
                name = choice.name
                if choice.chargeName:
                    name += ' [{}]'.format(choice.chargeName)
                if choice.state is not None:
                    name += ' ({})'.format(mo.STATE_NAMES.get(choice.state, ''))
            if choice.unchanged:
                name = self.infoName(self.originals.get(choice.position)) + ' ' + _t('(unchanged)')
            lines.append('{}: {}'.format(self.slotName(fit, choice.position), name))
        return '\n'.join(lines)

    @staticmethod
    def slotName(fit, position):
        try:
            return slotLabel(fit, position)
        except IndexError:
            return str(position)

    @staticmethod
    def infoName(info):
        if info is None:
            return _t('(empty)')
        from service.market import Market
        item = Market.getInstance().getItem(info.itemID)
        name = item.name if item is not None else str(info.itemID)
        if info.chargeID:
            charge = Market.getInstance().getItem(info.chargeID)
            if charge is not None:
                name += ' [{}]'.format(charge.name)
        return name

    def onColumnClick(self, event):
        col = event.GetColumn()
        if self.result is None or col < 0 or COLUMNS[col][1] is None:
            return
        _, key, higherFirst = COLUMNS[col]
        if self.sortColumn == col:
            self.rows.reverse()
        else:
            self.sortColumn = col
            self.rows = sorted(self.result.options, key=key, reverse=higherFirst)
        self.populate()

    # -------------------------------------------------------------------------
    # Applying
    # -------------------------------------------------------------------------

    def onActivate(self, event):
        self.applyRow(event.GetIndex())

    def onCharHook(self, event):
        if event.GetKeyCode() == wx.WXK_ESCAPE and self.run is None:
            self.Close()
            return
        event.Skip()

    def checkFit(self):
        fit = self.getFit()
        if fit is None or self.request is None:
            return None
        if not mo.fitMatchesRequest(fit, self.request):
            wx.MessageBox(_t('The fit has changed outside the optimized slots since the optimizer ran. '
                             'Run it again.'), _t('Optimize Modules'), wx.OK | wx.ICON_INFORMATION, self)
            return None
        return fit

    def applyRow(self, row):
        if self.result is None or self.run is not None:
            return
        fit = self.checkFit()
        if fit is None:
            return
        option = self.rowOption(row)
        if option is None:
            modInfos = dict(self.originals)
            name = 'Restore Modules'
        else:
            modInfos = {}
            for choice in option.slots:
                if choice.unchanged:
                    modInfos[choice.position] = self.originals.get(choice.position)
                elif choice.isEmpty:
                    modInfos[choice.position] = None
                else:
                    modInfos[choice.position] = ModuleInfo(
                        itemID=choice.typeID, chargeID=choice.chargeID,
                        state=choice.state if choice.state is not None else FittingModuleState.ONLINE)
            name = 'Apply Optimized Modules'
        self.mainFrame.command.Submit(cmd.GuiApplyOptimizedModulesCommand(self.fitID, modInfos, name=name))
        self.appliedRank = option.rank if option is not None else None
        selected = self.list.GetFirstSelected()
        self.populate()
        if selected >= 0:
            self.list.Select(selected)
            self.list.Focus(selected)
        self.checkApplied(fit, modInfos)

    def checkApplied(self, fit, modInfos):
        """Tell the user about slots pyfa refused (restrictions, charges)."""
        missing = []
        for position, info in sorted(modInfos.items()):
            mod = fit.modules[position] if position < len(fit.modules) else None
            if mod is None:
                continue
            if info is None:
                ok = mod.isEmpty
            else:
                ok = not mod.isEmpty and mod.itemID == info.itemID
            if not ok:
                missing.append(slotLabel(fit, position))
        if missing:
            wx.MessageBox(_t('pyfa could not fit every module of this option ({}).').format(', '.join(missing)),
                          _t('Optimize Modules'), wx.OK | wx.ICON_WARNING, self)

    def onSetOnline(self, event):
        if self.result is None or self.run is not None:
            return
        fit = self.checkFit()
        if fit is None:
            return
        modInfos = {}
        for clamped in self.result.clampedModules:
            position = clamped.get('position')
            if position is None or position >= len(fit.modules):
                continue
            mod = fit.modules[position]
            if mod.isEmpty or mod.itemID != clamped.get('typeID'):
                continue
            info = ModuleInfo.fromModule(mod)
            info.state = FittingModuleState.ONLINE
            modInfos[position] = info
        if not modInfos:
            return
        self.mainFrame.command.Submit(cmd.GuiApplyOptimizedModulesCommand(
            self.fitID, modInfos, name='Set Modules Online'))
        # The optimizer evaluated them online already: the results stay valid
        for entry in self.request['modules']:
            if entry['position'] in modInfos:
                entry['state'] = mo.STATE_NAMES[FittingModuleState.ONLINE]
        self.onlineButton.Hide()
        self.panel.Layout()

    # -------------------------------------------------------------------------
    # Exe path
    # -------------------------------------------------------------------------

    def askForExe(self, missing):
        answer = wx.MessageBox(
            _t('The module optimizer was not found at\n{}\n\nBuild it from cpp/PyfaReader.sln (Release, x64), '
               'or choose where PyfaReader.exe is. Choose it now?').format(missing),
            _t('Optimize Modules'), wx.YES_NO | wx.ICON_WARNING, self)
        if answer == wx.YES:
            self.onChooseExe(None)

    def onChooseExe(self, event):
        current = mo.exePath()
        with wx.FileDialog(
                self, _t('Choose PyfaReader.exe'), defaultDir=os.path.dirname(current),
                defaultFile=os.path.basename(current), wildcard='PyfaReader.exe|PyfaReader.exe|*|*',
                style=wx.FD_OPEN | wx.FD_FILE_MUST_EXIST) as dialog:
            if dialog.ShowModal() != wx.ID_OK:
                return
            path = dialog.GetPath()
        self.settings.set('exePath', '' if os.path.normcase(path) == os.path.normcase(mo.defaultExePath()) else path)

    def onClose(self, event):
        if self.run is not None:
            self.run.cancel()
            self.run = None
        self.saveOptions()
        if ModuleOptimizerDialog._instance is self:
            ModuleOptimizerDialog._instance = None
        self.Destroy()
