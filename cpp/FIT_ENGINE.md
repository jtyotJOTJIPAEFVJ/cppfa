# C++ fit engine (cppfa): how it works

This is a C++ reimplementation of the part of pyfa's fit calculation that tank-oriented fit optimization needs. Its results match pyfa on the user's saved fits, and it has a search mode (`SearchPlan`) that evaluates about 0.7–1M fit variations per second per thread.

It lives in `cpp/` of the pyfa repo, on branch `cppfa`. Pyfa is the ground truth, and all validation is against pyfa's own output.

## 1. Scope

**Goals (what the optimizer can maximize):**

| Goal | Value |
|---|---|
| `ShieldPassive` / `ArmorPassive` / `HullPassive` | EHP of that layer: HP ÷ mean of the layer's four resist multipliers (uniform damage profile) |
| `ShieldActive` / `ArmorActive` | Effective sustainable rep/s of that layer: pyfa's `sustainableTank` with **reload always factored in**, ÷ the same mean resist |
| `ShieldResists` / `ArmorResists` | EHP per HP of that layer: 1 ÷ the mean resist multiplier |
| `ShieldRemote` / `ArmorRemote` | Outgoing remote rep/s (HP/s) of that layer: pyfa's `Module.getRemoteReps` per remote repairer (reload factored in, mutadaptive ones fully spooled), the capacitor left after every other module shared between them most cap-efficient first (`makeRemoteProfile`) |
| `LocalShieldRemoteShield` … `LocalArmorRemoteArmor` | Balanced: the harmonic mean of the local layer's effective rep/s (the goal's remote repairers off) and remote rep/s × `Objective::remoteWeight` (the goal's local repairers off), `balancedParts` / `balancedScore` |

Every evaluation also returns:
- **Fitting:** CPU, powergrid and calibration, used and available.
- **Capacitor:** use (GJ/s) and peak recharge plus injectors (GJ/s). The fit is "cap stable" in the simplified sense when use ≤ recharge. pyfa's own sustainable-tank code uses the same model.

Active goals count only repairers (no passive shield regen). The damage profile is always uniform.

**Sources modeled:**
- **Ship:** all bonuses (role, per-skill, ship-conditional).
- **Character:** all skills at level 5 (`Character` can hold per-skill levels).
- **Implants:** including set bonuses, which work through implants' effects on each other.
- **Boosters:** including side effects that are switched on.
- **Modules:**
  - all states (offline/online/active/overheated)
  - charges and scripts
  - abyssal (mutated) modules
  - T3 cruiser subsystems
  - T3 destroyer modes
- **Command bursts:** the fit's own, all 74 buffs.

**Not modeled:**
- bursts from other fits, and the Titan phenomena generator
- system effects (wormhole, beacons, weather)
- projected effects (webs, neuts, remote reps)
- drones and fighters
- structures/citadels
- the CONCORD pilot-security bonus
- non-uniform damage profiles

## 2. Layout

```
cpp/
  PyfaReader.sln, PyfaReader/PyfaReader.vcxproj   VS 2026 (v145), x64, C++20, warning-free at /W4
  PyfaReader/src/
    GameData.*        in-memory eve.db (float), character, effect tables loader
    Fit.*             saveddata.db fit loader
    Eft.*             EFT text export (matches pyfa exportEft byte for byte)
    AttributeModifier.*  first, simple ship+skill-only engine (superseded by FitEngine)
    FitEngine.*       the engine and SearchPlan
    PyMath.h          pyfa/Python numeric helpers (round, floatUnerr)
    generated/FitSchema.generated.h   generated attribute IDs + per-group layouts
    main.cpp          CLI (EFT export, --engine-dump, --search-check, --bench)
  PyfaReader/data/
    effect_modifiers.tsv   generated from eos/effects.py (copied next to the exe on build)
    warfare_buffs.tsv      generated from eos/saveddata/fit.py (copied next to the exe)
  PyfaReader/third_party/sqlite/   SQLite amalgamation 3.53.4
  tools/
    gen_effect_modifiers.py   effects.py + fit.py -> the two TSV tables
    gen_fit_schema.py         eve.db + TSV -> FitSchema.generated.h
    goal_closure.py           which attributes/modifiers each goal depends on (shared by the schema generator)
    pyfa_reference.py         runs pyfa itself to produce reference results
    compare_engine.py         engine dump vs pyfa reference
```

**Regenerate after updating pyfa (game data or effects), in this order:**
1. `python cpp/tools/gen_effect_modifiers.py`
2. `python cpp/tools/gen_fit_schema.py`
3. build
4. validate (section 6)

The generated files are committed, so building needs no Python.

## 3. Data pipeline

### Game data (`GameData`)
eve.db is loaded into memory once, in about 100 ms. That covers types, groups, categories, market groups, attributes (defaults, min/max caps), effects, type attributes as `float`, and type effects. Helpers:
- **Lookups:** `loadModule(name, attrs)`, `loadShip(name, attrs)`, `loadCharacter(level)`
- **Skills:** `requiredSkills`, `skillID`
- **Groups:** `typeIDsInGroup`, `moduleNamesInGroup`
- **T3D modes:** `shipModes` (pyfa's mode lookup)

### Effect modifiers (`tools/gen_effect_modifiers.py`)
eve.db says *which* effects an item has. *What* an effect does exists only as Python handlers in `eos/effects.py`.

**How it works:** the generator reads that file with `ast` and doesn't import pyfa. It symbolically evaluates each handler:
- **Locals and loops:** constant locals are tracked, and loops over constant tuples, including tuple targets and `range()`, are unrolled.
- **Attribute names:** string formatting is resolved: `%`, `.format`, f-strings, slicing, `.capitalize()`.
- **Undecidable `if`s:** these split the walk. Ship conditions such as "the ship requires skill X" or "the ship has attribute Y" become row conditions; anything else is marked unresolved.
- **Contexts:** pyfa passes the owner kind as `context`, so each handler is evaluated once per context (`ship`, `skill`, `implant`, `booster`, `module`, `moduleCharge`, `drone`, `fighter`, `projected+…`). Identical results are merged.

**Output:** `data/effect_modifiers.tsv`, 3,900 rows from 2,385 effects. Each row is one modifier call:

| Column | Meaning |
|---|---|
| `effectID`, `effectName` | the effect the call belongs to |
| `effectType` | passive/active/overheat/offline/boosterSideEffect/… |
| `runTime` | pyfa pass: early/normal/late |
| `contexts` | owner kinds the row applies to |
| `operation` | boost (%), multiply, increase (pre/post), force, preAssign |
| `domain` | ship, `modules`, `appliedImplants`, `boosters`, self (the owner itself, or for charges the module), extraAttributes (= ship in pyfa) |
| `target` | item or charge |
| `targetAttr` | the attribute modified |
| `sourceAttr` | the attribute the amount comes from… |
| `sourceOf` | …and whose it is: owner / ship / charge / skill:X / buff |
| `factor`, `invert` | amount = factor × value (or factor ÷ value) |
| `perLevel` | amount × the owner skill's level |
| `skill` | amount × this skill's level (pyfa's `skill=` kwarg) |
| `filter` | which items are hit: terms `skill:X`, `ownerSkill`, `group:X`, `hasAttr:X`, optionally on the module's charge; `\|` = any term, `&` = all clauses |
| `condition` | ship or owner conditions from `if`s |
| `stackingPenalized`, `penaltyGroup` | stacking penalties |
| `valueExpr`, `note` | why a row is only partly resolved |

Partly resolved rows are skipped by the engine. Inside the goals' scope only rows for unsupported or runtime-state things remain unresolved: CONCORD security status, booster self-penalties, structure rigs. The RAH is resolved by hand-written code.

**Command bursts:** pyfa applies these in `Fit.__runCommandBoosts`, an `if warfareBuffID == N` chain. The same generator turns that chain into `data/warfare_buffs.tsv` (74 buffs, 176 rows, `sourceOf = buff`).

### Schema (`tools/gen_fit_schema.py`, `goal_closure.py`)
`goal_closure.py` defines each goal's root attributes, plus fitting, capacitor/cycle, burst and RAH roots. It walks the modifier table backwards: any modifier targeting a needed attribute makes its source attribute needed too.

`gen_fit_schema.py` turns that into `FitSchema.generated.h`:
- **`enum class Attr : uint16_t`:** 220 attributes the engine stores, plus eve IDs and names.
- **`ShipAttrs`:** 28 floats of ship attributes that can change per fit. Ship bonus amounts are excluded; they're constant per ship.
- **Group structs:** one float-only struct per module, charge, implant or booster group (246 groups → 103 shapes, ~10 fields on average). A struct contains only the attributes the group's items have, or that relevant modifiers can actually reach on them. Each has `kFields` (`Attr` per field) and `kGroupLayouts` maps groups to layouts.

The engine uses these layouts generically, by field index. The named structs exist for future hand-written code.

## 4. The engine (`fitengine::Engine`)

`Engine(gameData, character)` compiles the TSV rows into `Mod`s. A `Mod` holds enum operation, target and source selectors, a schema `Attr` target, resolved skill/attribute IDs, filters and conditions. Mods are stored per effect and per context. Only rows whose target attribute is in the schema are kept (~5,000).

`FitResult Engine::evaluate(const FitInput&)` (~40 µs per fit on the saved fits, `--engine-dump`) does the following.

**Items:** ship, mode, implants, boosters, modules, and charges as their own items. Each item has its layout's fields in a float arena, in 8 arrays:
- base value
- preAssign
- intermediary (pyfa's direct `itemModifiedAttributes[x] = v` writes)
- pre-increase
- multiplier
- post-increase
- forced value
- computed-value cache

The base value is the type's attribute. For abyssal modules it's the mutator value, else the mutated type's, else the base item's (pyfa's merge). Otherwise it's the dgmattribs default (pyfa never returns "missing").

**Evaluation order (pyfa's):** three passes (early/normal/late). Each pass runs:
1. all skills (passive effects)
2. ship (passive)
3. boosters (passive + enabled side effects)
4. implants (passive)
5. modules in order: the charge's effects (context `moduleCharge`, with the module as owner), then overheat effects if overheated, then the effects the module's state allows (offline effects always, passive if ≥ online, active if ≥ active)
6. T3D mode
7. this pass's command buffs

**Applying a modifier:**
- **Conditions:** checked against the ship type or owner.
- **Amount:** read from the source item, lazily, the way pyfa does. A read computes the current value from that moment's accumulators, so order-dependent behaviour matches.
- **Targets:** selected by domain and filter, then the amount goes into the target field's accumulator.
- **Penalized multipliers:** kept in per-item linked lists, tagged with their penalty group.

**Value of a field** (pyfa `ModifiedAttributeDict`):
- **Untouched fields:** returned as-is, with no caps and no rounding (pyfa's behaviour).
- **Touched fields, as a formula:** `forced` ? forced : `(intermediary|preAssign|base + preInc) × mult × penalties + postInc`, then:
  - min/max caps (other attributes of the same item)
  - CPU/power rounded to 2 decimals
- **Stacking penalties:** per group, bonuses and penalties separately, strongest first, `1 + (m−1)·exp(−i²/7.1289)`.

**Hand-written parts (pyfa code that isn't a modifier):**
- **Reactive Armor Hardener:** resist adaptation loop for the uniform profile. It runs in double internally, with inputs snapped to 6 decimals.
- **Cap booster:** injection amount written as the module's capacitor need.
- **Repairers:** rep effect flags (shield/armor/fueled armor/hull) used by the sustainable-tank calculation.
- **Command bursts:** buff registration (largest magnitude wins per buff ID).
- **Results:**
  - `numShots` (ammo and crystals)
  - average cycle time with reload
  - capacitor use and peak recharge
  - `sustainableTank`: pyfa's algorithm, reps sorted by cap efficiency and filled until capacitor use exceeds peak recharge; computed in double
  - uniform EHP

**Precision:** float everywhere, apart from the few discrete steps noted above. Results match pyfa to ~1e-4 relative. Shots per reload floor with a 1e-5 tolerance, so float noise can't turn 1000 into 999.

## 5. Search (`fitengine::SearchPlan`)

```cpp
SearchSpec spec;
spec.base = fit;                  // ship, mode, implants, boosters, pinned modules (with states/charges)
spec.slots = {{opt1, opt2, ...},  // open slot 0: candidate ModuleInputs (type, state, charge, mutators)
              {...}, ...};
SearchPlan plan(engine, spec);    // ~0.05-0.15 ms
TestResult r = plan.evaluate(choice, Goal::ArmorActive);  // choice[i] = option index or -1 (empty)
FitInput full = plan.fitFor(choice);   // the equivalent full fit
```

`evaluate` is `const` and thread-safe, with per-thread scratch: share one plan between worker threads. Results are identical to `Engine::evaluate(plan.fitFor(choice))`, within float summation order. Chosen options are placed **after** the pinned modules, in slot order. The engine uses module order only for pyfa's evaluation order, not for slot validity.

**How it works:**

1. **Superset.** The plan adds every option of every slot to the fit at once, each tagged (slot, option) or static.

2. **Recording.** It runs the engine's traversal with a *recorder* instead of an executor. The output is pyfa's order as a list of instructions:
   - `Apply`: one modifier from one source onto its resolved targets
   - `Special`: burst registration, cap booster, RAH
   - `Boosts`: end-of-pass buffs

   Each instruction lists the `(item, field)` values its amount reads (including caps) and the ones it writes.

3. **Classification.** This is a fixed point. An instruction is dynamic if:
   - its source is an option, or
   - it reads a field some dynamic instruction writes, or
   - it assigns (force/preAssign) to such a field.

   Everything else is static: skills, ship bonuses, pinned modules, including their effects *onto* options.

4. **Order check.** A dynamic instruction reading a field that a static instruction writes later in pyfa's order would see a different value than pyfa does. `stats().orderSensitive` counts such plans. 12 of ~3,000 test plans were flagged, and none of them produced a mismatch.

5. **Baking.** Static instructions are executed once onto the superset, in order. Then:
   - Modules whose cycle, reload and capacitor inputs, or fitting inputs, aren't written by dynamic instructions get those values precomputed.
   - All touched values of items that no dynamic instruction writes are computed into their cache.

6. **Per test.**
   1. The scratch uses the plan's item numbering. A pointer table points every item at the plan's copy.
   2. Only *written* items that are live are copied: header, field arrays and their penalties. Typically 5 of ~110 items.
   3. Dynamic instructions are replayed in order, skipping those whose source option isn't chosen and targets that aren't live.
   4. Then the goal value, fitting and capacitor numbers are computed.

`Stats` reports:
- instructions recorded and replayed per test
- items written per test
- modules with per-test cycle and fitting values
- order sensitivity

**Performance** (single thread, Release, i.e. the user's machine):
- `--search-check` average (≤4 open slots): **~1.0 µs** per test
- `--bench 1980 15 armorActive`: **1.5 µs** per test (~665k/s). That's a C5 Paladin with 11 open slots × up to 8 options, 152 instructions replayed per test, 5 of 110 items written, 4 of 98 modules with per-test cycle values.
- Full `Engine::evaluate`: ~20 µs (`--search-check` fits), ~40 µs (all saved fits).

Profiles showed the remaining cost spread over:
- the replay loop (amount reads and applying)
- the ship's per-test resist values with stacking penalties
- `cycleOf` for the few modules with changing cycles
- the sustainable-tank step

**What the plan does *not* do (the optimizer's job):**
- **Slot validity:**
  - slot type (high/med/low/rig/subsystem/service) matching
  - slot counts and turret/launcher hardpoints (`ShipAttrs` has the counts, and subsystems change them)
  - `maxGroupFitted` / `maxTypeFitted` / `canFitShipGroup`/`Type` restrictions
  - rig size
  - charge compatibility for options
- **Feasibility:** `cpuUsed ≤ cpuOutput`, `powerUsed ≤ powerOutput`, `calibrationUsed ≤ calibrationOutput`, and cap stability (`capUsed ≤ capRecharge`) are returned but not enforced.
- **Choosing candidates:** filtering by goal relevance, removing dominated options, and similar pruning.

## 6. Validation

**Reference.** `cpp/tools/pyfa_reference.py` runs pyfa itself, through its normal service and fit-loading path, on a temporary copy of saveddata.db. It uses pyfa's Python environment (needs wxPython). The copy is normalized:
- character All 5
- uniform damage profile
- reload factored in
- projected and command fits removed
- implants/boosters removed, unless `--with-implants`

`--keep-db` keeps the normalized copy, so the engine evaluates the same fits. The output has one JSON line per fit:
- ship attributes
- used CPU/PG/calibration, HP/EHP
- cap peak/recharge/use
- sustainable and effective rep/s
- per-module attributes, cycle/reload/shots/cap use

The user's own database is never modified (hash checked).

```
python cpp/tools/pyfa_reference.py ref.jsonl --with-implants --keep-db normalized.db   # pyfa's python
PyfaReader --save normalized.db --engine-dump engine.tsv
python cpp/tools/compare_engine.py ref.jsonl engine.tsv [--show N] [--key PREFIX]
PyfaReader --save normalized.db --search-check     # SearchPlan vs Engine, random choices, all goals
```

**Tolerances:**
- general: 1e-4 relative + 1e-6
- module CPU/PG: 0.01 (pyfa rounds to 2 decimals)
- fit CPU/PG totals: 0.1

**Current results:**
- **Engine vs pyfa:** 3,180 of 3,191 in-scope saved fits match on all ~200 compared values each (643,319 of 643,355 values; outgoing remote repair included on cap-stable fits). That includes 879 fits with implants, 1,458 with boosters, 425 with active command bursts and 58 with abyssal (mutated) modules, whose 773 module values all match. The differences:
  - 9 fits with a Reactive Armor Hardener in an exact tie, which pyfa breaks by double-precision noise (≤0.8% armor EHP)
  - 1 Enforcer (CONCORD security-status bonus, not modeled)
  - 1 Moa: a 2e-5 relative difference on a small rep value
  - 2 citadels excluded (out of scope)
- **SearchPlan vs Engine:** 305,900 random combinations over 3,059 fits (up to 4 open slots × 4 options, including empty slots), all five goals: 0 mismatches.
- **EFT export:** byte-identical to pyfa on all saved fits.

Earlier, while the engine still used double, it matched pyfa exactly on every in-scope fit except the CONCORD one. The float switch introduced the RAH tie differences.

## 7. CLI (`PyfaReader.exe`)

| Command | |
|---|---|
| `PyfaReader [<fit name> \| #<fitID>]` | EFT export of a saved fit (no argument: list recent fits) |
| `--engine-dump out.tsv` | evaluate all saved fits, `fitID<TAB>key<TAB>value` |
| `--search-check [maxFits]` | SearchPlan vs Engine on random choices |
| `--bench [fitID [seconds [goal]]]` | profiling case: the fit's mid and low slots become open slots, each with the fitted module + up to 7 same-group alternatives, enumerated in order on one thread |
| `--game eve.db`, `--save saveddata.db` | override the database paths (default: eve.db above the exe, `%USERPROFILE%\.pyfa\saveddata.db`) |

To profile in Visual Studio: Release|x64, Debugging → Command Arguments `--bench 1980 15 armorActive`, then Debug → Performance Profiler → CPU Usage.

## 8. Notes for the optimizer

- **Parallelism:** build one `SearchPlan` per (ship, pinned modules, implants, boosters, candidate sets) and evaluate combinations from multiple threads; `evaluate` is thread-safe.
- **Cost drivers:** cost per test grows with the number of instructions replayed per test, i.e. options whose effects interact with other parts of the fit. Options that don't affect the goal still cost their own instructions. Filtering candidates by goal relevance before building the plan pays off; `goal_closure.py`'s backward pass identifies which groups and attributes a goal depends on.
- **Rebuilding:** plan build is ~0.05–0.15 ms (refine's plans, ~220 items: 0.14 ms), so rebuilding plans with narrower candidate sets during a search, e.g. per branch, is affordable.
- **Pinned modules:** they keep their state. A pinned remote rep set to active consumes capacitor in every test, which is the intended way to reserve capacitor.
- **Choice of goal:** `Goal` only selects which value is returned. The cost of a test is nearly the same for all goals: active goals add the sustainable-tank step, and fitting and capacitor are always computed.
