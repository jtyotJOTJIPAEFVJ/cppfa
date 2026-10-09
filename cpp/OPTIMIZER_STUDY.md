# Module optimizer: improvement study (refine)

Companion to `OPTIMIZER_STATUS.md` (how the optimizer works; refine is item 10a there) and `OPTIMIZER_CLI.md` (request fields and test flags). This file covers the study behind refine: how the gaps were measured, what was tried, what was adopted, and what is still open.

Goal (from the user): find better module combinations than stage 1 + 2 did. Up to ~60 s for a complex case is acceptable; simple cases should stay fast. All numbers: Release x64, Ryzen 7 7800X3D (8 cores / 16 threads).

## 1. Summary

- **The old pipeline (stage 1 + 2) missed often.** On small requests (2-3 open slots), it missed the exhaustive member-level optimum in 10-13 % of the feasible requests, by 14 % on average and up to 35 %. On whole racks, 7 of the 19 large test cases were below the best known set, by up to 14.9 % (mean 2.25 %).
- **The cause is mostly representative choice under binding fitting.** Stage 1 sees one member per subgroup, the strongest. When CPU, power or the capacitor binds, the right answer uses weaker, cheaper-to-fit members, often traded across subgroups. Examples: a 'Radical' damage control for a Damage Control II, two 'Posse' hardeners for a T2 hardener plus an extender, Syndicate plates, a compact cap booster. Stage 2 only swaps within a subgroup, so it rarely gets there.
- **Adopted: refine.** It is an exact member-level local search after stage 2:
  - single and pair moves over every member and charge of each rack, with dominated choices pruned
  - then an iterated local search on 16 chains until it converges or the time budget (`timeBudget`, default 60 s) runs out
  - an exact enumeration pass when the request is small (at most 3e7 combinations)
- **Result.** It reaches the best known set on all 19 large cases (+1.2 % to +17.5 % on 7 of them) and on every feasible small request checked against the exhaustive reference (197 requests with 2-3 open slots, 0 misses; 20 with 4 open slots, where the one miss is fixed by the exact pass). Simple requests still take ~0.3-1.3 s. Whole racks take 1-10 s at 16 threads and 5-37 s at 1 thread, and every result is an exact engine evaluation.

## 2. Reference method

1. **Member brute force** (`Options::memberBruteForce`, `--members`; `Search::enumerate`). It evaluates every combination of every module choice of the open slots exactly:
   - choices: all useful members of every subgroup plus all charges, *unpruned*
   - per rack, the multisets with group/type limits
   - plan layout: racks that fit the plan's item limit are open slots; one rack may give the plan a single "last module" slot while its prefixes go to the outer loop; the remaining racks are pinned per plan, choosing the layout with the fewest plans
   - speed: 0.5-8M combinations/s at 16 threads, depending on the layout. Fine for 2-3 open slots, sometimes 4; it gives up beyond 5e9 combinations.

   `members.py` (in `%TEMP%\opt_study`) runs it on random subsets of the open slots of the 12 parity cases and the 7 saved-fit goals. Each run compares stage 2's best (`stats.stage2Best`), the final best and the exhaustive best.
2. **Long-running references for whole racks.** Several differently configured refine runs on the 12 parity cases and the 7 whole-rack saved fits (5, 123 ×2 goals, 3347, 3411, 910, 2593), up to 300 s each:
   - uniform kicks, 2,400 iterations, 1,024 pair checks
   - guided kicks only
   - starting from the empty set instead of stage 2's sets
   - pruned, 8,000 iterations

   The best known value per case is the maximum over all runs.
3. **3-position certificate** (`cert.py`). Put the final best set into the request, reopen 3 random positions of it, and run the member brute force on them. A better completion would be a 3-change refine missed. Result: 57 subsets on the 19 large cases, 0 better (4 skipped as too large).

## 3. Gaps found (old pipeline vs reference)

| Test set | Requests | Old pipeline (stage 2) misses | Size of the misses |
|---|---|---|---|
| Small requests, 2-3 open slots (first sweep) | 191 feasible (36 infeasible, 1 too large) | 19 | mean 14.2 %, max 34.8 % |
| Same subsets, final build (incl. 6 that used to fail, see below) | 197 feasible | 25 | - |
| 4 open slots | 20 feasible (4 timed out at 600 s) | 5 | up to 12.2 % |
| Whole racks, 12 parity cases + 7 saved-fit goals | 19 | 7 below best known | mean 2.25 %, worst 14.9 % (case 11) |

Also found:
- **Request errors.** Stage 1's model has key-size limits. "Too many influencing kinds for one record" failed 6 of 228 small requests outright, e.g. case 1 with `low:2; mid:0; rig:0` open. Refine now handles these alone, see 4.
- **Prices.** Candidate prices didn't include the charge, unlike the response's `price`, so equal-goal sets were tie-broken on module prices only. Now they include it.

## 4. Strategies tried

Unless noted, the runs cover the 19 large cases at 16 threads. "At best" means equal to the best known value. Time is the mean per case for the whole run.

| # | Strategy | At best (of 19) | Mean / worst gap | Mean time (max) | Verdict |
|---|---|---|---|---|---|
| S0 | Old pipeline (stage 2 only, `--no-refine`) | 12 | -2.25 % / -14.9 % | 1.9 s (6.8) | baseline |
| S1 | + descent over every member, single moves only | 13 | -0.94 % / -3.8 % | 1.9 s (6.9) | helps; cross-subgroup swaps and other members |
| S2 | + pair moves (256 verified per test) | 18 | -0.08 % / -1.5 % | 1.9 s (6.9) | big step: trades that free CPU/power for an upgrade; refine itself takes ~0.05 s |
| S3 | + ILS, uniform kicks, 96 iterations patience, unpruned (12 parity cases only) | 11 of 12 | -0.13 % / -1.5 % | 1.9 s (4.4) | case 4 still missed |
| S4 | + ILS, 50 % guided kicks, patience 96, unpruned | 18 | -0.08 % / -1.5 % | 2.7 s (8.6) | case 4 still missed |
| S5 | S4 + dominated choices pruned | 18 | -0.08 % / -1.5 % | 2.2 s (7.1) | same results, refine ~4x cheaper |
| **S6** | **S5 with patience 800 (adopted)** | **19** | **0 / 0** | **4.2 s (9.8)** | finds case 4 (+1.56 %, a different 6-module basin) |
| S7 | S6 without charge alternatives | 19 | 0 / 0 | 4.0 s (9.7) | no effect on these cases; kept (cheap; the small-request reference includes them) |
| S8 | S6 with uniform kicks only | 19 | 0 / 0 | 4.3 s (9.9) | as good here |
| S9 | S6 with guided kicks only | 18 | -0.08 % / -1.5 % | 4.1 s (9.7) | misses case 4; the 50/50 mix is kept |
| - | More phase-1 starts (64, 200) | no change | | | not adopted |
| - | Start from the empty set (`OPT_REFINE_FROM_EMPTY`, long) | 18 | case 7 stays at stage 2's value | | stage 2's starts matter |
| - | Long references (patience 2,400-8,000, pairs 1,024, up to 300 s) | 19 | equal to S6 everywhere | 20-45 s | S6 is converged on these cases |
| - | Exact pass for small requests (≤ 3e7 combinations) | - | fixes the one 4-slot miss (Hawk case 10, 1 low + 2 mid + 1 rig: 272.11 → 309.95, +13.9 %) | that request: 6.4 s at 16 threads, 50 s at 1 thread; small requests' median unchanged (0.65 s) | adopted |
| - | Tabu on the kicked module while the set doesn't fit | no change | | +60 % sweeps | reverted |
| - | Unfit sets ordered by overuse (repair step by step) | no change | | +240 % sweeps | reverted |
| - | Chains accept worse local optima 10-100 % (`OPT_ACCEPT`) | no change on the 4-slot miss | | | kept as a study hook, default 0 |
| - | Pareto members as stage-1 kinds (plan idea 6.1/6.2) | not implemented | | | refine covers what it would fix; the kind count would hit the model's limits |

The 4-slot miss is worth noting. The found set was **3-opt**: no exhaustive 2- or 3-position change improved it. The optimum (Dread Guristas co-processor, Pithum B-type booster, compact cap booster with Navy charges, Ancillary Current Router I) changes all 4 slots, with CPU at 258.0 of 258.9 tf. The iterated search found it only after ~32k iterations. The exact pass finds it directly.

## 5. Adopted (defaults)

| Option | Default | Notes |
|---|---|---|
| `Options::refine` | on | `--no-refine` gives the old pipeline |
| `timeBudget` (request field, `Options::timeBudget`) | 60 s | whole run; refine stops at it. 0 = no limit (stops when converged) |
| `refinePatience` | 800 chain iterations | converged when the best set doesn't improve |
| `refinePairs` | 256 | pair moves verified exactly per local-optimum test (0 = single moves) |
| `refineChains` | 16 | fixed, so results don't depend on the thread count |
| `refineStarts` | 16 | stage 2's best distinct end sets |
| `refineExact` | 3e7 | exact pass when the pruned choices give at most this many combinations (and ≤ 20k plans) |
| guided kicks | 50 % | `OPT_KICK` study hook |
| dominance pruning | on | `OPT_NO_PRUNE=1` keeps every choice |
| model-limit fallback | on | `ModelLimit`: refine alone from the empty set instead of an error |

pyfa:
- `ModuleOptimizerSettings.timeBudget` (60) and the window's **Max time (s)** field, which is sent as the request's `timeBudget`.
- The run timeout becomes max(`timeout`, budget + 120 s), or none when the budget is 0.
- Progress shows `refine: round N, best G`.

## 6. Final tables

12 parity cases (`opt_parity/run_cases.py`; requests with a 150M ISK per-module cap, 1 and 16 threads). "Before" is the pipeline as of the stage-2 fix and the rig split (`summary_before_refine.json`). "After" is this build. Both thread counts give the same best set in every case, and no run hit the time budget.

| Case | Goal | Before | After | Gain | Time before, 1t / 16t (s) | Time after, 1t / 16t (s) | Price (M ISK) before → after |
|---|---|---|---|---|---|---|---|
| 1 | armorActive | 1,958.06 | 1,958.06 | 0 | 2.3 / 1.0 | 15.9 / 3.7 | 649.7 → 649.7 |
| 2 | armorPassive | 233,547 | 242,463 | +3.82 % | 1.8 / 0.8 | 14.8 / 2.9 | 510.6 → 520.4 |
| 3 | armorActive | 1,958.06 | 1,958.06 | 0 | 2.3 / 1.0 | 15.9 / 3.6 | 649.7 → 649.7 |
| 4 | shieldActive | 966.21 | 981.28 | +1.56 % | 22.3 / 3.2 | 36.0 / 6.0 | 154.3 → 391.6 |
| 5 | shieldPassive | 84,914 | 84,914 | 0 | 0.4 / 0.3 | 4.7 / 1.0 | 177.1 → 177.1 |
| 6 | armorActive | 386.35 | 386.35 | 0 | 1.5 / 0.5 | 7.7 / 1.9 | 384.5 → 384.5 |
| 7 | shieldActive | 2,609.45 | 2,793.37 | +7.05 % | 19.5 / 2.9 | 36.7 / 6.4 | 379.8 → 472.9 |
| 8 | armorPassive | 251,600 | 251,600 | 0 | 1.2 / 0.8 | 6.2 / 4.1 | 531.0 → 531.0 |
| 9 | armorActive | 771.13 | 771.13 | 0 | 2.5 / 0.7 | 10.1 / 2.2 | 566.6 → 566.6 |
| 10 | shieldActive | 329.32 | 329.32 | 0 | 1.0 / 0.4 | 9.8 / 2.4 | 231.3 → 231.3 |
| 11 | shieldPassive | 11,933 | 14,018 | +17.47 % | 0.3 / 0.2 | 4.9 / 1.3 | 21.9 → 244.1 |
| 12 | armorActive | 1,372.32 | 1,389.15 | +1.23 % | 8.8 / 2.6 | 18.5 / 4.5 | 691.1 → 768.3 |

The higher-goal sets cost more because the cap applies per module, not per fit. The goal is first, then price.

Saved fits, whole racks (`--make-request <fit> --open all`, no price cap), 16 threads. "Before" = `--no-refine`. **The 1-thread times for these were not measured in the final build**; refine's thread time suggests ~10-45 s.

| Fit / goal | Before | After | Gain | Time before / after (s) |
|---|---|---|---|---|
| Incursus 5 armorActive | 529.68 | 529.68 | 0 | 0.4 / 2.2 |
| Hawk 123 shieldActive | 1,764.57 | 1,894.75 | +7.38 % | 0.5 / 3.6 |
| Hawk 123 shieldPassive | 58,507 | 63,575 | +8.66 % | 0.2 / 1.2 |
| Cyclone 3347 shieldActive | 4,250.44 | 4,250.44 | 0 | 3.2 / 6.7 |
| Absolution 3411 armorActive | 4,120.28 | 4,120.28 | 0 | 4.4 / 8.6 |
| Abaddon 910 armorPassive | 473,900 | 473,900 | 0 | 5.4 / 8.1 |
| Myrmidon 2593 armorActive | 3,447.46 | 3,447.46 | 0 | 6.8 / 9.7 |

Small requests (2-3 open slots, member sweep), whole run without the brute force:
- before refine (old `stats.stage2Best` runs): ~0.3 s median
- with refine (patience 800, no exact pass): median 0.63 s, 90th percentile 1.07 s, max 2.64 s
- with the exact pass (partial final sweep, 151 requests): median 0.65 s, 90th percentile 1.23 s, max 2.73 s

Time cost overall: +0.5-1 s for small requests, +1-5 s at 16 threads for whole racks, and about 2-3x the old time at 1 thread (all under the 60 s budget).

## 7. Validation of the final build

- `--search-check 0`: 0 mismatches, 0 detail mismatches (306,000 tests).
- `--optimize-check 5,123 all`: 10 cases, 0 mismatches (refine doesn't run in the stage-1 brute-force check).
- Build: warning-free at /W4.
- `opt_parity/sweep.py` (475 subsets × normal + `--check`): 0 problems, 0 mismatches. This ran on the build before the exact pass; the exact pass only adds a start.
  - In 131 feasible subsets the optimizer beat the representative-level brute force.
  - Case 10 has 1 subset where only refine finds a fitting set; the representative brute force says infeasible. sweep.py now counts that as "only refine fits".
  - Max run time 5.3 s (4 threads, 4 runs in parallel).
- Member sweep, final build: interrupted at 151 of 228 requests (132 feasible), with **0 misses** and 18 stage-2 misses. The full 228-request run on the build just before (no exact pass) gave 197 feasible and 0 misses.
- Exact over 4 slots: the one miss (above) is fixed. The 4-slot sweep was not repeated on the final build.
- Determinism: 1 and 16 threads give the same best on all 12 parity cases. Results depend only on the round count, not on timing, unless the time budget stops refine (`stats.refineStop == "time"`): then the result depends on machine speed. `--refine-rounds N` gives a fixed-work mode for tests. Two 16-thread runs were not compared option by option in the final build (`det.py` was not run).

## 8. Remaining gaps and next steps

- **Refine is a local search.** It is not a proof of optimality on whole racks. Every reference agrees with it on the 19 large cases, and a 3-position exhaustive check around each result found nothing. Small requests are exact over the pruned choices when the exact pass runs (≤ 3e7 combinations). Larger requests that are still small (e.g. 4 open slots with big mid pools: case 7 or 12 with 3 mids open) are only covered by the iterated search; the brute force timed out on them, so they are unmeasured.
- **The pruning rule is a dominance heuristic.** It is checked by the member brute force (unpruned) on the 197 small requests (132 of them again on the final build) with no loss. A module that is weaker on every measured stat but helps through an unmeasured interaction would be lost. None is known.
- "Infeasible (search)" is still not a proof (see `OPTIMIZER_STATUS.md` 4.).
- Next steps:
  1. Faster plan reuse in refine. Measured (log line `refine thread time`, 1 thread): plan builds were 61-70 % of refine's time, not ~85 %, at 0.31-0.46 ms per plan; plan evaluations were 24-35 %. Plan construction has since been sped up ~2.9x (0.41 → 0.14 ms on Hawk 123 and Myrmidon 2593; refine 1.7-1.9x faster at 1 and 16 threads, identical results): item prototypes per type, a reused base scratch, a skill/group module index for modifier targets, precomputed skill modifiers and effect lists, and a linear order check. Plan builds are now ~40 % of refine and evaluations ~58 %, so first-improvement sweeps or incremental plans would now save less; per-test cost matters as much.
  2. A 1-thread time table for the saved fits, plus `det.py` (two 16-thread runs compared option by option).
  3. Finish the 4-slot member sweep on the final build, with a faster brute-force layout for big mid pools.
  4. The member brute force could replace stage 1 entirely for very small requests (1-2 slots), where it is faster than building the model.
  5. Optionally, keep refine's best few distinct sets per structure for more varied options in the result list.
