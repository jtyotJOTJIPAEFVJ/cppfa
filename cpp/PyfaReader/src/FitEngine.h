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

// Local repair effects a module ran (ModuleStats::repEffects).
enum RepEffect : uint8_t
{
    RepShield = 1,
    RepArmor = 2,
    RepArmorFueled = 4,
    RepHull = 8,
};

// One module's inputs to the fit totals: fitting (pyfa getItemAttrOnlineSum),
// capacitor (Module.capUse) and local repair (Fit.calculateSustainableTank).
// The engine computes its totals from these with the functions below; the
// optimizer combines stats it measured the same way, so both share the arithmetic.
struct ModuleStats
{
    State state = State::Online;
    float cpu = 0, power = 0, calibration = 0;  // 0 when offline
    float rawCycle = 0, reactivation = 0, reload = 0, averageCycle = 0;
    float capNeed = 0, capUse = 0;  // capUse: GJ/s while active (negative for cap boosters)
    int numShots = 0;
    // Local repairers (repEffects != 0)
    uint8_t repEffects = 0;
    bool ancillaryShield = false;
    bool hasCharge = false;
    bool pasteCharge = false;  // the loaded charge is Nanite Repair Paste
    float duration = 0;        // 'duration' attribute (ms)
    float shieldBonus = 0, armorDamage = 0, structureDamage = 0, chargedArmorMult = 0;
};

// pyfa __generateDrain: capacitor used by active modules and added by cap boosters.
struct CapSums
{
    float used = 0, added = 0;
};
CapSums capSums(const ModuleStats* modules, int count);

// pyfa calculateCapRecharge at 25% capacitor (GJ/s).
float capPeakRecharge(float capacity, float rechargeRateMs);

// pyfa calculateSustainableTank, split before the step that shares the capacitor
// between repairers, so that step can be rerun with extra capacitor use (e.g.
// hardeners added later) without the modules: repair(layer, x) is the fit's
// sustainable repair if x GJ/s more were used.
struct RepProfile
{
    static constexpr int kMaxReps = 32;
    struct Rep
    {
        double capUse;
        double amount;
        double onDuty, paste;  // charged repairers: on-duty fraction (reload), paste multiplier
        double cycleSeconds;
        bool hasCharge;
        uint8_t layer;
    };
    std::array<double, LayerCount> extra{}, adjust{};
    double capUsed = 0;      // without the cap-using repairers
    double capRecharge = 0;  // peak recharge + cap boosters
    int repCount = 0;
    std::array<Rep, kMaxReps> reps;  // most cap-efficient first

    float repair(int layer, double extraCapUse = 0) const;
    std::array<float, LayerCount> repairAll(double extraCapUse = 0) const;
};
RepProfile makeRepProfile(const ModuleStats* modules, int count, float capUsed, float capRecharge);

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

    // What a module type's effects (module and overheat contexts) or a charge type's
    // effects (moduleCharge context) can modify, as compiled: a cheap first filter
    // for modules that can't matter to any goal.
    struct Reach
    {
        bool ship = false;     // ship attributes
        bool others = false;   // other modules, their charges, implants, boosters
        bool self = false;     // the module itself or its charge
        bool special = false;  // effects handled in code (repairers, cap booster, RAH, bursts)
        bool activatable = false;  // has an active effect: fitted as Active
    };
    Reach reach(int typeID, bool asCharge = false) const;

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
    // Keep the recorded instruction graph so influence() can be queried.
    bool keepInfluence = false;
};

// What choosing one option can change, directly or through other modifiers
// (transitively), on the superset fit (every option present).
struct Influence
{
    uint64_t shipFields = 0;  // bit i: fitschema::ShipAttrs::kFields[i]
    // Fields of other options (global option index) and of pinned modules (index
    // into SearchSpec::base.modules), including their charges. Sorted, unique.
    std::vector<std::pair<int, Attr>> optionFields;
    std::vector<std::pair<int, Attr>> pinnedFields;
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

// How the engine computed one attribute of an item (pyfa ModifiedAttributeDict):
// value = caps(penalties((base + preInc) * mult) + post), where penalties applies
// pyfa's stacking penalties to the penalized multipliers (pens, in application
// order, with their penalty groups). base is the original, preAssigned or
// intermediary value; mult the product of the unpenalized multipliers.
struct AttrParts
{
    static constexpr int kMaxPens = 32;
    float value = 0;
    float base = 0, preInc = 0, mult = 1, post = 0;
    bool forced = false;     // a forced value: value only
    bool truncated = false;  // more than kMaxPens penalized multipliers
    int penCount = 0;
    std::array<float, kMaxPens> pens{};
    std::array<uint8_t, kMaxPens> groups{};
};

// Everything a test computes, for callers that build their own model of the fit
// from measurements (the optimizer).
struct TestDetail
{
    static constexpr int kMaxModules = 64;
    // Input: also fill resonanceParts (costs a little per test).
    bool wantResonanceParts = false;
    // The ship's resonance attributes per layer (EM, thermal, kinetic, explosive).
    std::array<std::array<AttrParts, 4>, LayerCount> resonanceParts{};
    std::array<float, std::size(fitschema::ShipAttrs::kFields)> ship{};  // ShipAttrs field order
    std::array<float, LayerCount> hp{}, divider{};  // divider: mean resonance (uniform damage); EHP = hp / divider
    float capPeak = 0;
    CapSums cap;
    // The fit's modules in evaluation order: pinned ones, then the chosen options
    // in slot order. moduleOption: global option index (see SearchPlan::optionIndex),
    // or -1 - i for pinned module i.
    int moduleCount = 0;
    std::array<ModuleStats, kMaxModules> modules;
    std::array<int, kMaxModules> moduleOption{};
};

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
    // detail (optional) receives the per-module stats and ship values.
    TestResult evaluate(const int* choice, Goal goal, TestDetail* detail = nullptr) const;

    // The full fit a choice stands for.
    FitInput fitFor(const int* choice) const;

    // Options numbered across slots: slot s's option o is optionIndex(s, o).
    int optionIndex(int slot, int option) const;
    int optionCount() const;

    // Requires SearchSpec::keepInfluence.
    Influence influence(int slot, int option) const;

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
