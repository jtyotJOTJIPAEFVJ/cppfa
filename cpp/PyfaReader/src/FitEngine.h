#pragma once

// Fit evaluation engine: reproduces pyfa's calculation for the attributes the
// tank / fitting / capacitor goals need (see tools/goal_closure.py), using the
// generated compact layouts (src/generated/FitSchema.generated.h).
//
// Sources supported: ship, character skills, implants (incl. set bonuses),
// boosters (incl. enabled side effects), modules (with state), charges, T3
// destroyer modes, the fit's own command bursts. Not yet: bursts from other
// fits, system effects, projected effects, drones/fighters.
//
// Matches pyfa with: All-5 style character, uniform damage profile, reload
// factored into cycle times.
//
// Evaluation keeps pyfa's order (early/normal/late passes; skills, ship,
// modules, mode) and lazy attribute reads, so effects that read attributes
// other effects modify see the same values pyfa does. Values are float (pyfa
// uses double): results match pyfa to ~1e-4 relative, see cpp/FIT_ENGINE.md.

#include "GameData.h"
#include "generated/FitSchema.generated.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fitengine
{
using fitschema::Attr;

// Values match eos.const.FittingModuleState.
enum class State : int8_t
{
    Offline = -1,
    Online = 0,
    Active = 1,
    Overheated = 2,
};

struct ModuleInput
{
    int typeID = 0;
    int chargeID = 0;  // 0 = no charge
    State state = State::Online;
    // Abyssal (mutated) modules: typeID is the mutated type, baseTypeID the original
    // item. Attribute originals are mutators, then typeID's, then baseTypeID's.
    int baseTypeID = 0;
    std::vector<std::pair<int, float>> mutators;  // eve attributeID -> value
};

struct ImplantInput
{
    int typeID = 0;
    bool active = true;
};

struct BoosterInput
{
    int typeID = 0;
    bool active = true;
    std::vector<int> sideEffects;  // effectIDs of the side effects switched on
};

struct FitInput
{
    int shipTypeID = 0;
    int modeTypeID = 0;  // T3 destroyer mode, 0 = none
    std::vector<ModuleInput> modules;
    std::vector<ImplantInput> implants;  // the implants applied to the fit
    std::vector<BoosterInput> boosters;
};

struct ModuleResult
{
    int typeID = 0;
    int chargeID = 0;
    State state = State::Online;
    std::vector<std::pair<Attr, float>> attrs;  // every field of the module's layout, modified
    float rawCycleTime = 0;
    float reactivationDelay = 0;
    float reloadTime = 0;
    float averageCycleTime = 0;  // 0 when the module doesn't cycle
    float capUse = 0;            // GJ/s while active
    int numShots = 0;             // cycles before reload, 0 = never reloads
};

enum Layer
{
    Shield,
    Armor,
    Hull,
    LayerCount
};

struct FitResult
{
    // Ship attributes in fitschema::ShipAttrs field order.
    std::array<float, std::size(fitschema::ShipAttrs::kFields)> ship{};
    float cpuUsed = 0, powerUsed = 0, calibrationUsed = 0;
    std::array<float, LayerCount> hp{}, ehp{};  // ehp: uniform damage profile
    float capPeakRecharge = 0;  // GJ/s at 25% cap
    float capRecharge = 0;      // peak recharge + injectors
    float capUsed = 0;          // GJ/s used by active modules
    // Sustainable local repair (pyfa's sustainableTank), raw and effective HP/s.
    std::array<float, LayerCount> repair{}, effectiveRepair{};
    std::vector<ModuleResult> modules;  // in input order
    std::vector<std::string> warnings;

    float shipAttr(Attr a) const;
};

class Engine
{
public:
    // Keeps references to gd and character; they must outlive the engine.
    Engine(const GameData& gd, const Character& character);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    FitResult evaluate(const FitInput& fit) const;

    // Modifier rows compiled / skipped (unresolved, unsupported target, not goal-relevant).
    size_t compiledModifierCount() const;

    struct Impl;

private:
    friend class SearchPlan;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------- fit search

enum class Goal
{
    ShieldPassive,  // shield EHP
    ArmorPassive,   // armor EHP
    HullPassive,    // hull EHP
    ShieldActive,   // effective sustainable shield rep/s
    ArmorActive,    // effective sustainable armor rep/s
};

struct SearchSpec
{
    // Ship, mode, implants, boosters and pinned modules: the part every test shares.
    FitInput base;
    // Open slots, each with its candidate options. A test picks one option per slot
    // (or none). Chosen options come after the pinned modules, in slot order.
    std::vector<std::vector<ModuleInput>> slots;
};

struct TestResult
{
    float goal = 0;
    float cpuUsed = 0, powerUsed = 0, calibrationUsed = 0;
    float cpuOutput = 0, powerOutput = 0, calibrationOutput = 0;
    float capUsed = 0;      // GJ/s used by active modules
    float capRecharge = 0;  // peak recharge + injectors; cap stable (simplified) when capUsed <= capRecharge
};

TestResult toTestResult(const FitResult& r, Goal goal);

// Evaluates many variations of one fit quickly, with results identical to
// Engine::evaluate on the equivalent fit (see fitFor()).
//
// At construction every option of every slot is added to the fit at once and
// pyfa's evaluation order is recorded as a list of instructions (one modifier
// from one source onto its targets, or an effect handled in code). Instructions
// that can't depend on the choice - skills, ship bonuses, pinned modules, also
// their effects on the options - are executed once ("baked"). A test reads the
// items it doesn't write in place from the plan, copies the few it does, and
// replays only the remaining instructions.
class SearchPlan
{
public:
    // Keeps a reference to the engine; copies the spec.
    SearchPlan(const Engine& engine, const SearchSpec& spec);
    ~SearchPlan();
    SearchPlan(const SearchPlan&) = delete;
    SearchPlan& operator=(const SearchPlan&) = delete;

    // choice[i]: option index for slot i, or -1 to leave it empty. Thread-safe.
    TestResult evaluate(const int* choice, Goal goal) const;

    // The full fit a choice stands for.
    FitInput fitFor(const int* choice) const;

    struct Stats
    {
        size_t instructions = 0;  // recorded
        size_t dynamic = 0;       // replayed per test
        // Dynamic reads of an attribute that a baked instruction modifies later in
        // pyfa's order: pyfa would read the earlier value there. 0 means the plan
        // reproduces pyfa's order exactly.
        size_t orderSensitive = 0;
        size_t items = 0;              // in the plan (pinned + all options, incl. ship/charges/implants)
        size_t itemsWritten = 0;       // of those, written by replayed instructions: copied per test
        size_t modules = 0;
        size_t variableCycle = 0;      // modules whose cycle/cap values are recomputed per test
        size_t variableFitting = 0;    // modules whose cpu/power/calibration are re-read per test
    };
    Stats stats() const;

private:
    struct Data;
    std::unique_ptr<Data> d_;
};
}
