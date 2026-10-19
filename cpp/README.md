# C++ fit engine and module optimizer

`PyfaReader` is a C++ reimplementation of pyfa's tank, fitting and capacitor calculation, plus
a module optimizer built on it. pyfa's "Optimize Modules..." dialog runs it as a separate exe.
Documentation:

- `FIT_ENGINE.md`: the engine, its data and how it is checked against pyfa
- `OPTIMIZER_PLAN.md`, `OPTIMIZER_STATUS.md`: how the optimizer works, what was measured
- `OPTIMIZER_CLI.md`: the command line and the JSON request/response format
- `OPTIMIZER_STUDY.md`, `OPTIMIZER_PERF_PLAN.md`: the refine study and the stage 1 speedups

## Building

The engine builds with Visual Studio (Release x64):

    MSBuild.exe cpp\PyfaReader.sln -p:Configuration=Release -p:Platform=x64 -m -v:minimal

The exe is `cpp\bin\Release\PyfaReader.exe`. It is a Windows program: paths passed to it
are Windows paths. Scripts (PowerShell, from the repo root):

- `scripts\build_reader.ps1`: builds the exe (and copies it into an installed pyfa)
- `scripts\update_cpp_data.ps1`: after updating pyfa, regenerates eve.db, the engine's
  effect data (`PyfaReader\data\*.tsv`, by `tools/gen_effect_modifiers.py`) and
  `src\generated\FitSchema.generated.h` (by `tools/gen_fit_schema.py`; regenerate it rather
  than editing it), then builds the exe
- `scripts\build_pyfa.ps1`: the PyInstaller build of pyfa with the exe bundled, copied over
  an installed pyfa

## Running pyfa under WSL

`scripts/setup-wsl-venv.sh` creates `.venv` with the wxPython binary wheel and extracts the
shared libraries that wheel needs but newer Ubuntus lack into `.venv/syslib` (no root needed).
Run it again after `uv.lock` changes. Then run Python through `scripts/wsl-python.sh
<script.py> [args]`, which puts those libraries on `LD_LIBRARY_PATH`.

## Validation

Run these after engine or optimizer changes:

- `PyfaReader.exe --search-check 400`: search plan vs full evaluation, all goals; expect 0
  mismatches.
- `PyfaReader.exe --optimize-check <fitIDs> [goal|all] [--racks mid,low,rig,high]`: stage 1 vs
  brute force on small slot sets; expect 0 mismatches.
- Engine vs pyfa itself, on a normalized copy of the saved fits:

      scripts/wsl-python.sh cpp/tools/pyfa_reference.py ref.jsonl --save <saveddata.db> --keep-db <WINDOWS_DIR>/normalized.db
      PyfaReader.exe --save '<WIN_PATH>\normalized.db' --engine-dump '<WIN_PATH>\engine.tsv'
      python3 cpp/tools/compare_engine.py ref.jsonl <WINDOWS_DIR>/engine.tsv

  11 fits differ for known reasons (`FIT_ENGINE.md`, "Engine vs pyfa"); anything else is new.
- Unchanged results after a refactor: run `--optimize <fit> <goal> --threads 8 --time-budget 0
  --refine-rounds 15` on a few fits with the old and the new build (the old one from a
  temporary `git worktree`) and diff the option lines.
- The pyfa dialog end to end (opens real windows; works on a temporary copy of saveddata.db):

      scripts/wsl-python.sh cpp/tools/optimizer_dialog_test.py <fitID> <goal> positions=0,1 [apply=ROW] [...]

- Profiling: `tools/sample_profile.py` samples the exe's stacks (Windows Python, uses the PDB
  of the Release build).

The tools read pyfa's `saveddata.db`; point them at a copy rather than the one pyfa uses.
