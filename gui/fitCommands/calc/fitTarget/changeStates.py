import wx
from logbook import Logger

import eos.db
from service.fit import Fit


pyfalog = Logger(__name__)


class CalcChangeFitTargetStatesCommand(wx.Command):

    def __init__(self, fitID, states):
        """states maps target keys to the active flag they get"""
        wx.Command.__init__(self, True, 'Change Fit Target States')
        self.fitID = fitID
        self.states = states
        self.savedStates = None

    def Do(self):
        pyfalog.debug('Doing changing of target states to {} for fit {}'.format(self.states, self.fitID))
        fit = Fit.getInstance().getFit(self.fitID)
        if fit is None:
            return False
        self.savedStates = {t.key: t.active for t in fit.fitTargets}
        changed = False
        for fitTarget in fit.fitTargets:
            state = self.states.get(fitTarget.key)
            if state is not None and state != fitTarget.active:
                fitTarget.active = state
                changed = True
        if not changed:
            return False
        eos.db.saveddata_session.flush()
        return True

    def Undo(self):
        pyfalog.debug('Undoing changing of target states to {} for fit {}'.format(self.states, self.fitID))
        fit = Fit.getInstance().getFit(self.fitID)
        if fit is None:
            return False
        for fitTarget in fit.fitTargets:
            state = self.savedStates.get(fitTarget.key)
            if state is not None:
                fitTarget.active = state
        eos.db.saveddata_session.flush()
        return True
