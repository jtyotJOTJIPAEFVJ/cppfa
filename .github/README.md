# cppfa

A fork of [pyfa](https://github.com/pyfa-org/Pyfa), the python fitting assistant for EVE Online,
that adds a module optimizer: a C++ reimplementation of pyfa's tank, fitting and capacitor
calculation, and a search on top of it that proposes modules for the slots you select.

This is not official pyfa and is not supported by the pyfa developers. For pyfa itself (releases,
documentation, bug reports) go to [pyfa-org/Pyfa](https://github.com/pyfa-org/Pyfa). Bugs in the
optimizer belong here.

## What this fork changes

Everything else is pyfa as it is upstream; the branch `cppfa` is kept rebased on pyfa's `master`.

### The optimizer (`cpp/`)

`PyfaReader.exe` is a Windows C++ program (Visual Studio solution `cpp/PyfaReader.sln`).

- **Fit engine.** It recomputes the part of pyfa's fit calculation that tank optimization needs:
  ship, skills, implants and boosters, modules and charges, command bursts, mutated modules,
  stacking penalties, fitting (CPU, powergrid, calibration) and capacitor. It reads pyfa's
  `eve.db` and its effect definitions, generated from pyfa's own `eos/effects.py`, and is
  checked fit by fit against pyfa's results.
- **Module optimizer.** For the selected slots it searches every published module that fits
  them (and their charges), and returns the best fitting options for one goal:
  - passive EHP of a layer
  - sustained local shield or armor repair (EHP/s)
  - resists only
  - sustained remote shield or armor repair (HP/s)
  - local tank and remote repair balanced against each other

  Constraints: an ISK price cap per module, officer modules, ancillary repairers, capacitor
  boosters, the number of repairers, capacitor stability and a minimum capacitor recharge.
  Everything outside the selection stays as it is, with its state, charge and mutations.

### Hooks into pyfa

- **"Optimize Modules..."** in the fitting view's right-click menu, on selected high, mid, low
  and rig slots (empty ones included); `gui/builtinContextMenus/moduleOptimize.py`.
- **The optimizer dialog** (`gui/moduleOptimizerDialog.py`): goal and constraints, progress and
  cancel, the options found with their stats and price, and applying one to the fit as a single
  undoable step (`gui/fitCommands/gui/localModule/optimizerApply.py`).
- **`service/moduleOptimizer.py`** builds the request from the fit (character skills, implants,
  boosters, module states and mutations, prices from pyfa's price cache), runs the exe and reads
  its answer; the dialog's settings are `ModuleOptimizerSettings` in `service/settings.py`.
- **The Windows build** bundles the exe and its data (`pyfa.spec`).

Documentation of the engine and the optimizer: [`cpp/README.md`](../cpp/README.md) (building,
regenerating data, validation), [`cpp/FIT_ENGINE.md`](../cpp/FIT_ENGINE.md),
[`cpp/OPTIMIZER.md`](../cpp/OPTIMIZER.md), [`cpp/OPTIMIZER_CLI.md`](../cpp/OPTIMIZER_CLI.md).

## Building

Running pyfa from source works as in upstream pyfa: see [CONTRIBUTING.md](../CONTRIBUTING.md). In
short, with [uv](https://docs.astral.sh/uv/):

    uv sync
    uv run python scripts/compile_lang.py
    uv run python db_update.py
    uv run python pyfa.py

### Requirements for the optimizer (Windows)

- **Visual Studio 2026** (any edition) or the free **Build Tools for Visual Studio 2026**, with the
  **Desktop development with C++** workload (it brings the v145 compiler and a Windows SDK).
  Visual Studio 2022 doesn't have the v145 toolset and can't build the project as it is.
- **[uv](https://docs.astral.sh/uv/)**, a recent version, on the Windows `PATH`. It installs the
  Python version and packages the build uses; nothing else Python-related is needed.
- **Git for Windows** is optional: `update_cpp_data.ps1` uses it at the end to list the
  regenerated files.

The built exe needs nothing installed: it links the C runtime statically.

### Build scripts

Run them from the repository root. PowerShell doesn't run scripts by default, so either call them
as `powershell -ExecutionPolicy Bypass -File scripts\<script>.ps1 ...`, or allow local scripts once
with `Set-ExecutionPolicy -Scope CurrentUser RemoteSigned` and call `scripts\<script>.ps1 ...`.

- `build_reader.ps1 -NoCopy` builds `cpp\bin\Release\PyfaReader.exe`, where pyfa looks for it
  (the dialog can also point to another path).
- `build_pyfa.ps1 -NoCopy` builds the Windows pyfa (PyInstaller) with the optimizer bundled, in
  `dist\pyfa`.
- After pulling in a new pyfa, `update_cpp_data.ps1 -NoCopy` regenerates `eve.db` and the
  optimizer's data from it, then rebuilds the exe.

Each script needs either `-NoCopy` or `-PyfaInstall <dir>`, which also copies the result into
the pyfa installed there. There is no default, so a dev build never lands on a pyfa by accident.
To run pyfa under WSL, see `scripts/setup-wsl-venv.sh` and [`cpp/README.md`](../cpp/README.md).

## License

pyfa, and this fork with it, is licensed under the GNU GPL v3.0, see [LICENSE](../LICENSE).

## CCP Copyright Notice
EVE Online, the EVE logo, EVE and all associated logos and designs are the intellectual property of CCP hf. All artwork, screenshots, characters, vehicles, storylines, world facts or other recognizable features of the intellectual property relating to these trademarks are likewise the intellectual property of CCP hf. EVE Online and the EVE logo are the registered trademarks of CCP hf. All rights are reserved worldwide. All other trademarks are the property of their respective owners. CCP hf. has granted permission to pyfa to use EVE Online and all associated logos and designs for promotional and information purposes on its website but does not endorse, and is not in any way affiliated with, pyfa. CCP is in no way responsible for the content on or functioning of this program, nor can it be liable for any damage arising from the use of this program.
