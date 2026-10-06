#pragma once

// Fit evaluation engine: reproduces pyfa's calculation for the attributes the
// tank / fitting / capacitor goals need (see tools/goal_closure.py), using the
// generated compact layouts (src/generated/FitSchema.generated.h).
//
// Sources supported: ship, character skills, modules (with state), charges,
// T3 destroyer modes, the fit's own command bursts. Not yet: implants,
// boosters, bursts from other fits, system effects, projected effects,
// drones/fighters.
//
// Matches pyfa with: All-5 style character, uniform damage profile, reload
// factored into cycle times.
//
// Evaluation keeps pyfa's order (early/normal/late passes; skills, ship,
// modules, mode) and lazy attribute reads, so effects that read attributes
// other effects modify see the same values pyfa does. Values are computed in
// double to match pyfa; the float layouts are the storage format for search tables.

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
    std::vector<std::pair<int, double>> mutators;  // eve attributeID -> value
};

struct FitInput
{
    int shipTypeID = 0;
    int modeTypeID = 0;  // T3 destroyer mode, 0 = none
    std::vector<ModuleInput> modules;
};

struct ModuleResult
{
    int typeID = 0;
    int chargeID = 0;
    State state = State::Online;
    std::vector<std::pair<Attr, double>> attrs;  // every field of the module's layout, modified
    double rawCycleTime = 0;
    double reactivationDelay = 0;
    double reloadTime = 0;
    double averageCycleTime = 0;  // 0 when the module doesn't cycle
    double capUse = 0;            // GJ/s while active
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
    std::array<double, std::size(fitschema::ShipAttrs::kFields)> ship{};
    double cpuUsed = 0, powerUsed = 0, calibrationUsed = 0;
    std::array<double, LayerCount> hp{}, ehp{};  // ehp: uniform damage profile
    double capPeakRecharge = 0;  // GJ/s at 25% cap
    double capRecharge = 0;      // peak recharge + injectors
    double capUsed = 0;          // GJ/s used by active modules
    // Sustainable local repair (pyfa's sustainableTank), raw and effective HP/s.
    std::array<double, LayerCount> repair{}, effectiveRepair{};
    std::vector<ModuleResult> modules;  // in input order
    std::vector<std::string> warnings;

    double shipAttr(Attr a) const;
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
