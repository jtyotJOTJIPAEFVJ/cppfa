#pragma once

// Module search: the best modules for the open mid, low and rig slots of a fit
// for one goal (see cpp/OPTIMIZER_PLAN.md).
//
// Stage 1 searches with one representative per subgroup of similar modules and
// a separable model of the goal, value(core modules) / damage factor(resist
// modules), built from engine measurements. Branch and bound with exact upper
// bounds keeps most combinations from ever being scored. Stage 2 lets every slot
// of the best stage-1 sets try the other members of its subgroup, evaluated
// exactly with SearchPlan.

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
    RackCount
};
const char* rackName(int rack);

struct Options
{
    Goal goal = Goal::ArmorActive;
    std::array<bool, RackCount> search{true, true, true};  // racks whose slots are searched
    bool allowOfficer = false;
    double priceCap = 0;  // per module, ISK; 0 = no cap (modules without a price count as over the cap)
    const std::unordered_map<int, double>* prices = nullptr;  // typeID -> ISK
    int topK = 200;       // stage 1 keeps this many sets
    int patience = 50;    // stage 2 stops after this many starting sets without improvement
    int results = 10;     // distinct final sets reported
    double boundPad = 1e-5;  // relative padding of upper bounds (model vs engine float noise)
    std::array<int, RackCount> maxSlots{8, 8, 8};  // open slots per searched rack at most (the rest stay empty)
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
    double seconds[5] = {};  // candidates, model, stage 1, stage 2, total
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

struct Result
{
    TestResult start;  // the input fit
    std::array<int, RackCount> openSlots{};  // slots searched per rack
    // Input modules (FitInput::modules indices) whose state clampPinnedState lowered;
    // the start fit and the pinned part are evaluated with the lowered state.
    std::vector<std::pair<int, State>> clamped;  // (index, evaluated state)
    std::vector<FoundFit> fits;  // best first: fitting sets by goal, then cheaper
    Stats stats;
    BruteForceCheck check;
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
