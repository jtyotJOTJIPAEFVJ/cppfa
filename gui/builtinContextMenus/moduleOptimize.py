import wx

import gui.mainFrame
from eos.saveddata.module import Module
from gui.contextMenu import ContextMenuSingle
from service.fit import Fit
from service.moduleOptimizer import isOptimizable

_t = wx.GetTranslation


class OptimizeModules(ContextMenuSingle):
    """
    Optimize modules...: runs the C++ module optimizer on the selected high, mid, low and
    rig slots, empty ones included, and lists what it finds (gui.moduleOptimizerDialog).
    """

    def __init__(self):
        self.mainFrame = gui.mainFrame.MainFrame.getInstance()
        self.positions = []

    def display(self, callingWindow, srcContext, mainItem):
        if self.mainFrame.getActiveFit() is None:
            return False
        # Shown once: with the module's items for a module, with the ship's for an empty slot
        isModule = isinstance(mainItem, Module) and not mainItem.isEmpty
        if srcContext != ('fittingModule' if isModule else 'fittingShip'):
            return False
        # The menu's own selection leaves out empty slots, the fitting view's doesn't
        getSelected = getattr(callingWindow, 'getSelectedMods', None)
        if getSelected is None:
            return False
        fit = Fit.getInstance().getFit(self.mainFrame.getActiveFit())
        if fit is None:
            return False
        selected = [mod for mod in getSelected() if isinstance(mod, Module) and isOptimizable(mod)]
        self.positions = [p for p, mod in enumerate(fit.modules) if any(mod is s for s in selected)]
        return bool(self.positions)

    def getText(self, callingWindow, itmContext, mainItem):
        if len(self.positions) > 1:
            return _t('Optimize Modules ({} slots)...').format(len(self.positions))
        return _t('Optimize Module...')

    def activate(self, callingWindow, fullContext, mainItem, i):
        from gui.moduleOptimizerDialog import ModuleOptimizerDialog
        fitID = self.mainFrame.getActiveFit()
        if fitID is None or not self.positions:
            return
        ModuleOptimizerDialog.open(self.mainFrame, fitID, self.positions)


OptimizeModules.register()
