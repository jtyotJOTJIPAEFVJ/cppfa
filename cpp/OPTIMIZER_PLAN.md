# Module search optimizer on the C++ engine: implementation plan

Goal: port the Python "-alt" multi-slot search (the *Alt module search: optimization strategies* report) onto the C++ fit engine (`FIT_ENGINE.md`), and improve on it where the engine makes that cheap.

## 1. The approach in one paragraph

Brute force is still out of reach in C++. The Maelstrom case has 276M core sets × 1.4k–65k resist sets each, and at 1.5 µs per `SearchPlan::evaluate` that is days. So the prototype's structure stays: collapse variants, use the separable score `V(core, fillerCap) / D(resists)`, branch and bound with a lazy global queue, then a variant search. What changes is **where the numbers come from**. The prototype had to measure pyfa from outside and keep hand-written formulas consistent with it. Here the engine *is* the reference, so every component of the separable model (rep profiles, cap budgets, resist tables, fitting costs) is produced by the engine itself and shares its arithmetic. The cheap model only runs the inner loop. Every result reported to the user is re-scored exactly with `SearchPlan`.

Three layers:

| Layer | Cost per call | Used for |
|---|---|---|
| `Engine::evaluate` | ~70 µs | candidate screening, final report |
| `SearchPlan::evaluate` | ~1–2 µs | building model tables, per-core profiles, exact verification, model check harness |
| separable model (`V(x)/D`) | ~5–20 ns | stage 1 inner loop, stage 2 scoring |

## 2. Separability: checked, not assumed

The prototype relied on hand-maintained knowledge, such as "resist modules only touch resonances and their own cap" or "rigs are the only core modules with resist effects". It also hit pitfalls: same-rack amplifiers, no-op modifiers, and the RAH sharing a stacking group with the damage control.

The engine already records, for each instruction, the source option and the `(item, field)` values it writes and reads. A new **influence query** on `SearchPlan` exposes that per option. It reports which of these each option writes, directly or transitively through other dynamic instructions:
- ship HP of a layer
- ship resonances of a layer
- cap capacity/recharge
- fitting outputs
- the fields of other options (which ones)

Roles are then decided from data:
- **Resist filler:** writes only resonances, plus its own cycle/cap, and no goal value.
- **Core:** writes anything that reaches V.
- **Extras (`E_C`):** a core kind that writes resonances *or* a filler's fields (resist rigs, anything that boosts hardeners). These form the key of the D tables.
- **Fitting filler:** writes only fitting outputs.

A kind that breaks the split, for example one that writes both resonances and a repairer's amount, becomes core with the extras flag, so the decomposition stays exact.

The same query replaces the goal-group lists for relevance. A candidate whose instructions never reach the goal's roots, or fitting/cap, is dropped. The explicit skip list stays as config for kinds that are relevant but always harmful, such as capacitor power relays for shield active.

## 3. Work breakdown

### Phase 0: engine extensions (small, keep validation at 0 mismatches)
1. **`TestResult` additions:**
   - per-layer `hp` and resist divider `D`
   - `capPeak`
   - optionally a `RepProfile`
2. **Split `sustainableTank`** (`FitEngine.cpp:1589`) into:
   - `RepProfile extract(...)`: the base rate, the cap budget before repairers, and the repairers in efficiency order with their cap/s and rate per unit of sustainability
   - `double RepProfile::value(double extraCapUse)`

   The engine calls `value(0)`, so the optimizer's `V(core, x)` runs *the same code* as the engine. Rerun `--search-check` and the pyfa comparison to confirm nothing moved.
3. **`SearchPlan::influence()`:** the per-option write closure from section 2, built from the recorder's existing read/write lists.
4. **Item limit:** `kMaxItems = 160` (`FitEngine.cpp:25`) caps plan size, and a core plan with 6 mids × 20 kinds + lows + rigs exceeds it. Make the limit larger (e.g. 512) or size it dynamically. Check that `--bench` speed doesn't regress, since the per-test setup loops over `b.count`.

### Phase 1: candidates, measurement and subgroups (`opt/Candidates.*`)
1. **GameData helpers** that are missing today:
   - modules by slot
   - `canFitShipGroup*` / `canFitShipType*`
   - `rigSize` vs ship, and capital-size rules
   - `maxGroupFitted` / `maxTypeFitted`
   - meta group (officer filter)
   - skill requirements vs `Character`
2. **Prices:** read pyfa's `prices` table from saveddata.db. No price counts as over the cap, as in the prototype.
3. **Static filters**, plus group limits already used up by pinned modules.
4. **Measurement:** one SearchPlan per rack, with a single open slot whose options are all of that rack's candidates (chunked to fit the item limit). Each candidate is then measured alone by choosing it. That gives exact isolation with no batching rules. Rig × representative interactions are measured the same way, with a rig slot added.
5. **Strength tuple** (goal gain, resist gain, output gain, cost gain), **roles** (section 2), **subgroup key** (rack, group, damage-type set from *measured* non-unit resonance changes, size), and **representative** = strongest member.
6. **CLI:** `--optimize <fit> <goal> --slots mid,low,rig --dump-kinds` prints the kind table for review against the prototype's numbers (476 → 49 armor, 334 → 43 shield).

### Phase 2: separable model and exactness harness (`opt/Model.*`)
1. **Resist tables.** For each extras key, enumerate every resist multiset per rack within slot counts. For each set store:
   - `D` per goal layer
   - filler cap/s
   - CPU/PG/calibration cost
   - per-rack size vector

   The values come from a "resist plan" (SearchPlan: resist slots + extras options), which is exact even with an RAH, pinned resist modules in the same stacking chains, and bursts. Estimated cost: 65k × 15 keys at ~0.5 µs ≈ 0.5 s, done in parallel. If profiling says otherwise, switch to analytic stacking from measured multipliers, with sets containing an RAH still going through the engine.
2. **Sorted views:** sorted indices per (extras key, free-slot vector), built lazily, in structure-of-arrays layout.
3. **Core profiles:** a "core plan" over the core representatives gives each core its `RepProfile` (active goals) or HP (passive), cap budget, fitting costs and outputs. Memoized on the core's kind-count vector (one byte per kind, hashed). Repairer stats per rig set are cached the same way for the combo bound.
4. **Fitting check:** the summed costs vs outputs, each from engine-measured per-kind values keyed by the rigs that influence them. When the result is within 0.02 of a limit, the exact `SearchPlan` verdict decides (float sum order and 2-decimal rounding).
5. **`--optimize-check` harness:** random (core, resist, fitting-filler) sets per test fit; model score and fits-verdict vs `SearchPlan::evaluate` of the same full set. It reports the max relative error, which sets the bound padding (expect ~1e-6, pad 1e-5). This harness gates every later phase, like `--search-check` does for the engine.

### Phase 3: stage 1 branch and bound (single thread first) (`opt/Stage1.*`)
1. **Multiset enumeration** with group limits enforced while generating: combos (primary rack), rests, resist sets. Count vectors; combinatorial-number-system ranks as ids.
2. **Fitting prefilter** (cost scaled by the best rig reduction vs the best reachable outputs).
3. **Core bound:** `V(core, 0) × (1+pad) / min D(extras, free vector)`.
4. **Combo bound:** the fractional-knapsack relaxation over all rests:
   - the largest cap budget any rest leaves
   - each repairer at its best rate and lowest cap across rig sets
   - the largest fixed rate
   - the smallest D any rest's free slots reach

   Passive goals use the max of the per-rest bounds.
5. **Lazy global queue:** the next unscanned core of every opened combo plus the next unopened combo's bound. Top-K heap (K = 200).
6. **Core scan:**
   1. cut the sorted resist list where `V(core,0)/D` falls to the cutoff (binary search)
   2. score the rest with `V(core, capUse(R)) / D`
   3. fill leftover slots with fitting-filler options until the set fits
   4. push sets that beat the cutoff
7. **Debug mode:** assert `bound ≥ score` for every scored set. On small cases (Incursus, Absolution EHP) cross-check the top-K against brute force over representatives.

### Phase 4: stage 2 and final verification (`opt/Stage2.*`)
1. **Variant search** from stage-1 sets in score order:
   - single swaps within the locked subgroup, then pair swaps, until neither improves
   - key (fits, value, −price)
   - memo on the sorted multiset
   - stop after 50 starting sets in a row without improvement

   Scoring uses the model with per-variant measurements from Phase 1.
2. **Final exact pass:** re-score the reported sets with `SearchPlan` (one plan per final set, its variants as options) and rank by the exact score. Report any model-vs-exact difference over the pad as a warning, which should never happen.
3. **Output:** EFT export of the best fits (the existing `Eft.*`), gain over the starting fit, price, fitting margins and cap. Note when nothing can fit because the pinned modules alone exceed an output.

### Phase 5: parallelism and performance
1. **Parallel stages:**
   - parallel table builds and per-combo bounding
   - core scanning with per-thread top-K heaps and a shared cutoff read with relaxed atomics (a stale cutoff only costs work)
   - a deterministic merge (ties broken by set rank)
   - stage-2 starting sets in batches, with the 50-in-a-row rule applied in order afterwards
2. **Benchmark:** `--optimize-bench` on the prototype's test fits, with time per phase and counts (cores bounded/scanned, sets scored, engine calls).
3. **Targets:** most fits well under 1 s single thread; Maelstrom shield active in a few seconds single thread and under 1 s with 16 threads. These are estimates, not promises; the first measurement after Phase 3 will tell.

### Phase 6: improvements beyond the prototype (each one measured and opt-in until proven)
1. **Optimistic representatives → exact search.** Stage 1 currently pairs the strongest member's value with that member's own fitting cost. Instead, it could use the strongest value with the *cheapest* member's cost, a virtual module that bounds every member of its subgroup. Stage 1 then bounds whole variant families, and stage 2 becomes an exact search inside each family. The search stops when the next family's bound is below the best exact set found. The result is provably optimal under the model, not "best of the 200". It also fixes the known weakness on CPU-starved fits, where strong representatives don't fit.
2. **Pareto pruning of variants** by (strength, CPU, PG, calibration, price). This shrinks stage-2 neighbourhoods and the families in Phase 6.1.
3. **Cap-stability constraint** as an option, since the engine returns cap use/recharge anyway.
4. **New goals** (DPS) need their own V and bound. The queue, tables and stage 2 carry over.

## 4. Validation summary

| Check | Compares | Pass criterion |
|---|---|---|
| existing `--search-check`, pyfa comparison | engine after Phase 0 | unchanged (0 / same 11 known diffs) |
| `--optimize-check` | model vs `SearchPlan` on random sets | max rel. error < pad; fits verdicts agree |
| bound asserts (debug) | every bound vs every score below it | never violated |
| brute force on small cases | stage 1 top-K vs exhaustive | identical |
| prototype parity | 13 prototype test cases: kinds, top-200, final sets and gains (table in the report) | same or better final values; differences explained (float engine vs pyfa double) |

## 5. Risks and open points

- **Item limit / plan size:** Phase 0.4. If raising it slows the per-test setup, split core plans per rack group instead.
- **Combo-bound inputs** ("best rate across rig sets") need repairer × rig-set measurements. Rig sets are few (rig multisets ≤ a few hundred), so this is a cached table, not a problem, but it is the most intricate piece to get exactly right.
- **Subgroup "size"** still comes from names, as in the prototype. Could be derived from attributes later.
- **Prices:** pyfa's table may be stale or missing. The CLI gets a flag to ignore price.
- **The 12 order-sensitive plans** found by `--search-check` never produced a mismatch. The model harness will show whether the optimizer's plans hit that.
- **Parity data:** matching the prototype needs its 13 test cases (fit IDs, searched slots, options) and its outputs. The Python sources themselves aren't needed for the design.
