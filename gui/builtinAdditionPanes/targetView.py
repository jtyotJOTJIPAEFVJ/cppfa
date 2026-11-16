# =============================================================================
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
# =============================================================================

# Target tab: the fits and target profiles the active fit is aimed at, at most one
# of them enabled (see service.target)

# noinspection PyPackageRequirements
import wx

import gui.display as d
import gui.fitCommands as cmd
import gui.globalEvents as GE
import gui.mainFrame
from eos.saveddata.fitTarget import KIND_FIT, KIND_PROFILE
from graphs.wrapper import TargetWrapper
from gui.builtinViewColumns.state import State
from gui.contextMenu import ContextMenu
from gui.utils.sorter import smartSort
from service.fit import Fit
from service.target import getTargetFit, getTargetKey

_t = wx.GetTranslation


class DummyItem:

    def __init__(self, txt):
        self.name = txt
        self.iconID = None


class DummyEntry:

    def __init__(self, txt):
        self.item = DummyItem(txt)


class TargetRow(TargetWrapper):
    """A row of the view: the target (fit or profile) wrapped for the graph columns, plus its state"""

    def __init__(self, fitTarget, item):
        super().__init__(item=item, lightnessID=None, lineStyleID=None)
        self.fitTarget = fitTarget

    @property
    def active(self):
        return self.fitTarget.active

    @property
    def key(self):
        return self.fitTarget.key


class TargetView(d.Display):

    DEFAULT_COLS = ['State',
                    'Base Icon',
                    'Base Name',
                    'SigRadius',
                    'Speed',
                    'Target Resists']

    def __init__(self, parent):
        d.Display.__init__(self, parent, style=wx.BORDER_NONE)

        self.lastFitId = None
        self.rows = []

        self.mainFrame.Bind(GE.FIT_CHANGED, self.fitChanged)
        self.mainFrame.Bind(GE.FIT_REMOVED, self.OnFitRemoved)
        self.mainFrame.Bind(GE.TARGET_PROFILE_RENAMED, self.OnProfileChanged)
        self.mainFrame.Bind(GE.TARGET_PROFILE_CHANGED, self.OnProfileChanged)
        self.mainFrame.Bind(GE.TARGET_PROFILE_REMOVED, self.OnProfileChanged)
        self.Bind(wx.EVT_LEFT_DOWN, self.click)
        self.Bind(wx.EVT_LEFT_DCLICK, self.onLeftDoubleClick)
        self.Bind(wx.EVT_KEY_UP, self.kbEvent)

        self.Bind(wx.EVT_CONTEXT_MENU, self.spawnMenu)

    def _refreshFromOutside(self, changedKey):
        # A fit or profile changed elsewhere (deleted, edited): show it, and if the
        # active fit's list had it, tell everybody the fit's targets changed
        fitID = self.mainFrame.getActiveFit()
        hadTarget = any(r.key == changedKey for r in self.rows)
        self.refreshContents(Fit.getInstance().getFit(fitID))
        if hadTarget and fitID is not None:
            wx.PostEvent(self.mainFrame, GE.FitChanged(fitIDs=(fitID,)))

    def OnFitRemoved(self, event):
        event.Skip()
        self._refreshFromOutside((KIND_FIT, event.fitID))

    def OnProfileChanged(self, event):
        event.Skip()
        self._refreshFromOutside((KIND_PROFILE, event.profileID))

    def kbEvent(self, event):
        keycode = event.GetKeyCode()
        modifiers = event.GetModifiers()
        if keycode == wx.WXK_ESCAPE and modifiers == wx.MOD_NONE:
            self.unselectAll()
        elif keycode == 65 and modifiers == wx.MOD_CONTROL:
            self.selectAll()
        elif keycode in (wx.WXK_DELETE, wx.WXK_NUMPAD_DELETE) and modifiers == wx.MOD_NONE:
            self.removeTargets(self.getSelectedTargets())
        event.Skip()

    def handleDrag(self, type, fitID):
        # Those are drags coming from pyfa sources, NOT builtin wx drags
        if type == 'fit':
            self.addFitsByIDs([fitID])

    def fitChanged(self, event):
        event.Skip()
        activeFitID = self.mainFrame.getActiveFit()
        # Also refresh when one of the shown target fits changed
        targetFitIDs = {r.item.ID for r in self.rows if r.isFit}
        if activeFitID is not None and activeFitID not in event.fitIDs and not targetFitIDs.intersection(event.fitIDs):
            return

        sFit = Fit.getInstance()
        fit = sFit.getFit(activeFitID)

        self.Parent.Parent.Parent.DisablePage(self.Parent, not fit)  # the tab page is TargetPanel

        # Clear list and get out if current fitId is None
        if activeFitID is None and self.lastFitId is not None:
            self.DeleteAllItems()
            self.rows = []
            self.lastFitId = None
            return

        if activeFitID != self.lastFitId:
            self.lastFitId = activeFitID

            item = self.GetNextItem(-1, wx.LIST_NEXT_ALL, wx.LIST_STATE_DONTCARE)

            if item != -1:
                self.EnsureVisible(item)

            self.unselectAll()

        self.refreshContents(fit)

    @staticmethod
    def rowSort(row):
        # Fits first, then profiles, by name
        return row.isProfile, smartSort(row.name)

    def refreshContents(self, fit):
        self.rows = []
        if fit is not None:
            for fitTarget in fit.targets:
                item = fitTarget.target
                # The columns read the target fit's attributes, so it has to be calculated
                if fitTarget.isFit:
                    item = getTargetFit(item)
                    if item is None:
                        continue
                self.rows.append(TargetRow(fitTarget, item))
            self.rows.sort(key=self.rowSort)
        stuff = self.rows
        if not stuff:
            stuff = [DummyEntry(_t('Drag a fit here, or use the right-click menu to add a target profile'))]
        self.update(stuff)

    def get(self, row):
        if 0 <= row < len(self.rows):
            return self.rows[row]
        return None

    def click(self, event):
        mainRow, _ = self.HitTest(event.Position)
        if mainRow != -1:
            col = self.getColumn(event.Position)
            if col == self.getColIndex(State):
                mainTarget = self.get(mainRow)
                if mainTarget is None:
                    return
                self.mainFrame.command.Submit(cmd.GuiToggleFitTargetStateCommand(
                    fitID=self.mainFrame.getActiveFit(),
                    targetKey=mainTarget.key))
                return
        event.Skip()

    def spawnMenu(self, event):
        clickedPos = self.getRowByAbs(event.Position)
        self.ensureSelection(clickedPos)

        fitID = self.mainFrame.getActiveFit()
        if fitID is None:
            return

        if self.getColumn(self.screenToClientFixed(event.Position)) == self.getColIndex(State):
            return

        mainTarget = self.get(clickedPos)
        contexts = []
        if mainTarget is not None:
            contexts.append(('targetItem', _t('Target')))
        contexts.append(('targetView',))
        menu = ContextMenu.getMenu(self, mainTarget, self.getSelectedTargets(), *contexts)
        if menu is not None:
            self.PopupMenu(menu)

    def onLeftDoubleClick(self, event):
        row, _ = self.HitTest(event.Position)
        if row != -1:
            col = self.getColumn(event.Position)
            if col != self.getColIndex(State):
                mainTarget = self.get(row)
                if mainTarget is not None:
                    self.removeTargets([mainTarget])

    def getSelectedTargets(self):
        targets = []
        for row in self.getSelectedRows():
            target = self.get(row)
            if target is not None:
                targets.append(target)
        return targets

    def removeTargets(self, targets):
        targetKeys = [t.key for t in targets if t in self.rows]
        if not targetKeys:
            return
        self.mainFrame.command.Submit(cmd.GuiRemoveFitTargetsCommand(
            fitID=self.mainFrame.getActiveFit(),
            targetKeys=targetKeys))

    # Context menu handlers
    def addFit(self, fit):
        if fit is None:
            return
        self.addFitsByIDs([fit.ID])

    def getExistingFitIDs(self):
        # The fit itself can't be its own target either
        return [r.item.ID for r in self.rows if r.isFit] + [self.mainFrame.getActiveFit()]

    def addFitsByIDs(self, fitIDs):
        fitID = self.mainFrame.getActiveFit()
        if fitID is None:
            return
        targetKeys = [(KIND_FIT, i) for i in fitIDs if i != fitID]
        if not targetKeys:
            return
        self.mainFrame.command.Submit(cmd.GuiAddFitTargetsCommand(fitID=fitID, targetKeys=targetKeys))

    def addProfile(self, profile):
        fitID = self.mainFrame.getActiveFit()
        if fitID is None or profile is None:
            return
        self.mainFrame.command.Submit(cmd.GuiAddFitTargetsCommand(
            fitID=fitID,
            targetKeys=[getTargetKey(profile)]))

    def getTabExtraText(self):
        fitID = self.mainFrame.getActiveFit()
        if fitID is None:
            return None
        sFit = Fit.getInstance()
        fit = sFit.getFit(fitID)
        if fit is None:
            return None
        opt = sFit.serviceFittingOptions["additionsLabels"]
        # Amount of active targets
        if opt == 1:
            amount = 1 if fit.activeTarget is not None else 0
            return ' ({})'.format(amount) if amount else None
        # Total amount of targets
        elif opt == 2:
            amount = len(fit.targets)
            return ' ({})'.format(amount) if amount else None
        else:
            return None


class TargetPanel(wx.Panel):
    """The Target tab: the engagement (range and target speed, shared by the Firepower panel's
    applied damage and the module optimizer) above the fit's target list."""

    def __init__(self, parent):
        wx.Panel.__init__(self, parent)
        from service import appliedDamage
        self.mainFrame = gui.mainFrame.MainFrame.getInstance()
        distance, speedFactor = appliedDamage.engagement()
        sizer = wx.BoxSizer(wx.VERTICAL)
        row = wx.BoxSizer(wx.HORIZONTAL)
        rangeLabel = wx.StaticText(self, label=_t('Range (km):'))
        row.Add(rangeLabel, 0, wx.ALIGN_CENTER_VERTICAL | wx.LEFT | wx.RIGHT, 4)
        self.distance = wx.SpinCtrlDouble(self, min=0, max=300, inc=0.5, initial=distance / 1000)
        self.distance.SetDigits(1)
        tip = _t('Distance to the enabled target, surface to surface. The Firepower panel\'s applied damage and '
                 'the module optimizer\'s applied damage goal use it.')
        rangeLabel.SetToolTip(tip)
        self.distance.SetToolTip(tip)
        row.Add(self.distance, 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 10)
        speedLabel = wx.StaticText(self, label=_t('Target speed (%):'))
        row.Add(speedLabel, 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 4)
        self.speed = wx.SpinCtrl(self, min=0, max=100, initial=int(round(speedFactor * 100)))
        tip = _t('The share of its maximum velocity the target moves at, straight across (you stand still). '
                 'Your webs, target painters and scramblers act on it.')
        speedLabel.SetToolTip(tip)
        self.speed.SetToolTip(tip)
        row.Add(self.speed, 0, wx.ALIGN_CENTER_VERTICAL)
        sizer.Add(row, 0, wx.TOP | wx.BOTTOM, 3)
        self.view = TargetView(self)
        sizer.Add(self.view, 1, wx.EXPAND)
        self.SetSizer(sizer)
        self._pending = None
        for control, event in ((self.distance, wx.EVT_SPINCTRLDOUBLE), (self.speed, wx.EVT_SPINCTRL),
                               (self.distance, wx.EVT_TEXT), (self.speed, wx.EVT_TEXT)):
            control.Bind(event, self.onChange)

    def onChange(self, event):
        event.Skip()
        # Spinning fires many events: apply the last one
        if self._pending is not None:
            self._pending.Stop()
        self._pending = wx.CallLater(300, self.apply)

    def apply(self):
        self._pending = None
        from service import appliedDamage
        appliedDamage.setEngagement(distance=self.distance.GetValue() * 1000, speedFactor=self.speed.GetValue() / 100)
        fitID = self.mainFrame.getActiveFit()
        if fitID is not None:
            # Targets and the engagement don't change the fit's own calculation: no recalc
            wx.PostEvent(self.mainFrame, GE.FitChanged(fitIDs=(fitID,)))

    def getTabExtraText(self):
        return self.view.getTabExtraText()

