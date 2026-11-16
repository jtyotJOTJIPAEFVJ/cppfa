# Optimizer CLI: request / response format (version 1)

pyfa runs the C++ optimizer as a separate process. It writes a JSON request describing the fit, including module states, and the slots to optimize. It gets back a JSON response with several options for those slots. Everything that isn't requested stays as it is (pinned), with its state, charge and mutations.

JSON is read and written with nlohmann/json 3.11.3 (single header, MIT license, `cpp/PyfaReader/third_party/nlohmann/json.hpp`, included through `src/Json.h`). The code is in `src/Request.h/.cpp`.

## Command line

```
PyfaReader.exe [--game <eve.db>] [--save <saveddata.db>] --optimize-request <request.json | -> [--out <response.json | ->]
               [--threads N] [--goal G] [--check] [--slots m,l,r] [--verbose]
               [--time-budget S] [--members] [--no-refine] [--refine-rounds N] [--refine-patience N]
               [--refine-pairs N] [--refine-chains N] [--refine-starts N] [--refine-exact N]
PyfaReader.exe [--game ...] [--save ...] --make-request <fitID> [--open all | "mid:0,1;low:2;rig:*;high:*"] [--goal G] [--out <request.json>]
```

Test commands on saved fits (whole racks, no request; same constraint, refine and `--threads` flags):

```
PyfaReader.exe --optimize <fitID> <goal> [--racks mid,low,rig,high] [--price-cap ISK] [--allow-officer] [--top K]
               [--results N] [--dump-kinds] [--slots m,l,r[,h]] [--no-ancillary] [--no-cap-boosters] [--no-polarized] [--max-reps N]
               [--cap-stable] [--min-cap-recharge GJ/s] [--remote-weight W]
               [--target frigate|cruiser|battleship|em,th,ki,ex,sig,speed,radius,distance[,speedFactor]]
PyfaReader.exe --optimize-check <fitID>[,<fitID>...] [goal|all] [--racks ...] [--slots m,l,r[,h]] [--verbose]
```

`--optimize` prints the result as text. `--optimize-check` runs stage 1 against the brute force (`OPTIMIZER.md`, section 3) on small slot sets (default 2 mid / 2 low / 1 rig / 2 high open), one line per (fit, goal), exit code 1 on any mismatch.

- `-` reads the request from stdin. Without `--out`, or with `--out -`, the response goes to stdout. stdout then carries only the JSON.
- `--game`: if it is omitted, eve.db is searched for next to the exe and then in the working directory, as for every other mode. `effect_modifiers.tsv` must be next to the exe; the build copies it there.
- `--save` is optional for requests. It is used only for pyfa's `prices` table, and only when the request has no `prices`. If it is omitted, the default pyfa save path is tried, and no prices are used if it isn't found.
- `--threads N` overrides the request's `threads`. pyfa may want to leave a core free.
- `--time-budget S` overrides the request's `timeBudget`.
- `--goal`, `--check` and `--slots` are for testing. `--goal` overrides the request's goal. `--check` runs the brute-force validation (`Options::bruteForce`) on the request's open slots instead of stage 2: the response then has `check` and an empty `options`. `--slots m,l,r` caps the open slots per rack (`Options::maxSlots`). `--verbose` writes the optimizer log to stderr.
- Refine tests (see `OPTIMIZER.md`, refine): `--members` also runs the member-level brute force after the search (every combination of every module choice of the open slots, exact; small requests only) and adds `members` to the response. `--no-refine` skips refine (stage 2's result, the pipeline before refine). `--refine-rounds N` caps the iterated-search rounds: with it (or when refine converges first) the result doesn't depend on timing. `--refine-patience N` (chain iterations without a better best set, default 800), `--refine-pairs N` (pair moves verified per local-optimum test, default 256, 0 = single moves only), `--refine-chains N` (default 16; 0 = one per thread), `--refine-starts N` (stage 2 end sets descended first; default 0 = max(chains, 8)), `--refine-exact N` (refine's exact pass runs when the open slots have at most N combinations of its choices; default 3e7, 0 = off). The same flags work with `--optimize`. Debugging environment switches are listed in `OPTIMIZER.md`, section 3.
- **Exit code**: 0 = ok, also for `"status": "infeasible"` (a valid answer). 1 = `--check` found a mismatch. 2 = error; a response with `"status": "error"` is still written.
- **Progress** (stderr): lines of the form `progress <fraction> <phase>`, where fraction is 0.000-1.000. Phases: `setup`, `candidates`, `measuring candidates`, `interactions`, `model`, `stage 1` (0.25), `stage 2` (0.70, then steps per batch of starting sets, up to 0.80), `refine` (0.80), then once per refine round `refine: round N, best G` (0.80-1.00: the larger of the elapsed share of the time budget and the share of the convergence patience used), `done` (1.000). Stage 1 reports no steps of its own, so the bar sits at 0.25 until stage 1 ends (0.1-2 s on the test fits with 16 threads). Every other stderr line is free-form log text; ignore it. To cancel, kill the process.
- `--make-request` writes a request for a saved fit, mainly for tests. `--open all` (the default) requests every mid, low and rig slot. The racks are padded with empty entries up to their slot counts, so the result equals `--optimize <fitID>`. `--open "mid:0,2;low:1"` uses indices within each rack, counting empty slots and following position order; `rig:*` requests a whole rack. High slots are only opened when named (`high:*`).

## Request

```json
{
  "version": 1,
  "ship": 11993,
  "mode": 0,
  "character": {"defaultLevel": 5, "skills": [{"skillID": 3416, "level": 4}]},
  "implants": [{"typeID": 13219, "active": true}],
  "boosters": [{"typeID": 28674, "active": true, "sideEffects": [5869]}],
  "drones": [{"typeID": 2488, "amount": 5}],
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
  "timeBudget": 60,
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
| `factorReload` | bool | no (true) | pyfa's `Fit.factorReload` (the global "factor in reload time" option): reload time in the modules' average cycle, so in capacitor use, sustained local and remote repair and damage. Capacitor boosters always count it, as in pyfa. |
| `spoolup` | number | no (1) | pyfa's default spool-up (`globalDefaultSpoolupPercentage`, 0..1 of the maximum bonus): mutadaptive remote repairers and entropic disintegrators. A module's own setting (`spoolType`) wins. |
| `drones` | list | no | `{typeID, amount, baseTypeID, mutators}`: the drones in space, `amount` = `Drone.amountActive` (entries with 0 are ignored); mutated drones as modules. Only the `appliedDps` goal uses them. |
| `modules` | list | no | One entry per entry of `fit.modules`, empty slots included. See below. |
| `goal` | string | yes | `shieldPassive`, `armorPassive`, `hullPassive` (EHP, uniform damage profile), `shieldActive`, `armorActive` (effective sustainable repair, EHP/s), `shieldResists`, `armorResists` (resists only: EHP per HP of the layer, 1 / mean resonance for the uniform damage profile; HP and repair modules are worth nothing), or `shieldRemote`, `armorRemote` (outgoing remote repair, HP/s: pyfa's `Module.getRemoteReps` per remote repairer, reload and spool-up as `factorReload` / `spoolup`, the capacitor left after every other module shared between them most cap-efficient first, like the local sustainable tank; open high slots for them), or `localShieldRemoteShield`, `localShieldRemoteArmor`, `localArmorRemoteShield`, `localArmorRemoteArmor` (balanced local tank and remote repair, see below), or `appliedDps` (damage per second applied to `target`, see below). |
| `target` | object | for `appliedDps` | `{em, thermal, kinetic, explosive, signatureRadius, maxVelocity, radius, distance, speedFactor}`: the target's resists (0..1), signature radius (m; missing, null or <= 0 = none, i.e. infinite, as pyfa's profiles without one), max velocity (m/s), radius (m), the distance (m, surface to surface) and the share of its velocity it moves at (0..1, default 1). Tackle: `maxVelocityScrammed`, `signatureRadiusScrammed` (the target with its MWD / MJD off, used when the fit's scrambler reaches it; default unchanged), `webResistance`, `painterResistance` (1 = none, 0 = immune; default 1), `immune` (bool, pyfa `disallowOffensiveModifiers`). pyfa sends the fit's enabled Target tab entry (`service.target.getActiveTarget`). |
| `priceCap` | number | no (0) | ISK per module (with its charge excluded). 0 = no cap. With a cap, modules without a known price are skipped. |
| `allowOfficer` | bool | no (false) | Allow officer modules (metaGroup 5). |
| `allowAncillary` | bool | no (true) | false: no ancillary shield boosters or armor repairers (local or remote) are proposed. Pinned ones stay. |
| `excludePolarized` | bool | no (false) | `appliedDps`: no polarized weapons (types with `resistanceKiller`) are proposed. Pinned ones stay. |
| `allowCapBoosters` | bool | no (true) | false: no capacitor boosters are proposed (e.g. for a tank that is stable without charges). Pinned ones stay. |
| `maxReps` | int | no (0) | Active and remote goals: the most repairers of the goal in the fit, pinned active ones included (local ones of the goal's layer, or remote ones of it). 0 = no limit. |
| `requireCapStable` | bool | no (false) | Only options with capacitor use within recharge (`stats.capStable`). |
| `remoteWeight` | number | no (4) | Balanced goals: the EHP/s one remote HP/s is worth, i.e. the rep target's resist multiplier (4 for 75 % average resists). |
| `minCapRecharge` | number | no (0) | GJ/s: only options whose `stats.capRecharge` (peak recharge + cap boosters) is at least this. 0 = none. |
| `results` | int | no (10) | How many distinct options to return at most. |
| `threads` | int | no (0) | Worker threads. 0 = all hardware threads. |
| `timeBudget` | number | no (60) | Seconds for the whole run. Refine, the last phase, stops when the budget is used up, or earlier when its best set stops improving (simple requests finish in well under a second). 0 = no time limit (refine runs until it converges). Stage 1 (the model search) stops at half the budget if it hasn't finished; stages 2 and refine then go on from the sets it found, and a warning says the result is the best found, not proven (`stats.stage1Stopped`). pyfa's run timeout should be above it. |
| `prices` | object | no | `{"<typeID>": ISK}`. If it is given, saveddata's prices table isn't read. JSON keys are strings; Python's `json.dumps` writes int dict keys that way. |

Balanced goals score a fit on two tests from one evaluation. **Local** is the effective sustainable local repair of the local layer (EHP/s, as `armorActive`/`shieldActive`) with the goal's remote repairers off. **Remote** is the remote repair of the remote layer (HP/s, as `armorRemote`/`shieldRemote`) with the goal's local repairers of the local layer off. So each side gets the capacitor on its own, and neither pays for the other's cap use. The score is the harmonic mean of local and `remoteWeight` × remote: 2ab / (a + b). It grows with either side but approaches twice the smaller one when they differ a lot, so piling onto one side gains little. With equal sides it is their value. `stats.localRep` and `stats.remoteRep` give the two sides. `stats.capStable` and `capUsed` still describe everything running at once. `maxReps` counts the local and the remote repairers together.

`appliedDps` is pyfa's damage graph at one point: the attacker stands still, the target moves at `speedFactor × maxVelocity` straight across (angle 90°) at `distance`. Each active weapon's damage per second (reload and spool-up as `factorReload` / `spoolup` say) times the target's resists and its application: turrets the chance to hit (range and tracking, wrecking shots included), launchers the missile range (flight time, both whole-second ranges) and explosion (radius, velocity), vorton projectors their range and explosion, smartbombs their range; nothing beyond the fit's lock range (smartbombs and FoF missiles excepted). Drones in space count too, within lock and drone control range (they keep up with the target when faster, else they shoot from the ship). First the fit's webs, grapplers, target painters and scramblers (and web / painter drones) act on the target as in pyfa's graph with "apply projected": slower, bigger, a scrambled MWD switched off (`FIT_ENGINE.md`, "Tackle"). Bombs, doomsdays and defender missiles count no applied damage, breacher pods none at all. There is no stage-1 model for this goal: refine searches alone (`OPTIMIZER.md`), from the open slots' own modules and from the empty set.

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
| `spoolType`, `spoolAmount` | int, number | The module's own spool-up setting (`Module.spoolType`, `eos.const.SpoolType` 0-3, and `spoolAmount`); missing = `spoolup`. Opened slots' new modules use `spoolup`. |
| `optimize` | bool | true = this slot is open: the optimizer may put any module there, or leave it empty. |

Rules:
- Only high, med, low and rig slots can be optimized. A requested subsystem or service slot gets a warning and is kept as it is. High slots get no weapons for the tank goals; they are for remote repairers. For `appliedDps` they get weapons (turrets, launchers, vorton projectors, smartbombs), each with every charge it can load, within the ship's free turret and launcher hardpoints (`turretSlotsLeft`, `launcherSlotsLeft` less the pinned weapons).
- `appliedDps`: modules that take scripts (tracking and guidance computers, sensor boosters, HIC disruptors) are offered without a script and with each script. Webs, grapplers, target painters and scramblers are candidates too (fitted active).
- An option that misses `requireCapStable` or `minCapRecharge` doesn't count as fitting, so it isn't reported. When none meets them (and the pinned part fits), the status is `infeasible` with reason `capacitor` and a message naming the closest set's capacitor numbers.
- Per rack, the open slot count is the number of requested entries in that rack. It is capped by the free slots: the rack size (the ship's `medSlots`/`lowSlots`/`rigSlots`, as modified by the pinned fit, e.g. by subsystems) minus the rack's pinned modules. If the cap applies, there is a warning, and the extra requested positions come back empty.
- Pinned modules keep their state, charge and mutations, and they count toward group limits (`maxGroupFitted`, `maxTypeFitted`), CPU, power and calibration.
- **Abyssal (mutated) modules** are read-only: the engine evaluates them with their mutations (the mutator values over the base item's attributes, as pyfa does), but the optimizer never proposes new ones (abyssal types aren't published). A mutated module in an opened slot is offered back as it is: a candidate of its own, at no price (it is owned, so the price cap doesn't apply), at most as many times as the opened slots hold it. A found mutated module stays in its own slot when it is kept; in the response, its slot carries `baseTypeID` and `mutators` so the caller can put back the original module (mutaplasmid included).
- **Module rule, assault damage controls**: any type with effect 7012 `moduleBonusAssaultDamageControl`. Its active bonus lasts about 12 s, so it doesn't count toward tank. These modules are never proposed, because a regular damage control is always better. A pinned one is evaluated at most online, which keeps its passive `damageControl` part. The response lists such modules in `clampedModules` and adds a warning. pyfa should tell the user, and may set the module online.
- Fighters, projected effects onto the fit (its Projected tab), fleet boosts from other fits (command fits), and damage profiles are not part of the request; the engine doesn't model them (pyfa's dialog warns when the fit has any). The tank goals use the uniform damage profile. Drones are pinned: the optimizer doesn't choose them.

## Response

```json
{
  "version": 1,
  "status": "ok",
  "message": "",
  "goal": "shieldActive",
  "openSlots": {"med": 2, "low": 1, "rig": 0, "high": 0},
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
| `status`, `message` | `"ok"` with an empty message; `"infeasible"` when no choice for the requested slots can fit, with the reason in `message` (also in `infeasible.message`) and empty `options`, see below; or `"error"` with the reason. An error response has only `version`, `status` and `message`. |
| `infeasible` | Only with `"status": "infeasible"`: why nothing fits, see below. |
| `goal` | The goal that was used (after `--goal`). |
| `openSlots` | The open slots per rack that were searched. |
| `start` | The input fit, as requested, with the module rules applied. `price` is the price of the modules and charges currently in the requested slots. |
| `clampedModules` | Pinned modules evaluated with a lower state than requested: `{position, slot, typeID, name, state (as requested), evaluatedState}`. |
| `options` | Best first: by goal, then, at an equal goal (within ~1e-7 relative), cheaper. Only options that fit (CPU, power and calibration) are returned; `fits` is always true. There are at most `results`, and they are pairwise distinct as module multisets. The list is empty with `"status": "infeasible"`, and with `"ok"` when no choice that fits reaches a goal above 0 (then `warnings` says so, with the free CPU / power / calibration). |
| `options[].goal`, `gain` | Goal value (EHP, EHP/s for active goals, damage per second for `appliedDps`) and gain over `start.goal`. |
| `options[].fits` | CPU, power and calibration within their outputs. |
| `options[].price`, `priceDelta` | ISK of the modules and charges in the requested slots, and the difference from `start.price`. Unknown prices count as 0. |
| `options[].changedSlots` | Requested slots whose module, charge or state differs from the request. |
| `options[].stats` | Engine numbers for the whole fit with this option: `cpuUsed/cpuOutput` (tf), `powerUsed/powerOutput` (MW), `calibrationUsed/calibrationOutput`, `capUsed` (GJ/s used by active modules), `capRecharge` (GJ/s: peak recharge at 25 % plus cap boosters), and `capStable` (capUsed <= capRecharge; simplified). | `appliedDps`: also `dps`, the raw damage per second (no resists, no application).
| `options[].slots` | One entry per requested slot (high/med/low/rig), in position order: `position`, `slot`, `typeID` (0 = leave empty), `chargeID`, `state` (null if empty), `name`, `chargeName`, `unchanged`. |
| `warnings` | Strings to show the user (slots that can't be opened, the free-slot cap, clamped modules, optimizer warnings). |
| `stats` | Run statistics, informational. |
| `stats` (refine) | `stage2Best` (best goal before refine), `secondsStage1`, `secondsStage2`, `secondsRefine`, `refineStarts`, `refineRounds`, `refineEvaluations`, `refineImproved` (times refine found a better best set), `refineExact` (combinations refine's exact pass evaluated; 0 when it didn't run), `refineStop` (`converged`, `time`, `rounds`, `off`, `no start`), `stage1Stopped` (stage 1 hit half the time budget), `modelSkipped` (only when a stage-1 model size limit was hit: stages 1 and 2 were skipped and refine searched alone). Informational. |
| `members` | Only with `--members`: `{ran, skipped, combinations, fitting, best, bestPrice, bestModules, seconds, found, foundPrice}`: the exhaustive best over every module choice vs the optimizer's first option (`found`). Prices here count modules and charges as the optimizer compares them. |
| `check` | Only with `--check`: `{ran, combinations, feasible, compared, mismatches, fittingGoalChanges, bruteBest, stage1Best, maxError}`. |

### Infeasible requests

An open slot may stay empty, so a request is infeasible only when the pinned part (every module outside the selection, the selected slots empty) is over on CPU, power or calibration and the selected slots can't fix it. The user chose the selection, so the optimizer never touches other slots: it reports why instead.

```json
{
  "version": 1,
  "status": "infeasible",
  "message": "The rigs outside the selection use 450.0 calibration of 400.0 (50.0 over). No module raises calibration or lowers a rig's calibration cost, so no choice for the selected slots can fix this. To fix it, select one of these rigs (to replace it) or remove it: 3x Medium Command Processor I (150.0 each).",
  "goal": "armorActive",
  "infeasible": {
    "reason": "calibration",
    "message": "(same as the top-level message)",
    "resources": [
      {"resource": "calibration", "pinnedUsed": 450.0, "output": 400.0, "bestPinnedUsed": 450.0, "bestOutput": 400.0,
       "shortfall": 50.0, "message": "The rigs outside the selection use 450.0 calibration of 400.0 ..."}
    ],
    "modules": [
      {"position": 16, "slot": "rig", "typeID": 43896, "name": "Medium Command Processor I", "cost": 150.0},
      {"position": 17, "slot": "rig", "typeID": 43896, "name": "Medium Command Processor I", "cost": 150.0},
      {"position": 18, "slot": "rig", "typeID": 43896, "name": "Medium Command Processor I", "cost": 150.0}
    ]
  },
  "openSlots": {"med": 1, "low": 1, "rig": 0, "high": 0},
  "start": {"...": "as usual"},
  "clampedModules": [],
  "options": [],
  "warnings": [],
  "stats": {"seconds": 0.005, "candidates": 0, "kinds": 0, "engineCalls": 0, "stage2Starts": 0}
}
```

| Field | Meaning |
|---|---|
| `reason` | `"calibration"`: the pinned rigs alone are over. Nothing raises calibration output or lowers a rig's calibration cost, so this is decided before any search (a few ms). `"outputs"`: CPU or power is over even with the strongest output modules and cost reductions the selected slots allow (bounds, decided after measuring the candidates, before the search; ~0.1 s). `"search"`: the bounds allow it, but the search found no combination that fits. `"capacitor"`: sets fit CPU, power and calibration, but none found meets `requireCapStable` / `minCapRecharge`; the message names the closest set's capacitor numbers. |
| `message` | For the user; says what to change (select or remove a pinned module, select slots for fitting modules). |
| `resources[]` | One entry per resource the pinned part is over on: `resource` (`cpu`, `power`, `calibration`), `pinnedUsed` / `output` (the pinned part, selected slots empty), `bestPinnedUsed` / `bestOutput` (bounds: the least the pinned modules can need and the most output the selected slots can reach; equal to the pinned values for calibration), `shortfall` (`max(0, bestPinnedUsed - bestOutput)`; 0 for `"search"`, where the bounds didn't decide), `message`. |
| `modules[]` | Pinned modules the message names (for `"calibration"`: the pinned rigs whose removal alone fixes it, else all pinned rigs): `position`, `slot`, `typeID`, `name`, `cost` (calibration). |

With `--check`, the fast exits are skipped so the brute force can confirm the diagnosis; the response has the same `status` and `infeasible`, plus `check` (with `feasible` 0).

How options map to positions, deterministically, per rack:
1. A found module with the same type and charge as a requested slot's current module stays in that slot. A second pass matches on the type only, so the module stays and its charge changes. Mutated current modules are never matched.
2. The remaining found modules, sorted by (typeID, chargeID), fill the remaining requested positions in ascending order.
3. Requested positions that are left over are empty (`typeID` 0).

States: activatable modules (those with an active effect) are `"active"`, all others `"online"`. That is what the optimizer evaluated. The stats are for these states, so pyfa should set them when it applies an option.

Consistency: every option's fit is put together again in pyfa's module order and evaluated with the full engine. If its goal differs by more than 1e-4 relative, a warning is added. None has so far.

## Applying an option in pyfa

For each `slots[]` entry: `typeID` 0 → `fit.modules.free(position)`. Otherwise build `Module(item)`, set its charge and state, and replace it at `position` (`fit.modules.replace(position, mod)`; the position is the list index). Then recalculate. pyfa's own numbers can differ from the response by about 1e-4 relative (float engine).

## In pyfa

pyfa uses this through **Optimize Modules...** in the fitting view's context menu (select high/mid/low/rig slots, empty ones too). The client is `service/moduleOptimizer.py` (request, runner, parser) and `gui/moduleOptimizerDialog.py`; see `OPTIMIZER.md`, section 4. pyfa runs `PyfaReader.exe --game <eve.db> --optimize-request - --out -` and always sends `prices` (from its price cache), so `--save` isn't needed. `parseResponse` accepts `ok` and `infeasible`; for `infeasible`, `OptimizerResult.infeasible` (`reason`, `message`, `resources`, `modules`) is set and the window shows "No option can fit." plus the message instead of an empty table. The window's **Max time (s)** field is the request's `timeBudget` (setting `timeBudget`, default 60; 0 = no limit), and the run timeout (setting `timeout`, default 300 s) is raised to at least the budget + 120 s (none with no limit). The exe path is a pyfa setting (default `<pyfa root>/cpp/bin/Release/PyfaReader.exe`, changed with the window's "Optimizer path..." button).
