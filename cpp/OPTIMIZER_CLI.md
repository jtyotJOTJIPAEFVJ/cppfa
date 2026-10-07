# Optimizer CLI: request / response format (version 1)

pyfa runs the C++ optimizer as a separate process. It writes a JSON request describing the fit, including module states, and the slots to optimize. It gets back a JSON response with several options for those slots. Everything that isn't requested stays as it is (pinned), with its state, charge and mutations.

JSON is read and written with nlohmann/json 3.11.3 (single header, MIT license, `cpp/PyfaReader/third_party/nlohmann/json.hpp`, included through `src/Json.h`). The code is in `src/Request.h/.cpp`.

## Command line

```
PyfaReader.exe [--game <eve.db>] [--save <saveddata.db>] --optimize-request <request.json | -> [--out <response.json | ->]
               [--threads N] [--goal G] [--check] [--slots m,l,r] [--verbose]
PyfaReader.exe [--game ...] [--save ...] --make-request <fitID> [--open all | "mid:0,1;low:2;rig:*"] [--goal G] [--out <request.json>]
```

- `-` reads the request from stdin. Without `--out`, or with `--out -`, the response goes to stdout. stdout then carries only the JSON.
- `--game`: if it is omitted, eve.db is searched for next to the exe and then in the working directory, as for every other mode. `effect_modifiers.tsv` must be next to the exe; the build copies it there.
- `--save` is optional for requests. It is used only for pyfa's `prices` table, and only when the request has no `prices`. If it is omitted, the default pyfa save path is tried, and no prices are used if it isn't found.
- `--threads N` overrides the request's `threads`. pyfa may want to leave a core free.
- `--goal`, `--check` and `--slots` are for testing. `--goal` overrides the request's goal. `--check` runs the brute-force validation (`Options::bruteForce`) on the request's open slots instead of stage 2: the response then has `check` and an empty `options`. `--slots m,l,r` caps the open slots per rack (`Options::maxSlots`). `--verbose` writes the optimizer log to stderr.
- **Exit code**: 0 = ok. 1 = `--check` found a mismatch. 2 = error; a response with `"status": "error"` is still written.
- **Progress** (stderr): lines of the form `progress <fraction> <phase>`, where fraction is 0.000-1.000. Phases: `setup`, `candidates`, `measuring candidates`, `interactions`, `model`, `stage 1` (0.25), `stage 2` (0.70, then steps per batch of starting sets), `done` (1.000). Stage 1 reports no steps of its own, so the bar sits at 0.25 until stage 1 ends (0.1-2 s on the test fits with 16 threads). Every other stderr line is free-form log text; ignore it. To cancel, kill the process.
- `--make-request` writes a request for a saved fit, mainly for tests. `--open all` (the default) requests every mid, low and rig slot. The racks are padded with empty entries up to their slot counts, so the result equals `--optimize <fitID>`. `--open "mid:0,2;low:1"` uses indices within each rack, counting empty slots and following position order; `rig:*` requests a whole rack.

## Request

```json
{
  "version": 1,
  "ship": 11993,
  "mode": 0,
  "character": {"defaultLevel": 5, "skills": [{"skillID": 3416, "level": 4}]},
  "implants": [{"typeID": 13219, "active": true}],
  "boosters": [{"typeID": 28674, "active": true, "sideEffects": [5869]}],
  "modules": [
    {"position": 0, "slot": "low", "typeID": 2048, "chargeID": 0, "state": "online", "optimize": false},
    {"position": 1, "slot": "low", "typeID": 0, "optimize": true},
    {"position": 4, "slot": "med", "typeID": 3841, "chargeID": 0, "state": "active", "optimize": true},
    {"position": 5, "slot": "med", "typeID": 47702, "state": "active", "baseTypeID": 10858, "mutaplasmidID": 47701,
     "mutators": {"68": 192.5, "73": 3.6}, "optimize": false},
    {"position": 8, "slot": "high", "typeID": 2873, "chargeID": 21894, "state": "active", "optimize": false}
  ],
  "goal": "shieldActive",
  "priceCap": 0,
  "allowOfficer": false,
  "results": 10,
  "threads": 0,
  "prices": {"2048": 1250000.0, "3841": 3400000.0}
}
```

| Field | Type | Required | Meaning |
|---|---|---|---|
| `version` | int | no (default 1) | Must be 1. |
| `ship` | int | yes | Ship typeID (`fit.ship.item.ID`). |
| `mode` | int | no | T3 destroyer mode typeID (`fit.mode.item.ID`). As in pyfa, a missing or invalid mode on a T3D becomes the ship's first mode. Ignored for other ships. |
| `character` | object | no | Omitted: all skills at V. `defaultLevel` (0-5, default 5) is the level of every skill not listed in `skills`. `skills` is a list of `{skillID, level}` overrides. For a real pyfa character, send `defaultLevel: 0` and every skill: `[{"skillID": s.itemID, "level": s.level} for s in fit.character.skills]`. |
| `implants` | list | no | `{typeID, active}`. The implants that apply to the fit: `fit.appliedImplants`, which pyfa takes from the character or the fit according to `implantLocation`. |
| `boosters` | list | no | `{typeID, active, sideEffects}`. `sideEffects` lists the effectIDs of the side effects that are switched on: `[e.ID for e in booster.activeSideEffectEffects]`. |
| `modules` | list | no | One entry per entry of `fit.modules`, empty slots included. See below. |
| `goal` | string | yes | `shieldPassive`, `armorPassive`, `hullPassive` (EHP, uniform damage profile), or `shieldActive`, `armorActive` (effective sustainable repair, EHP/s). |
| `priceCap` | number | no (0) | ISK per module (with its charge excluded). 0 = no cap. With a cap, modules without a known price are skipped. |
| `allowOfficer` | bool | no (false) | Allow officer modules (metaGroup 5). |
| `results` | int | no (10) | How many distinct options to return at most. |
| `threads` | int | no (0) | Worker threads. 0 = all hardware threads. |
| `prices` | object | no | `{"<typeID>": ISK}`. If it is given, saveddata's prices table isn't read. JSON keys are strings; Python's `json.dumps` writes int dict keys that way. |

Module entries:

| Field | Type | Meaning |
|---|---|---|
| `position` | int | `Module.position`: the index in `fit.modules`, which spans all racks, with empty (dummy) modules taking positions too. It must be unique. Only the order matters to the engine: modules are evaluated in position order, as pyfa does. The response echoes the positions. |
| `slot` | string or int | `Module.slot` (`FittingSlot`): `"low"`, `"med"` (`"mid"` is accepted), `"high"`, `"rig"`, `"subsystem"`, `"service"`, or the enum value (1, 2, 3, 4, 5, 8). It is required for empty slots. For modules it is optional and must match the type's slot. |
| `typeID` | int | `Module.itemID`. 0 or missing = an empty slot (`Module.isEmpty`). For a mutated module, this is the mutated (abyssal) type. |
| `chargeID` | int | `Module.chargeID`. 0 or missing = no charge. |
| `state` | string or int | `Module.state` (`FittingModuleState`): `"offline"`, `"online"`, `"active"`, `"overheated"`, or -1, 0, 1, 2. Default `"online"`. |
| `baseTypeID` | int | Mutated modules: `Module.baseItemID`. |
| `mutaplasmidID` | int | Mutated modules: `Module.mutaplasmidID`. Informational: the engine uses `baseTypeID` and `mutators`. |
| `mutators` | object | `{"<attributeID>": value}`: `{str(m.attrID): m.value for m in mod.mutators.values()}`. |
| `optimize` | bool | true = this slot is open: the optimizer may put any module there, or leave it empty. |

Rules:
- Only med, low and rig slots can be optimized. A requested high, subsystem or service slot gets a warning and is kept as it is.
- Per rack, the open slot count is the number of requested entries in that rack. It is capped by the free slots: the rack size (the ship's `medSlots`/`lowSlots`/`rigSlots`, as modified by the pinned fit, e.g. by subsystems) minus the rack's pinned modules. If the cap applies, there is a warning, and the extra requested positions come back empty.
- Pinned modules keep their state, charge and mutations, and they count toward group limits (`maxGroupFitted`, `maxTypeFitted`), CPU, power and calibration.
- **Module rule, assault damage controls**: any type with effect 7012 `moduleBonusAssaultDamageControl`. Its active bonus lasts about 12 s, so it doesn't count toward tank. These modules are never proposed, because a regular damage control is always better. A pinned one is evaluated at most online, which keeps its passive `damageControl` part. The response lists such modules in `clampedModules` and adds a warning. pyfa should tell the user, and may set the module online.
- Drones, fighters, projected effects, fleet boosts from other fits, and damage profiles are not part of the request; the engine doesn't model them. The goal uses the uniform damage profile.

## Response

```json
{
  "version": 1,
  "status": "ok",
  "message": "",
  "goal": "shieldActive",
  "openSlots": {"med": 2, "low": 1, "rig": 0},
  "start": {"goal": 187.27, "cpuUsed": 120.0, "cpuOutput": 258.875, "powerUsed": 40.1, "powerOutput": 56.25,
            "calibrationUsed": 0.0, "calibrationOutput": 400.0, "capUsed": 5.2, "capRecharge": 9.8,
            "capStable": true, "fits": true, "price": 4650000.0},
  "clampedModules": [],
  "options": [
    {
      "rank": 1,
      "goal": 429.91406,
      "gain": 242.64,
      "fits": true,
      "price": 31000000.0,
      "priceDelta": 26350000.0,
      "changedSlots": 2,
      "stats": {"goal": 429.91406, "cpuUsed": 250.0, "cpuOutput": 258.875, "powerUsed": 50.3, "powerOutput": 56.25,
                "calibrationUsed": 0.0, "calibrationOutput": 400.0, "capUsed": 9.1, "capRecharge": 9.8,
                "capStable": true, "fits": true},
      "slots": [
        {"position": 1, "slot": "low", "typeID": 0, "chargeID": 0, "state": null, "name": null, "chargeName": null,
         "unchanged": true},
        {"position": 4, "slot": "med", "typeID": 3841, "chargeID": 0, "state": "active",
         "name": "Large Shield Extender II", "chargeName": null, "unchanged": true},
        {"position": 6, "slot": "med", "typeID": 10858, "chargeID": 0, "state": "active",
         "name": "Medium Shield Booster II", "chargeName": null, "unchanged": false}
      ]
    }
  ],
  "warnings": [],
  "stats": {"seconds": 0.21, "candidates": 369, "kinds": 46, "engineCalls": 13696, "stage2Starts": 66}
}
```

The numbers above are illustrative.

| Field | Meaning |
|---|---|
| `status`, `message` | `"ok"` with an empty message, or `"error"` with the reason. An error response has only `version`, `status` and `message`. |
| `goal` | The goal that was used (after `--goal`). |
| `openSlots` | The open slots per rack that were searched. |
| `start` | The input fit, as requested, with the module rules applied. `price` is the price of the modules and charges currently in the requested slots. |
| `clampedModules` | Pinned modules evaluated with a lower state than requested: `{position, slot, typeID, name, state (as requested), evaluatedState}`. |
| `options` | Best first. Fitting options come first, ordered by goal; then, at an equal goal, cheaper ones; non-fitting options last. There are at most `results`, and they are pairwise distinct as module multisets. The list can be empty, e.g. when nothing reaches a goal above 0. |
| `options[].goal`, `gain` | Goal value (EHP, or EHP/s for active goals) and gain over `start.goal`. |
| `options[].fits` | CPU, power and calibration within their outputs. |
| `options[].price`, `priceDelta` | ISK of the modules and charges in the requested slots, and the difference from `start.price`. Unknown prices count as 0. |
| `options[].changedSlots` | Requested slots whose module, charge or state differs from the request. |
| `options[].stats` | Engine numbers for the whole fit with this option: `cpuUsed/cpuOutput` (tf), `powerUsed/powerOutput` (MW), `calibrationUsed/calibrationOutput`, `capUsed` (GJ/s used by active modules), `capRecharge` (GJ/s: peak recharge at 25 % plus cap boosters), and `capStable` (capUsed <= capRecharge; simplified). |
| `options[].slots` | One entry per requested slot (med/low/rig), in position order: `position`, `slot`, `typeID` (0 = leave empty), `chargeID`, `state` (null if empty), `name`, `chargeName`, `unchanged`. |
| `warnings` | Strings to show the user (slots that can't be opened, the free-slot cap, clamped modules, optimizer warnings). |
| `stats` | Run statistics, informational. |
| `check` | Only with `--check`: `{ran, combinations, feasible, compared, mismatches, fittingGoalChanges, bruteBest, stage1Best, maxError}`. |

How options map to positions, deterministically, per rack:
1. A found module with the same type and charge as a requested slot's current module stays in that slot. A second pass matches on the type only, so the module stays and its charge changes. Mutated current modules are never matched.
2. The remaining found modules, sorted by (typeID, chargeID), fill the remaining requested positions in ascending order.
3. Requested positions that are left over are empty (`typeID` 0).

States: activatable modules (those with an active effect) are `"active"`, all others `"online"`. That is what the optimizer evaluated. The stats are for these states, so pyfa should set them when it applies an option.

Consistency: every option's fit is put together again in pyfa's module order and evaluated with the full engine. If its goal differs by more than 1e-4 relative, a warning is added. None has so far.

## Applying an option in pyfa (Phase C notes)

For each `slots[]` entry: `typeID` 0 → `fit.modules.free(position)`. Otherwise build `Module(item)`, set its charge and state, and replace it at `position` (`fit.modules.replace(position, mod)`; the position is the list index). Then recalculate. pyfa's own numbers can differ from the response by about 1e-4 relative (float engine).

## In pyfa

pyfa uses this through **Optimize Modules...** in the fitting view's context menu (select mid/low/rig slots, empty ones too). The client is `service/moduleOptimizer.py` (request, runner, parser) and `gui/moduleOptimizerDialog.py`; see `OPTIMIZER_STATUS.md`, Phase C. pyfa runs `PyfaReader.exe --game <eve.db> --optimize-request - --out -` and always sends `prices` (from its price cache), so `--save` isn't needed. The exe path is a pyfa setting (default `<pyfa root>/cpp/bin/Release/PyfaReader.exe`, changed with the window's "Optimizer path..." button).
