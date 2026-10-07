import wx

import eos.db
import gui.mainFrame
from gui import globalEvents as GE
from gui.fitCommands.calc.module.localRemove import CalcRemoveLocalModulesCommand
from gui.fitCommands.calc.module.localReplace import CalcReplaceLocalModuleCommand
from gui.fitCommands.helpers import InternalCommandHistory, ModuleInfo, restoreRemovedDummies
from service.fit import Fit


class GuiApplyOptimizedModulesCommand(wx.Command):
    """
    Put modules into several slots as one undoable step, as the module optimizer proposes
    them: modInfos is {position: ModuleInfo, or None to leave the slot empty}. Slots that
    already hold what is wanted (module, charge, state, mutations) are left alone.
    """

    def __init__(self, fitID, modInfos, name='Apply Optimized Modules'):
        wx.Command.__init__(self, True, name)
        self.internalHistory = InternalCommandHistory()
        self.fitID = fitID
        self.modInfos = modInfos
        self.savedRemovedDummies = None

    def Do(self):
        sFit = Fit.getInstance()
        fit = sFit.getFit(self.fitID)
        changes = {}
        for position, modInfo in self.modInfos.items():
            if position >= len(fit.modules):
                continue
            mod = fit.modules[position]
            current = None if mod.isEmpty else ModuleInfo.fromModule(mod)
            if current != modInfo:
                changes[position] = modInfo
        if not changes:
            return False
        # Empty every changed slot first, then fill them: replacing one at a time could break
        # group limits on the way (e.g. moving a damage control to another slot)
        toRemove = [p for p in sorted(changes) if not fit.modules[p].isEmpty]
        results = []
        if toRemove:
            results.append(self.internalHistory.submit(CalcRemoveLocalModulesCommand(
                fitID=self.fitID, positions=toRemove, recalc=False)))
        toAdd = [p for p in sorted(changes) if changes[p] is not None]
        for position in toAdd:
            # With recalc, as the restriction check (free slots) relies on the flushed fit
            results.append(self.internalHistory.submit(CalcReplaceLocalModuleCommand(
                fitID=self.fitID, position=position, newModInfo=changes[position],
                unloadInvalidCharges=True)))
        success = any(results)
        eos.db.flush()
        sFit.recalc(self.fitID)
        self.savedRemovedDummies = sFit.fill(self.fitID)
        eos.db.commit()
        wx.PostEvent(gui.mainFrame.MainFrame.getInstance(), GE.FitChanged(fitIDs=(self.fitID,)))
        return success

    def Undo(self):
        sFit = Fit.getInstance()
        fit = sFit.getFit(self.fitID)
        restoreRemovedDummies(fit, self.savedRemovedDummies)
        success = self.internalHistory.undoAll()
        eos.db.flush()
        sFit.recalc(self.fitID)
        sFit.fill(self.fitID)
        eos.db.commit()
        wx.PostEvent(gui.mainFrame.MainFrame.getInstance(), GE.FitChanged(fitIDs=(self.fitID,)))
        return success
