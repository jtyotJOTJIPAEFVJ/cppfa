import wx

import eos.db
import gui.mainFrame
from gui import globalEvents as GE
from gui.fitCommands.calc.fitTarget.remove import CalcRemoveFitTargetCommand
from gui.fitCommands.helpers import InternalCommandHistory


class GuiRemoveFitTargetsCommand(wx.Command):

    def __init__(self, fitID, targetKeys):
        wx.Command.__init__(self, True, 'Remove Fit Targets')
        self.internalHistory = InternalCommandHistory()
        self.fitID = fitID
        self.targetKeys = targetKeys

    def Do(self):
        results = []
        for targetKey in self.targetKeys:
            cmd = CalcRemoveFitTargetCommand(fitID=self.fitID, targetKey=targetKey)
            results.append(self.internalHistory.submit(cmd))
        success = any(results)
        eos.db.commit()
        wx.PostEvent(gui.mainFrame.MainFrame.getInstance(), GE.FitChanged(fitIDs=(self.fitID,)))
        return success

    def Undo(self):
        success = self.internalHistory.undoAll()
        eos.db.commit()
        wx.PostEvent(gui.mainFrame.MainFrame.getInstance(), GE.FitChanged(fitIDs=(self.fitID,)))
        return success
