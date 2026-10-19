# Module optimizer: stage 1 performance plan

Measurements and a plan for speeding up stage 1 (the model branch and bound in
`Optimizer.cpp`, `Search::stage1`). Read `OPTIMIZER_PLAN.md` (how stage 1 works) and
`OPTIMIZER_STATUS.md` first. Section 6 says what is done so far and what is left.

## 1. The case that prompted it

"full logi proteus" (saved fit 458), goal `localArmorRemoteArmor` (balanced local armor
tank + remote armor repair, weight 4). Open: the 5 remote repairers (high), the 3 cap
mids (not the afterburner), all 6 lows, 2 of 3 rigs (not the Command Processor). That is
positions `4,5,6,7,8,19,20,21,22,17,18,12,13,14,15,23` of the `--make-request 458 --open all`
request, with `"timeBudget": 0` so nothing is cut:

    PyfaReader.exe --make-request 458 --open all --goal localArmorRemoteArmor --out <WIN>\rq.json
    (set "optimize" true for exactly those positions, "timeBudget": 0)
    PyfaReader.exe --optimize-request <WIN>\rq.json --out <WIN>\rs.json --verbose

16 threads (the development machine), Release build.

| Goal (same open slots) | Total | Stage 1 | Stage 2 | Refine | Best |
|---|---|---|---|---|---|
| `armorActive` | 4.2 s | 1.0 s | 0.1 s | 3.0 s (converged) | 2612.4 |
| `armorRemote` | 6.2 s | 3.8 s | 0.4 s | 1.8 s (converged) | 451.9 |
| `localArmorRemoteArmor` | 63.6 s | **59.4 s** | 0.4 s | 3.6 s (converged) | 1705.6 |

The time budget only limits refine. With the default 60 s budget, stage 1 used it all
and refine was cut at once: the dialog showed 1664 instead of 1705.6.

Stage 1 statistics (`--verbose`): 63e9 core sets, racks fixed low (4092 multisets) ->
high (161) -> rig (435) -> mid (220); 180,557 partial sets expanded, 5,797,440 cores
bounded, 394,262 scanned, 20,111,133 resist sets scored, top-K 200. Bounds per level
(share above the final cutoff): level 1 2508 of 4092, level 2 151,696 of 403,788, level 3
26,352 of 416,931, level 4 394,261 of 2,864,433.

## 2. Where the time goes

`cpp/tools/sample_profile.py` (stack sampling, 5 ms, ~20k thread samples in stage 1):

- 86 % inclusive in `partialBoundSide` (bounds of partial sets).
- 53 % in `capRecord`, of which **40 % of all stage-1 time is `Cache<CapRecord>::get`
  itself**: the per-projection lookup (hash of a 128-bit key, shard, probe), millions of
  cheap hits. `partialBoundSide` loops over every projection of `capAll_` (the cap-relevant
  kinds) into the free racks and looks each up, per node and, for balanced goals, **per
  side** (the local and remote side run the identical loop).
- 26 % self in `partialBoundSide`: the projection loops, `keepMax` (a linear search over
  the size vectors), `withProjection` copying the whole kind-count vector (`memmove` +
  `vector::operator=` ~5 %).
- ~10 % `fixedFits` (it assembles a whole `CoreEval`).
- Thread use ~61 %: 7,852 of 20,178 thread samples idle (between batches: the main thread
  queues children and starts the next `Pool::run`).
- `armorActive` on the same slots has the same profile (`Cache<CapRecord>::get` first),
  with far fewer nodes: the fixes below help every goal.
- Refine: 3.6 s, mostly idle threads once the chains converge. Not a priority.

## 3. Rack order

`OPT_RACK_ORDER` (environment, comma list) overrides the order stage 1 fixes racks in.
Same request, same final result (1705.6) in every case:

| Order | Stage 1 |
|---|---|
| low, high, rig, mid (default) | 59.8 s |
| high, low, rig, mid | 76.3 s |
| low, mid, rig, high | 58.5 s |
| low, rig, mid, high | 23.6 s |
| rig, low, high, mid | 11.5 s |
| **low, rig, high, mid** | **9.6 s** |
| rig, mid, low, high | 151.2 s |

Single goals: `armorActive` default (low, rig, mid) 1.0 s, rig first 1.1 s; `armorRemote`
default (high, rig, low, mid) 4.0 s, rig first 3.0 s.

The default rule (`stage1()`): racks holding goal kinds first (most first), then rigs,
then the rest by multiset count. A balanced goal has goal kinds in two racks, so rigs
fall to third. A 16x spread between orders, and different winners per case, says no
fixed rule is reliably best: the order should be chosen per request (item 2 below).

Why order matters: a partial set's bound takes the free racks at their best
independently. Racks that decide the most (repairers: the value; rigs: they modify
everything, so records with rigs free are maxima over rig sets) should be fixed early,
but each fixed rack multiplies the nodes by its multiset count, so a big rack early is
costly unless it prunes well.

## 4. Plan (biggest win first)

1. **Memoize the capacitor part of the bound per node.** `valueBySize` from the
   `capAll_` projections depends only on the fixed counts of `capAll_` kinds and on
   `freeMask`: cache it under (packCounts(capAll_, fixed), freeMask). Most nodes share
   few keys (cap modules sit in mid/low/rig). Share it between the two sides of a
   balanced goal (it is the same loop). Same for the `minCapRecharge` recharge maximum.
   Expected: most of the 53 % `capRecord` time, for every goal.
2. **Choose the rack order dynamically.** Options, cheapest first:
   - *Rule fix as the baseline*: the rack with the most goal kinds, then rigs, then the
     other racks (for balanced goals the second repairer rack comes after the rigs). Gets
     the Proteus to ~10 s, but it is still a fixed rule.
   - *Probe the orders* (recommended): estimate the tree size of each candidate order
     (at most 4! = 24, fewer with racks that have one multiset) before searching, and
     search with the smallest. Knuth's estimator fits branch and bound: follow random
     root-to-leaf paths, at each level compute the bounds of all children of the current
     node (or a sample), count those above the cutoff, multiply the counts along the
     path; the average over paths estimates the node count. Needs a cutoff: get an
     incumbent first (see 3.). Bound computations are memoized, so probes also warm the
     caches. Give the probe a small budget (e.g. <= 5 % of a typical stage 1 or a fixed
     number of paths per order) and log the estimates next to the actual node counts to
     calibrate.
   - *Per-node branching* (most adaptive, most invasive): instead of one global order,
     each node picks its next rack to branch on, e.g. the one with the fewest children
     above the cutoff (as in "most constrained variable" in constraint search). Queue
     nodes would carry a fixed-rack mask instead of a level; `projections` is already
     keyed by `freeMask`. Consider only if probing leaves big gaps.
3. **Start stage 1 with an incumbent.** The cutoff is 0 until K sets are found, so early
   nodes are not pruned. A quick exact incumbent (e.g. a short refine-style descent from
   the empty set or a greedy fill, well under a second) gives a cutoff floor: prune nodes
   whose bound is below max(K-th score, incumbent x (1 - margin)). Stage 2 only used 58
   of the 200 kept sets here, so losing low-ranked top-K entries costs little; keep a
   margin so stage 2 still gets distinct starts.
4. **Smaller top-K for large searches** (`Options::topK`, 200). A higher cutoff prunes
   earlier. Untested: the request path can't set it; expose it (`RunOptions`) and measure
   final quality vs time on the benchmark set.
5. **No copies in the projection loops.** `withProjection` copies the whole count vector
   per projection; increment/decrement in place, or pass (base, projection) to the record
   functions. `keepMax`: index the size vectors (4 bits per rack) instead of a linear search.
6. **Thread use (~61 %).** Larger batches or overlapping the next batch's preparation with
   the current `Pool::run`; check the serial part between batches with the profiler.
7. **Tighter balanced bound.** Now `balancedScore(bound(L/D), bound(R))` with the sides
   bounded independently (different slot splits, the most capacitor either test can have).
   Bounding both sides over the same split of the free slots would tighten it. Must stay a
   valid upper bound; check with `--optimize-check`.

## 5. Benchmark set and acceptance

Run before and after each item (`--threads 16`, `timeBudget 0`, Release):

- Proteus 458, the slots above: `localArmorRemoteArmor`, `armorActive`, `armorRemote`.
- `--optimize 3411 armorActive` (Absolution, ~6.5 s total today), `--optimize 2593 armorActive`
  (Myrmidon, the slowest case in `OPTIMIZER_STATUS.md`), `--optimize 5 shieldActive`,
  `--optimize 910 armorPassive`.
- A remote and a balanced goal on a cruiser logi with all racks open, e.g.
  `--optimize 78 shieldRemote --racks mid,low,rig,high`.

Report stage 1 time, partial sets expanded, cores bounded/scanned, and the best goal.
Acceptance for every item:

- Same results: diff the option lines against the previous build (`--refine-rounds 15
  --time-budget 0 --threads 8`, see AGENTS.md); items 3 and 4 may change which top-K sets
  stage 1 keeps, so for them require the same or better best set on every case.
- `--optimize-check <fits> all` (and with `--racks mid,low,rig,high` on logi fits):
  0 mismatches. `--search-check 400`: 0 mismatches (engine untouched, but cheap to run).
- Profile again with `sample_profile.py` to confirm the hotspot moved.

## 6. Status (first round, done)

Done, in this order (each kept only if the benchmark stayed the same):

1. **Memoized value-side inputs of the partial bound** (item 1). `projTable(what, fixed,
   freeMask)`: per (fixed counts of `capAll_` / `inflHp_`, free racks) every projection's
   capacitor budget, recharge or HP with its cheapest cost, grouped by size, by value,
   dominated entries dropped; a node takes the first entry of each size that fits its
   room. `repItem(kind, side, fixed, freeMask)`: a repairer's best stats over its
   influencers' projections, per (fixed influencer counts, free racks, side). Same
   numbers as before (same summation order), so bounds are bit-identical.
2. **`fixedFits` sums costs only** (`assemble(..., costsOnly)`): it built rep profiles
   it never used (60 % of the remaining stage 1 time after 1.).
3. **Mixed batches.** One parallel loop per batch takes the best nodes, whole cores and
   partial sets alike (up to 512 cores, 64 partial sets, 4,096 children); before, a batch
   ended at the first whole core, and with cores and partial sets alternating near the
   cutoff most batches were tiny (55k `Pool::run`s on the Proteus). `Pool::run` has a
   guided mode (chunks shrink with the work left); the scans go last.
4. **Serial child filter.** Children whose cheapest cost (pinned + parent + the rack's
   multiset, precomputed per multiset) is over the output bound by more than any
   summation order changes are dropped before the parallel loop; `partialFits` decides
   the rest exactly.
5. **Rack order rule fix** (item 2, the baseline variant): the rack with the most goal
   kinds, then rigs, then a balanced goal's second goal rack, then the rest. Changes only
   balanced goals. Probing (Knuth) is not done.
6. **Dive for a first cutoff** (item 3, exact variant): until K sets are kept, nodes
   come depth first (best child first) from a stack instead of best first from the
   queue, so cores are scanned early and the cutoff rises. Every node is still expanded
   or pruned against a true K-th score: same top-K (up to ties at the K-th score). No
   incumbent from outside the model, no margin.
7. **Per-node output bound** (`nodeOutputMax`): `partialFits` compares the fixed modules'
   cheapest costs with the output reachable from the fixed core modules' own output
   bonuses plus the strongest output modules in the slots still free, instead of every
   rack full of output modules. Myrmidon 721 -> 248 partial sets, Hawk 64k -> 25k cores.
8. **Memory.** The queue is a vector heap compacted (nodes at or below the cutoff
   dropped) whenever it has doubled since the last compaction; the per-level bound log is
   kept only with `--verbose`. The Osprey balanced case below used 19.5 GB after 15 min
   before; now ~1 GB after 2 min.
9. **Stage 1 time cap.** With `timeBudget` > 0, stage 1 stops at half of it; stage 2 and
   refine go on from the sets found so far (warning "stage 1 stopped at half the time
   budget ...", response `stats.stage1Stopped`). `timeBudget` 0 never stops it.

Also: `--verbose` prints a stage 1 progress line every 2 s (nodes per level, queue,
top bound, cutoff) and the wall time / batch count at the end.

Benchmark (16 threads, `timeBudget` 0, prices frozen in the request; stage 1 seconds,
then partial sets expanded):

| Case | Before | After | Best (same) |
|---|---|---|---|
| Proteus 458 `localArmorRemoteArmor` | 61.2 s, 180,557 | **4.8 s**, 63,092 | 1705.57 |
| Proteus 458 `armorActive` | 1.04 s | 0.58 s | 2612.42 |
| Proteus 458 `armorRemote` | 4.21 s | 0.80 s | 451.86 |
| Absolution 3411 `armorActive` | 3.89 s | 2.68 s | 4120.28 |
| Myrmidon 2593 `armorActive` | 6.23 s, 719 | 2.54 s, 248 | 3447.46 |
| Incursus 5 `shieldActive` / `armorActive` | 0.34 / 0.18 s | 0.36 / 0.16 s | same |
| Abaddon 910 `armorPassive` | 4.72 s | 4.86 s | 473899.7 (resist tables; untouched) |
| Hawk 123 `shieldActive` | 0.21 s | 0.13 s | 1894.75 |
| Cyclone 3347 `shieldActive` | 3.02 s | 2.34 s | 4250.44 |
| Osprey 78 `shieldRemote`, all racks + highs | 8.70 s | 1.90 s | 587.88 |
| Osprey 78 `shieldActive`, all racks + highs | - | 3.23 s | 2064.96 |

Every option line is identical to before except Proteus `armorRemote`: many sets tie at
the same model score there, so which 200 stage 1 keeps depends on the processing order;
the best goal is the same and the cheapest option is cheaper (198.8M vs 200.6M ISK).

Validation: `--optimize-check 5,123,3347,3411,910,2593 all` 78 cases, 0 mismatches;
`--optimize-check 458,78 all --racks mid,low,rig,high` (also `--slots 2,2,1,2`) 26 + 26
cases, 0 mismatches (the remote and balanced goals there have up to 14M combinations);
`--optimize-check 5,123,3347,3411,910,2593 all --slots 3,3,2` 78 cases, 0 mismatches;
`--search-check 400` 0 mismatches (engine untouched).

### Left

- **Osprey 78 `localShieldRemoteShield`, all racks + highs** (4.2e11 core sets; mid
  42,504 multisets): stage 1 does not finish in minutes (before or now). With the default
  60 s budget it now stops at 30 s and the run ends in 35 s with 1434.2 (refine; stage 1
  had 798, stage 2 1015). The bound is not far off (top bound ~1900 after 100 s), but
  the dive's first cutoff is poor (411): it spends ~30 s in level-3 nodes (mid, rig, high
  fixed) whose low children never fit. `partialFits` treats the free low rack as full of
  output modules and `fixedFits` is skipped because core lows change outputs; only the
  whole core's exact check rejects them. Next: a fitting bound that stays exact-ish with
  output-changing core kinds in free racks (e.g. `fitSlack` maximized over the free
  racks' projections of `inflFit_` kinds), and/or a better incumbent (item 3 proper).
- Rack order probing (item 2), smaller top-K (item 4), tighter balanced bound (item 7).
  For balanced goals the engine evaluates the two sides as separate tests (each with the
  other side's repairers off), so the sides don't share the capacitor; they share slots,
  CPU and power. A joint bound would bound both sides over the same slot split and
  capacitor projection, and split CPU / power between them (concave in the split).
- Thread use: stage 1 batches are often small because the frontier above the cutoff is
  narrow; the remaining cost is mostly whole-core bounds (`assemble` + `feasibleMinD`).
