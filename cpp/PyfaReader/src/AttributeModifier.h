#pragma once

// Applies the modifiers of a fit's sources (ship, character skills, and later
// implants, boosters, modules, system effects, ...) to attribute values,
// following pyfa's ModifiedAttributeDict calculation order:
//
//   forced value wins outright; otherwise
//   (preAssigned or given base value + pre-increases) * multipliers
//   * stacking-penalized multipliers (per penalty group) + post-increases
//
// then clamped by the attribute's min/max cap attributes (dgmattribs
// min/maxAttributeID) and, for cpu/power(Output), rounded to 2 decimals.
//
// Modifiers whose amount depends on runtime state (pilot security status,
// cloaked, ...; see EffectModifier::isResolved) are skipped.

#include "GameData.h"

#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// Where a modifier comes from. Mirrors the context pyfa passes to effect handlers.
enum class SourceKind
{
    Ship,
    Skill,
    Implant,       // not implemented yet
    Booster,       // not implemented yet
    Module,        // not implemented yet (includes subsystems, rigs, T3D modes)
    Drone,         // not implemented yet
    Fighter,       // not implemented yet
    SystemEffect,  // not implemented yet (wormhole / incursion / ... effects)
    CommandBurst,  // not implemented yet (warfare buffs, separate mechanism in pyfa)
};

// What kind of thing an attribute belongs to.
enum class TargetKind
{
    Ship,
    Module,
    Charge,   // a charge loaded in a module
    Drone,
    Fighter,
    Implant,
    Booster,
};
// Fit-wide values (maxActiveDrones, maxTargetsLockedFromSkills, ...) are ship
// attributes: pyfa's fit.extraAttributes is the ship's attribute dict.

struct ModifierTarget
{
    TargetKind kind = TargetKind::Ship;
    const Type* item = nullptr;    // the item whose attribute is computed (unused for Ship/Fit)
    const Type* module = nullptr;  // Charge: the module holding the charge
    const Type* charge = nullptr;  // Module: the charge loaded in it (for charge filters)
};

// One modifier from one source, ready to apply.
struct AppliedModifier
{
    const EffectModifier* modifier = nullptr;
    SourceKind source = SourceKind::Ship;
    const Type* sourceType = nullptr;  // the ship / skill / ... owning the effect
    TargetKind target = TargetKind::Ship;
    float levelMultiplier = 1.0;      // owner level (perLevel) * level of modifier->skill
};

class NotImplementedError : public std::logic_error
{
public:
    using std::logic_error::logic_error;
};

class AttributeModifier
{
public:
    // Keeps references to gd and character (and the ship's Type); they must outlive this object.
    AttributeModifier(const GameData& gd, const Ship& ship, const Character& character);

    // Future sources. Each throws NotImplementedError for now.
    void addImplant(const Type& implant);
    void addBooster(const Type& booster);
    void addModule(const Type& module);
    void addDrone(const Type& drone);
    void addFighter(const Type& fighter);
    void addSystemEffect(const Type& effectBeacon);
    void addCommandBurst(const Type& burstCharge);

    // Ship attribute: takes the attribute name and its unmodified value.
    float modify(const std::string& attr, float value) const;
    float modify(const ModifierTarget& target, const std::string& attr, float value) const;

    // Convenience wrappers around modify(ModifierTarget, ...).
    float modifyModule(const Type& module, const std::string& attr, float value, const Type* charge = nullptr) const;
    float modifyCharge(const Type& charge, const Type& module, const std::string& attr, float value) const;
    float modifyDrone(const Type& drone, const std::string& attr, float value) const;
    float modifyFighter(const Type& fighter, const std::string& attr, float value) const;

    // The modifiers that change `attr` on `target` (for "affected by" displays).
    std::vector<const AppliedModifier*> modifiersFor(const ModifierTarget& target, const std::string& attr) const;
    // Amount a modifier applies (percent for boost, factor for multiply, ...).
    float amount(const AppliedModifier& m) const;

    // Modifiers left out because they target something unsupported or depend
    // on runtime state the generator couldn't resolve.
    const std::vector<AppliedModifier>& skipped() const { return skipped_; }

private:
    void addSource(SourceKind kind, const Type& source, int ownerLevel);
    bool conditionsHold(const EffectModifier& m, const Type& source) const;
    bool matches(const AppliedModifier& m, const ModifierTarget& target) const;
    float modifyImpl(const ModifierTarget& target, const std::string& attr, float value, int depth) const;
    float calculate(const ModifierTarget& target, const std::string& attr, float value, int depth) const;
    float finish(const ModifierTarget& target, const std::string& attr, float value, int depth) const;
    float amountImpl(const AppliedModifier& m, int depth) const;

    const GameData& gd_;
    const Type* ship_;
    const Character& character_;
    std::unordered_map<std::string, std::vector<AppliedModifier>> byAttr_;  // targetAttr -> modifiers
    std::vector<AppliedModifier> skipped_;
};
