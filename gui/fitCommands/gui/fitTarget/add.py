import wx

import eos.db
import gui.mainFrame
from gui import globalEvents as GE
from gui.fitCommands.calc.fitTarget.add import CalcAddFitTargetCommand
from gui.fitCommands.helpers import InternalCommandHistory


class GuiAddFitTargetsCommand(wx.Command):

    def __init__(self, fitID, targetKeys):
        """targetKeys: ('fit', fitID) / ('profile', profileID) tuples, see FitTarget.key"""
        wx.Command.__init__(self, True, 'Add Fit Targets')
        self.internalHistory = InternalCommandHistory()
        self.fitID = fitID
        self.targetKeys = targetKeys

    def Do(self):
        results = []
        for targetKey in self.targetKeys:
            cmd = CalcAddFitTargetCommand(fitID=self.fitID, targetKey=targetKey)
            results.append(self.internalHistory.submit(cmd))
        success = any(results)
        # Targets don't take part in the fit's calculation, no recalc needed
        eos.db.commit()
        wx.PostEvent(gui.mainFrame.MainFrame.getInstance(), GE.FitChanged(fitIDs=(self.fitID,)))
        return success

    def Undo(self):
        success = self.internalHistory.undoAll()
        eos.db.commit()
        wx.PostEvent(gui.mainFrame.MainFrame.getInstance(), GE.FitChanged(fitIDs=(self.fitID,)))
        return success
