#include "AttributeModifier.h"

#include "PyMath.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>

namespace
{
// Context name pyfa passes to handlers for each source kind.
const char* contextName(SourceKind kind)
{
    switch (kind)
    {
    case SourceKind::Ship: return "ship";
    case SourceKind::Skill: return "skill";
    case SourceKind::Implant: return "implant";
    case SourceKind::Booster: return "booster";
    case SourceKind::Module: return "module";
    case SourceKind::Drone: return "drone";
    case SourceKind::Fighter: return "fighter";
    case SourceKind::SystemEffect: return "projected+module";
    case SourceKind::CommandBurst: return "";
    }
    return "";
}

// Maps an effect row's domain/target to what it modifies; false if unsupported.
bool targetKindOf(const EffectModifier& m, SourceKind source, TargetKind& out)
{
    if (m.domain == "self")
    {
        if (source != SourceKind::Ship)
            return false;  // module/charge self-modification comes with module support
        out = TargetKind::Ship;
    }
    else if (m.domain == "ship")
        out = TargetKind::Ship;
    else if (m.domain == "modules")
        out = m.target == "charge" ? TargetKind::Charge : TargetKind::Module;
    else if (m.domain == "drones")
        out = TargetKind::Drone;
    else if (m.domain == "fighters")
        out = TargetKind::Fighter;
    else if (m.domain == "implants")
        out = TargetKind::Implant;
    else if (m.domain == "boosters")
        out = TargetKind::Booster;
    else if (m.domain == "extraAttributes")
        out = TargetKind::Ship;  // pyfa's fit.extraAttributes *is* the ship's attribute dict
    else
        return false;  // e.g. the effect's own item ('module', 'container'), character, projected targets
    return true;
}

constexpr int kMaxDepth = 8;  // guards against cycles when amounts depend on modified attributes

double roundTo2(double v) { return pymath::round(v, 2); }
}

AttributeModifier::AttributeModifier(const GameData& gd, const Ship& ship, const Character& character)
    : gd_(gd), ship_(ship.type), character_(character)
{
    addSource(SourceKind::Ship, *ship.type, 0);
    for (const Type* skill : character.skills)
        addSource(SourceKind::Skill, *skill, character.skillLevel(skill->id));
}

void AttributeModifier::addImplant(const Type&) { throw NotImplementedError("implants are not supported yet"); }
void AttributeModifier::addBooster(const Type&) { throw NotImplementedError("boosters are not supported yet"); }
void AttributeModifier::addModule(const Type&) { throw NotImplementedError("module effects are not supported yet"); }
void AttributeModifier::addDrone(const Type&) { throw NotImplementedError("drone effects are not supported yet"); }
void AttributeModifier::addFighter(const Type&) { throw NotImplementedError("fighter effects are not supported yet"); }
void AttributeModifier::addSystemEffect(const Type&) { throw NotImplementedError("system effects are not supported yet"); }
void AttributeModifier::addCommandBurst(const Type&) { throw NotImplementedError("command bursts are not supported yet"); }

void AttributeModifier::addSource(SourceKind kind, const Type& source, int ownerLevel)
{
    const std::string context = contextName(kind);
    for (int effect : source.effects)
    {
        for (const EffectModifier& m : gd_.effectModifiers(effect))
        {
            if (!m.appliesInContext(context))
                continue;

            AppliedModifier a;
            a.modifier = &m;
            a.source = kind;
            a.sourceType = &source;
            if (m.perLevel)
                a.levelMultiplier *= ownerLevel;
            if (!m.skill.empty())
                a.levelMultiplier *= character_.skillLevel(gd_.skillID(m.skill));

            // 'charge' sources need a loaded charge: comes with module support.
            if (!targetKindOf(m, kind, a.target) || !m.isResolved() || m.sourceAttr.empty() || m.targetAttr.empty() ||
                m.sourceOf == "charge")
            {
                skipped_.push_back(a);
                continue;
            }
            if (!conditionsHold(m, source))
                continue;
            byAttr_[m.targetAttr].push_back(a);
        }
    }
}

bool AttributeModifier::conditionsHold(const EffectModifier& m, const Type& source) const
{
    for (const ShipCondition& c : m.conditions)
    {
        bool holds = false;
        switch (c.kind)
        {
        case ShipCondition::Kind::Skill:
            holds = gd_.requiresSkill(*ship_, gd_.skillID(c.value));
            break;
        case ShipCondition::Kind::HasAttr:
            holds = gd_.typeAttr(*ship_, c.value).has_value();
            break;
        case ShipCondition::Kind::SelfBaseNonNeg:
        {
            auto v = gd_.typeAttr(source, c.value);
            holds = v && *v >= 0;
            break;
        }
        }
        if (holds == c.negate)
            return false;
    }
    return true;
}

bool AttributeModifier::matches(const AppliedModifier& a, const ModifierTarget& target) const
{
    if (a.target != target.kind)
        return false;

    // Filters test the module (or drone, ...) unless the term says charge.
    const Type* item = target.kind == TargetKind::Charge ? target.module : target.item;
    const Type* charge = target.kind == TargetKind::Charge ? target.item : target.charge;

    for (const auto& clause : a.modifier->filter)
    {
        bool any = false;
        for (const FilterTerm& term : clause)
        {
            const Type* t = term.onCharge ? charge : item;
            if (!t)
                continue;
            switch (term.kind)
            {
            case FilterTerm::Kind::Skill:
                any = gd_.requiresSkill(*t, gd_.skillID(term.value));
                break;
            case FilterTerm::Kind::OwnerSkill:
                any = gd_.requiresSkill(*t, a.sourceType->id);
                break;
            case FilterTerm::Kind::Group:
            {
                const Group* g = gd_.groupOf(*t);
                any = g && g->name == term.value;
                break;
            }
            case FilterTerm::Kind::HasAttr:
                any = gd_.typeAttr(*t, term.value).has_value();
                break;
            }
            if (any)
                break;
        }
        if (!any)
            return false;
    }
    return true;
}

double AttributeModifier::amount(const AppliedModifier& m) const
{
    return amountImpl(m, 0);
}

double AttributeModifier::amountImpl(const AppliedModifier& a, int depth) const
{
    const EffectModifier& m = *a.modifier;
    const Attribute* attr = gd_.attribute(gd_.attributeID(m.sourceAttr));
    if (!attr)
        return 0.0;

    // Whose attribute holds the amount: usually the effect's owner.
    const Type* from = a.sourceType;
    bool fromShip = a.source == SourceKind::Ship;
    if (m.sourceOf == "ship")
    {
        from = ship_;
        fromShip = true;
    }
    else if (m.sourceOf.rfind("skill:", 0) == 0)
    {
        from = gd_.type(gd_.skillID(m.sourceOf.substr(6)));
        fromShip = false;
    }
    if (!from)
        return 0.0;

    double base = from->attr(attr->id).value_or(attr->defaultValue);
    // pyfa reads the *modified* attribute; for the ship that includes skill and
    // ship modifiers on the bonus attribute itself. (Nothing supported yet
    // modifies skill attributes.)
    if (fromShip && depth < kMaxDepth)
        base = modifyImpl(ModifierTarget{}, m.sourceAttr, base, depth + 1);
    double value = base * a.levelMultiplier;
    return m.invert ? (value != 0 ? m.factor / value : 0.0) : m.factor * value;
}

std::vector<const AppliedModifier*> AttributeModifier::modifiersFor(const ModifierTarget& target,
                                                                    const std::string& attr) const
{
    std::vector<const AppliedModifier*> out;
    auto it = byAttr_.find(attr);
    if (it != byAttr_.end())
        for (const AppliedModifier& a : it->second)
            if (matches(a, target))
                out.push_back(&a);
    return out;
}

double AttributeModifier::modify(const std::string& attr, double value) const
{
    return modifyImpl(ModifierTarget{}, attr, value, 0);
}

double AttributeModifier::modify(const ModifierTarget& target, const std::string& attr, double value) const
{
    return modifyImpl(target, attr, value, 0);
}

double AttributeModifier::modifyModule(const Type& module, const std::string& attr, double value, const Type* charge) const
{
    return modify({TargetKind::Module, &module, nullptr, charge}, attr, value);
}

double AttributeModifier::modifyCharge(const Type& charge, const Type& module, const std::string& attr, double value) const
{
    return modify({TargetKind::Charge, &charge, &module, nullptr}, attr, value);
}

double AttributeModifier::modifyDrone(const Type& drone, const std::string& attr, double value) const
{
    return modify({TargetKind::Drone, &drone, nullptr, nullptr}, attr, value);
}

double AttributeModifier::modifyFighter(const Type& fighter, const std::string& attr, double value) const
{
    return modify({TargetKind::Fighter, &fighter, nullptr, nullptr}, attr, value);
}

double AttributeModifier::modifyImpl(const ModifierTarget& target, const std::string& attr, double value, int depth) const
{
    return finish(target, attr, calculate(target, attr, value, depth), depth);
}

// Caps and rounding pyfa applies after the modifiers (also to forced values).
double AttributeModifier::finish(const ModifierTarget& target, const std::string& attr, double val, int depth) const
{
    if (const Attribute* a = gd_.attribute(gd_.attributeID(attr)); a && depth < kMaxDepth)
    {
        // The cap is the (modified) value of another attribute on the same item.
        const Type* item = target.kind == TargetKind::Ship ? ship_ : target.item;
        auto capValue = [&](int capID) -> std::optional<double> {
            const Attribute* cap = gd_.attribute(capID);
            if (!cap || !item)
                return std::nullopt;
            return modifyImpl(target, cap->name, item->attr(cap->id).value_or(cap->defaultValue), depth + 1);
        };
        if (a->minAttributeID)
            if (auto lo = capValue(*a->minAttributeID))
                val = std::max(val, *lo);
        if (a->maxAttributeID)
            if (auto hi = capValue(*a->maxAttributeID))
                val = std::min(val, *hi);
    }
    if (attr == "cpu" || attr == "power" || attr == "cpuOutput" || attr == "powerOutput")
        val = roundTo2(val);
    return val;
}

double AttributeModifier::calculate(const ModifierTarget& target, const std::string& attr, double value, int depth) const
{
    auto it = byAttr_.find(attr);
    if (it == byAttr_.end())
        return value;

    std::optional<double> forced, preAssigned;
    double preIncrease = 0.0, postIncrease = 0.0, multiplier = 1.0;
    std::map<std::string, std::vector<double>> penalized;  // penalty group -> multipliers

    for (const AppliedModifier& a : it->second)
    {
        if (!matches(a, target))
            continue;
        const EffectModifier& m = *a.modifier;
        double amt = amountImpl(a, depth);

        if (m.operation == "force")
            forced = amt;
        else if (m.operation == "preAssign")
            preAssigned = amt;
        else if (m.operation == "increase")
            (m.position == "post" ? postIncrease : preIncrease) += amt;
        else if (m.operation == "boost" || m.operation == "multiply")
        {
            double mult = m.operation == "boost" ? 1.0 + amt / 100.0 : amt;
            if (m.stackingPenalized)
                penalized[m.penaltyGroup.empty() ? "default" : m.penaltyGroup].push_back(mult);
            else
                multiplier *= mult;
        }
    }

    if (forced)
        return *forced;

    double val = preAssigned.value_or(value);
    val += preIncrease;
    val *= multiplier;
    // Each group is penalized independently; bonuses and penalties separately,
    // strongest first: 1 + (m - 1) * exp(-i^2 / 7.1289).
    for (auto& [group, mults] : penalized)
    {
        std::vector<double> bonuses, maluses;
        for (double m : mults)
            (m > 1 ? bonuses : maluses).push_back(m);
        for (auto* list : {&bonuses, &maluses})
        {
            std::stable_sort(list->begin(), list->end(),
                             [](double a, double b) { return std::abs(a - 1) > std::abs(b - 1); });
            for (size_t i = 0; i < list->size(); ++i)
                val *= 1 + ((*list)[i] - 1) * std::exp(-static_cast<double>(i * i) / 7.1289);
        }
    }
    val += postIncrease;
    return val;
}
