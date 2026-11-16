import wx
from logbook import Logger

import eos.db
from service.fit import Fit


pyfalog = Logger(__name__)


class CalcRemoveFitTargetCommand(wx.Command):

    def __init__(self, fitID, targetKey):
        wx.Command.__init__(self, True, 'Remove Fit Target')
        self.fitID = fitID
        self.targetKey = targetKey
        self.savedActive = None

    def Do(self):
        pyfalog.debug('Doing removal of target {} for fit {}'.format(self.targetKey, self.fitID))
        fit = Fit.getInstance().getFit(self.fitID)
        if fit is None:
            return False
        fitTarget = next((t for t in fit.fitTargets if t.key == self.targetKey), None)
        if fitTarget is None:
            pyfalog.debug('Target is not on the list')
            return False
        self.savedActive = fitTarget.active
        fit.fitTargets.remove(fitTarget)
        eos.db.saveddata_session.flush()
        return True

    def Undo(self):
        pyfalog.debug('Undoing removal of target {} for fit {}'.format(self.targetKey, self.fitID))
        from .add import CalcAddFitTargetCommand
        cmd = CalcAddFitTargetCommand(fitID=self.fitID, targetKey=self.targetKey, active=self.savedActive)
        return cmd.Do()
