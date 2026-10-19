"""Drive pyfa's Optimize Modules dialog end to end, headless apart from the windows it opens.

Starts pyfa (main frame) on a *copy* of saveddata.db, opens the dialog for one fit and a
set of slots, runs the optimizer with the given options, prints the result list (as
the dialog shows it), and optionally applies an option and prints the fit's slots after.
Your own saveddata.db is never touched.

Run with pyfa's environment (scripts/setup-wsl-venv.sh, then scripts/wsl-python.sh):
    scripts/wsl-python.sh cpp/tools/optimizer_dialog_test.py FIT_ID GOAL [key=value ...]

Options:
    save=PATH          saveddata.db to copy (default: the first of ~/.pyfa and the Windows
                       profile's .pyfa that has the fit)
    positions=1,2,3    fit.modules positions to optimize (default: every high/mid/low/rig slot)
    slots=high,med,..  racks to optimize when positions isn't given
    time=S             time budget (default 10)
    maxReps=N capStable=0|1 ancillary=0|1 capBoosters=0|1 minCap=GJ/s weight=W priceCap=MISK
    apply=ROW          apply this list row (0 = the original modules) and print the slots after
"""
import os
import shutil
import sys
import tempfile

PYFA = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
sys.path.insert(0, PYFA)
os.chdir(PYFA)

FIT, GOAL = int(sys.argv[1]), sys.argv[2]
OPTS = dict(a.split('=', 1) for a in sys.argv[3:])


def defaultSave():
    import glob
    import sqlite3
    for path in [os.path.expanduser('~/.pyfa/saveddata.db')] + glob.glob('/mnt/c/Users/*/.pyfa/saveddata.db'):
        try:
            with sqlite3.connect('file:{}?mode=ro'.format(path), uri=True) as db:
                if db.execute('SELECT 1 FROM fits WHERE ID = ?', (FIT,)).fetchone():
                    return path
        except sqlite3.Error:
            continue
    sys.exit('no saveddata.db with fit {} found: pass save=PATH'.format(FIT))


tmp = tempfile.mkdtemp(prefix='pyfa_dialog_test_')
shutil.copy2(OPTS.get('save') or defaultSave(), os.path.join(tmp, 'saveddata.db'))


def quit(code):
    """Exits without tearing the GUI down (wx under WSLg can crash on teardown)."""
    sys.stdout.flush()
    shutil.rmtree(tmp, ignore_errors=True)
    os._exit(code)

import config  # noqa: E402

config.defPaths(tmp)
config.defLogging()
import wx  # noqa: E402

with config.logging_setup:
    import eos.db  # noqa: F401
    import eos.events  # noqa: F401
    import service.prefetch  # noqa: F401

    eos.db.saveddata_meta.create_all(eos.db.saveddata_engine)
    from gui.app import PyfaApp

    app = PyfaApp(False)
    from eos.const import FittingSlot
    from gui.mainFrame import MainFrame
    from gui.moduleOptimizerDialog import ModuleOptimizerDialog
    from service import moduleOptimizer as mo
    from service.fit import Fit

    mf = MainFrame('pyfa optimizer dialog test')
    state = {}

    def fail(message):
        print(message)
        quit(1)

    def start():
        fit = Fit.getInstance().getFit(FIT)
        if fit is None:
            return fail('fit {} not found'.format(FIT))
        if 'positions' in OPTS:
            positions = [int(x) for x in OPTS['positions'].split(',')]
        else:
            racks = {'high': FittingSlot.HIGH, 'med': FittingSlot.MED, 'low': FittingSlot.LOW, 'rig': FittingSlot.RIG}
            want = [racks[r] for r in OPTS.get('slots', 'high,med,low,rig').split(',')]
            positions = [p for p, m in enumerate(fit.modules) if m.slot in want]
        dlg = ModuleOptimizerDialog.open(mf, FIT, positions)
        dlg.goalChoice.SetSelection(mo.GOALS.index(GOAL))
        dlg.onGoalChanged(None)
        dlg.maxReps.SetValue(int(OPTS.get('maxReps', 0)))
        dlg.requireCapStable.SetValue(OPTS.get('capStable') == '1')
        dlg.allowAncillary.SetValue(OPTS.get('ancillary', '1') == '1')
        dlg.allowCapBoosters.SetValue(OPTS.get('capBoosters', '1') == '1')
        dlg.minCapRecharge.SetValue(float(OPTS.get('minCap', 0)))
        dlg.timeBudget.SetValue(int(OPTS.get('time', 10)))
        dlg.remoteWeight.SetValue(float(OPTS.get('weight', 4)))
        dlg.priceCap.SetValue(float(OPTS.get('priceCap', 0)))
        state['dlg'] = dlg
        dlg.onRun(None)
        wx.CallLater(1000, poll)

    def poll():
        if state['dlg'].run is not None:
            wx.CallLater(1000, poll)
        else:
            wx.CallLater(500, report)

    def report():
        dlg = state['dlg']
        if dlg.request is None:
            return fail('the optimizer did not run (see the log)')
        print('slots:', dlg.slotsText.GetLabel())
        print('request:', {k: dlg.request.get(k) for k in (
            'goal', 'allowAncillary', 'allowCapBoosters', 'maxReps', 'requireCapStable', 'minCapRecharge', 'remoteWeight', 'priceCap')})
        print('phase:', dlg.phaseText.GetLabel())
        print('warnings:', dlg.warningText.GetLabel())
        lst = dlg.list
        print(' | '.join(lst.GetColumn(c).GetText() for c in range(lst.GetColumnCount())))
        for r in range(lst.GetItemCount()):
            print(' | '.join(lst.GetItemText(r, c) for c in range(lst.GetColumnCount())))
        if 'apply' in OPTS:
            dlg.applyRow(int(OPTS['apply']))
            fit = Fit.getInstance().getFit(FIT)
            for pos in dlg.positions:
                m = fit.modules[pos]
                text = 'empty' if m.isEmpty else m.item.name
                if not m.isEmpty and m.charge is not None:
                    text += ' [{}]'.format(m.charge.name)
                if not m.isEmpty and m.isMutated:
                    text += ' (mutated: mutaplasmid {}, {} mutators)'.format(m.mutaplasmidID, len(m.mutators))
                print('after apply: position {}: {}'.format(pos, text))
        # Let pyfa handle the fit-changed events first.
        wx.CallLater(2000, quit, 0)

    wx.CallLater(3000, start)
    app.MainLoop()
