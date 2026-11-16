import wx

import eos.db
import gui.mainFrame
from gui import globalEvents as GE
from gui.fitCommands.calc.fitTarget.changeStates import CalcChangeFitTargetStatesCommand
from gui.fitCommands.helpers import InternalCommandHistory
from service.fit import Fit


class GuiToggleFitTargetStateCommand(wx.Command):

    def __init__(self, fitID, targetKey):
        """Enables the target (disabling the others), or disables it if it is the enabled one"""
        wx.Command.__init__(self, True, 'Toggle Fit Target State')
        self.internalHistory = InternalCommandHistory()
        self.fitID = fitID
        self.targetKey = targetKey

    def Do(self):
        fit = Fit.getInstance().getFit(self.fitID)
        if fit is None:
            return False
        mainTarget = next((t for t in fit.fitTargets if t.key == self.targetKey), None)
        if mainTarget is None:
            return False
        if mainTarget.active:
            states = {self.targetKey: False}
        else:
            states = {t.key: t.key == self.targetKey for t in fit.fitTargets}
        cmd = CalcChangeFitTargetStatesCommand(fitID=self.fitID, states=states)
        success = self.internalHistory.submit(cmd)
        eos.db.commit()
        wx.PostEvent(gui.mainFrame.MainFrame.getInstance(), GE.FitChanged(fitIDs=(self.fitID,)))
        return success

    def Undo(self):
        success = self.internalHistory.undoAll()
        eos.db.commit()
        wx.PostEvent(gui.mainFrame.MainFrame.getInstance(), GE.FitChanged(fitIDs=(self.fitID,)))
        return success
