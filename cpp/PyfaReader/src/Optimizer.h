#pragma once

// Module search: the best modules for the open mid, low, rig and high slots of a
// fit for one goal (see cpp/OPTIMIZER_PLAN.md).
//
// Stage 1 searches with one representative per subgroup of similar modules and
// a separable model of the goal, value(core modules) / damage factor(resist
// modules), built from engine measurements. Branch and bound with exact upper
// bounds keeps most combinations from ever being scored. Stage 2 lets every slot
// of the best stage-1 sets try the other members of its subgroup, evaluated
// exactly with SearchPlan. Refine then searches every module choice of each rack
// (across subgroups and roles, with single and pair moves and an iterated local
// search), exactly, until the time budget is used or it converges.

#include "FitEngine.h"
#include "GameData.h"

#include <array>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <string>
#include <unordered_map>
#include <vector>

namespace fitengine::opt
{
enum Rack
{
    RackMid,
    RackLow,
    RackRig,
    RackHigh,  // remote repairers (other high slot modules are rarely useful to a goal)
    RackCount
};
const char* rackName(int rack);

struct Options
{
    Goal goal = Goal::ArmorActive;
    double remoteWeight = 4;  // balanced goals (Objective::remoteWeight): EHP/s per remote HP/s
    std::array<bool, RackCount> search{true, true, true, false};  // racks whose slots are searched
    bool allowOfficer = false;
    // Constraints. allowAncillary false: no ancillary repairers (local or remote) among
    // the candidates; allowCapBoosters false: no capacitor boosters. maxReps (active, remote and balanced goals; 0 = no limit): the
    // goal's repairers in the fit at most (local ones of the goal's layer, remote ones,
    // or both for balanced goals), the pinned ones included. requireCapStable: capacitor use within recharge
    // (TestResult::capUsed <= capRecharge, pyfa's simplified check). minCapRecharge:
    // TestResult::capRecharge (peak recharge + cap boosters, GJ/s) at least this. A set
    // that misses a constraint doesn't count as fitting.
    bool allowAncillary = true;
    bool allowCapBoosters = true;
    int maxReps = 0;
    bool requireCapStable = false;
    double minCapRecharge = 0;
    double priceCap = 0;  // per module, ISK; 0 = no cap (modules without a price count as over the cap)
    const std::unordered_map<int, double>* prices = nullptr;  // typeID -> ISK
    int topK = 200;       // stage 1 keeps this many sets
    int patience = 50;    // stage 2 stops after this many starting sets without improvement
    int results = 10;     // distinct final sets reported
    double boundPad = 1e-5;  // relative padding of upper bounds (model vs engine float noise)
    std::array<int, RackCount> maxSlots{8, 8, 8, 8};  // open slots per searched rack at most (the rest stay empty)
    int threads = 0;              // worker threads, 0 = one per hardware thread
    std::ostream* log = nullptr;  // progress and statistics
    bool dumpKinds = false;       // print the candidate / subgroup table to log and stop
    // Validation (small cases only): after stage 1, evaluate every combination of the
    // stage-1 kinds exactly and compare the best sets with stage 1's; skips stage 2.
    bool bruteForce = false;
    // Partial opening (pyfa requests, see cpp/OPTIMIZER_CLI.md). When `partial` is set,
    // `search` is ignored: openModules has one flag per FitInput module; flagged mid,
    // low and rig modules are removed and their slots opened, every other module is
    // pinned (also the unflagged ones of a searched rack). openEmpty adds requested
    // empty slots per rack. A rack's open slot count is its requested slots, at most
    // its free slots (the ship's slot count minus the rack's pinned modules) and maxSlots.
    bool partial = false;
    std::vector<uint8_t> openModules;
    std::array<int, RackCount> openEmpty{};
    // Progress reports (fraction 0..1, phase). Called from the optimizing thread.
    std::function<void(double, const std::string&)> progress;

    // Refine (after stage 2): exact local search over every module choice of each rack
    // (all subgroup members and charges, across subgroups and roles) from stage 2's
    // sets, then iterated local search (perturb, search again) on parallel chains
    // until the time budget is used up or the best set stops improving.
    bool refine = true;
    double timeBudget = 60;   // seconds for the whole run; refine stops when reached, stage 1 at half of it (0 = no limit)
    int refineRounds = -1;    // iterated-search rounds at most (-1 = no limit). Results are deterministic
                              // for a given thread count unless the time budget ends the run.
    int refinePatience = 800; // chain iterations without a better best set: converged
    int refinePairs = 256;    // pair moves verified exactly per local-optimum test (0 = single moves only)
    int refineChains = 16;    // chains (0 = one per thread); a fixed count keeps results independent of threads
    int refineStarts = 0;     // stage 2 end sets descended first, best first (0 = max(chains, 8))
    // Small requests: when the open slots have at most this many combinations of refine's
    // choices, refine first evaluates them all (exact over those choices). 0 = off.
    double refineExact = 30e6;
    // Validation (small requests): after the search, evaluate every combination of
    // every module choice (Result::members); the reference for refine.
    bool memberBruteForce = false;
};

struct FoundFit
{
    FitInput fit;          // pinned modules first, then the searched racks
    TestResult exact;      // SearchPlan evaluation
    bool fits = false;     // CPU, power and calibration within outputs
    double price = 0;      // searched modules only
    std::vector<int> typeIDs[RackCount];
    std::vector<ModuleInput> modules[RackCount];  // the searched modules as evaluated (charge, state)
};

struct Stats
{
    size_t candidates = 0, kinds = 0, coreKinds = 0, resistKinds = 0, fittingKinds = 0;
    double combos = 0, rests = 0, coreSets = 0, resistSets = 0;
    size_t combosOpened = 0, coresBounded = 0, coresScanned = 0, setsScored = 0;
    size_t engineCalls = 0, stage2Starts = 0, stage2Evaluations = 0;
    double seconds[6] = {};  // candidates, model, stage 1, stage 2, total, refine
    double stage2Best = 0;   // best fitting goal after stage 2 (before refine)
    size_t refineStarts = 0, refineRounds = 0, refineEvaluations = 0, refineImproved = 0;
    double refineExact = 0;  // combinations refine's exact pass evaluated (0: not run)
    std::string refineStop;  // why refine ended: "converged", "rounds", "time", "off"
    bool stage1Stopped = false;  // stage 1 hit half the time budget: its sets are the best found, not proven
    std::string modelSkipped;  // a stage-1 model limit that was hit: stages 1 and 2 skipped, refine only
};

// Options::memberBruteForce: every combination of every module choice (all members
// and charges of the open racks, empty slots included), evaluated exactly.
struct MemberCheck
{
    bool ran = false;
    std::string skipped;      // why it didn't run (too many combinations)
    double combinations = 0;  // evaluated
    size_t fitting = 0;       // of those, fitting with goal > 0
    double best = 0, bestPrice = 0;  // the best fitting set (goal, then cheaper)
    std::string bestModules;
    double seconds = 0;
};

// Options::bruteForce: stage 1's top sets against exhaustive enumeration over the
// same kinds (core, resist and fitting representatives, empty slots included).
struct BruteForceCheck
{
    bool ran = false;
    double combinations = 0;  // core + resist combinations evaluated
    size_t feasible = 0;      // of those, with goal > 0 and some fitting-module fill that fits
    size_t compared = 0;      // ranks compared (min(topK, feasible))
    size_t mismatches = 0;    // ranks whose scores differ (or sets stage 1 missed)
    size_t fittingGoalChanges = 0;  // fitting modules that changed the goal (classification error)
    double bruteBest = 0, stage1Best = 0;
    double maxError = 0;      // largest relative score difference over the compared ranks
};

// Why no option can fit (Result::infeasible). Only diagnosed when the pinned part
// alone (the open slots empty) is over CPU, power or calibration.
enum Resource
{
    ResCpu,
    ResPower,
    ResCalibration,
    ResourceCount
};
const char* resourceName(int resource);  // "cpu", "power", "calibration"

struct Shortfall
{
    int resource = ResCpu;
    double pinnedUsed = 0, output = 0;  // the pinned part, open slots empty
    // With the open slots' strongest allowed modules: the least the pinned modules can
    // need (cost reductions; calibration: none) and the most output reachable
    // (calibration: none raise it). Each is a bound, taken separately.
    double bestPinnedUsed = 0, bestOutput = 0;
    double shortfall = 0;  // max(0, bestPinnedUsed - bestOutput)
    std::string message;
};

struct Infeasibility
{
    bool infeasible = false;
    // "calibration": the pinned rigs alone are over (checked before any search).
    // "outputs": CPU/power over even with the open slots' best output and cost bounds
    //            (checked after measuring the candidates, before the model and search).
    // "search": the bounds allow it, but the search found no set that fits.
    // "capacitor": sets fit CPU, power and calibration, but none found meets the
    //              capacitor constraints (Options::requireCapStable, minCapRecharge).
    std::string reason;
    std::string message;  // for the user
    std::vector<Shortfall> resources;  // the resources the pinned part is over on
    std::vector<int> modules;  // FitInput::modules indices named in the message (pinned rigs)
    std::vector<double> moduleCost;  // their cost in the failing resource (calibration)
};

struct Result
{
    TestResult start;  // the input fit
    // Set when no option can fit. fits is then empty (Options::bruteForce still runs
    // the search, for validation, but the diagnosis is the same).
    Infeasibility infeasible;
    std::array<int, RackCount> openSlots{};  // slots searched per rack
    // Input modules (FitInput::modules indices) whose state clampPinnedState lowered;
    // the start fit and the pinned part are evaluated with the lowered state.
    std::vector<std::pair<int, State>> clamped;  // (index, evaluated state)
    std::vector<FoundFit> fits;  // fitting sets only, best first: by goal, then cheaper
    Stats stats;
    BruteForceCheck check;
    MemberCheck members;
    std::vector<std::string> warnings;
};

// Module rules of the search (add similar ones here).
// - Assault damage controls (effect moduleBonusAssaultDamageControl, 7012): the active
//   bonus lasts ~12 s, so it doesn't count toward tank. They are never candidates (a
//   regular damage control is always better then) and pinned ones are evaluated at
//   most online (their passive damageControl part).
bool excludedFromSearch(const GameData& gd, const Type& type);
State clampPinnedState(const GameData& gd, const Type& type, State state);

Result optimize(const GameData& gd, const Engine& engine, const FitInput& fit, const Options& options);
}
