import wx
from logbook import Logger

import eos.db
from eos.saveddata.fitTarget import FitTarget
from service.fit import Fit
from service.target import resolveTargetKey


pyfalog = Logger(__name__)


class CalcAddFitTargetCommand(wx.Command):

    def __init__(self, fitID, targetKey, active=None):
        """
        targetKey is ('fit', fitID) or ('profile', profileID). active None enables the
        new target if the fit has no enabled target; True enables it and disables the
        others, False adds it disabled.
        """
        wx.Command.__init__(self, True, 'Add Fit Target')
        self.fitID = fitID
        self.targetKey = targetKey
        self.active = active
        self.savedStates = None

    def Do(self):
        pyfalog.debug('Doing addition of target {} for fit {}'.format(self.targetKey, self.fitID))
        fit = Fit.getInstance().getFit(self.fitID)
        if fit is None:
            return False
        target = resolveTargetKey(self.targetKey)
        if target is None:
            pyfalog.debug('Target is not available')
            return False
        # A fit can't target itself, and holds each target once
        if self.targetKey == ('fit', self.fitID):
            return False
        if any(t.key == self.targetKey for t in fit.fitTargets):
            pyfalog.debug('Target is already on the list')
            return False
        active = self.active
        if active is None:
            active = fit.activeTarget is None
        self.savedStates = {t.key: t.active for t in fit.fitTargets}
        if active:
            for fitTarget in fit.fitTargets:
                fitTarget.active = False
        fit.fitTargets.append(FitTarget(target, active))
        eos.db.saveddata_session.flush()
        return True

    def Undo(self):
        pyfalog.debug('Undoing addition of target {} for fit {}'.format(self.targetKey, self.fitID))
        fit = Fit.getInstance().getFit(self.fitID)
        if fit is None:
            return False
        fitTarget = next((t for t in fit.fitTargets if t.key == self.targetKey), None)
        if fitTarget is None:
            return False
        fit.fitTargets.remove(fitTarget)
        for t in fit.fitTargets:
            if t.key in self.savedStates:
                t.active = self.savedStates[t.key]
        eos.db.saveddata_session.flush()
        return True
