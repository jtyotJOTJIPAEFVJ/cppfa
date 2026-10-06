#include "FitEngine.h"

#include "PyMath.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace fitengine
{
namespace
{
constexpr int kMaxFields = 64;     // widest layout (checked at startup); field masks are 64-bit
constexpr int kMaxItems = 160;     // ship + mode + modules + their charges
constexpr int kMaxPenalties = 4096;
constexpr int kMaxDepth = 12;      // guards recursive reads (caps, chained amounts)
constexpr double kUniformShare = 0.25;  // uniform damage profile: 25% of each type

enum Context
{
    CtxShip,
    CtxSkill,
    CtxModule,
    CtxModuleCharge,
    CtxCount
};
constexpr const char* kContextNames[CtxCount] = {"ship", "skill", "module", "moduleCharge"};

enum class Op : uint8_t
{
    PreAssign,
    PreIncrease,
    Multiply,
    PostIncrease,
    Force,
};

enum class TargetSel : uint8_t
{
    Ship,           // the ship (incl. pyfa's fit.extraAttributes)
    Modules,        // every fitted module passing the filter
    ModuleCharges,  // the charge of every fitted module passing the filter
    Self,           // the effect's owner (for charge effects: the module)
    SelfCharge,     // the owner's loaded charge
};

enum class SourceSel : uint8_t
{
    Owner,
    Ship,
    OwnerCharge,
    Skill,  // a character skill's attribute
};

enum class ItemKind : uint8_t
{
    Ship,
    Mode,
    Module,
    Charge,
};

struct Term
{
    FilterTerm::Kind kind = FilterTerm::Kind::Skill;
    bool onCharge = false;
    int id = 0;          // skill typeID / eve attributeID
    std::string group;   // group name
};

struct Cond
{
    ShipCondition::Kind kind = ShipCondition::Kind::Skill;
    bool negate = false;
    int id = 0;  // skill typeID / eve attributeID
};

struct Mod
{
    Op op = Op::Multiply;
    bool boost = false;  // amount is a percentage
    bool penalized = false;
    uint8_t penaltyGroup = 0;
    TargetSel target = TargetSel::Ship;
    Attr targetAttr{};
    SourceSel source = SourceSel::Owner;
    int sourceEveAttr = 0;
    int sourceSkillID = 0;
    double factor = 1.0;
    bool invert = false;
    bool perLevel = false;
    int levelSkillID = 0;
    std::vector<std::vector<Term>> filter;
    std::vector<Cond> conds;
};

struct EffectInfo
{
    uint8_t runTime = 1;  // 0 early, 1 normal, 2 late
    bool passive = false, active = false, overheat = false, offline = false;
    std::array<std::vector<Mod>, CtxCount> mods;
};

struct Layout
{
    const Attr* fields = nullptr;
    int count = 0;
    std::array<int8_t, fitschema::kAttrCount> fieldOf{};  // Attr -> field index, -1 if absent
};

// Effects handled in code (pyfa computes these in Python we don't translate).
enum : int
{
    kEffShieldBoosting = 4,
    kEffCapBooster = 48,
    kEffStructureRepair = 26,
    kEffArmorRepair = 27,
    kEffRAH = 4928,
    kEffFueledShieldBoosting = 4936,
    kEffFueledArmorRepair = 5275,
};

struct Item
{
    const Type* type = nullptr;
    const Type* baseType = nullptr;  // mutated modules: the original item (pyfa merges its attributes)
    const Layout* layout = nullptr;
    const Group* group = nullptr;
    ItemKind kind = ItemKind::Module;
    int charge = -1;  // module: its charge item
    int owner = -1;   // charge: its module item
    int input = -1;   // module: index into FitInput::modules
    State state = State::Online;
    const std::vector<std::pair<int, double>>* mutators = nullptr;
    std::array<int, 6> reqSkills{};
    int reqCount = 0;

    double base[kMaxFields];
    double preAssign[kMaxFields];
    double intermediary[kMaxFields];  // pyfa itemModifiedAttributes[key] = v: replaces the base
    double preInc[kMaxFields];
    double mult[kMaxFields];
    double postInc[kMaxFields];
    double force[kMaxFields];
    uint64_t hasPreAssign = 0, hasIntermediary = 0, hasForce = 0, touched = 0, penalized = 0;

    uint32_t repEffects = 0;   // which repairer effects ran (bit per kRep*)
};

struct Penalty
{
    uint16_t item;
    uint8_t field;
    uint8_t group;
    double value;
};

enum : uint32_t
{
    kRepShield = 1,
    kRepArmor = 2,
    kRepArmorFueled = 4,
    kRepHull = 8,
};

struct Scratch
{
    std::array<Item, kMaxItems> items;
    int count = 0;
    std::array<Penalty, kMaxPenalties> pens;
    int penCount = 0;
    int ship = -1;
    int mode = -1;
    std::array<int, kMaxItems> modules;
    int moduleCount = 0;
};

uint8_t runTimeIndex(const std::string& rt)
{
    return rt == "early" ? 0 : rt == "late" ? 2 : 1;
}

bool hasWord(const std::string& list, const char* word)
{
    size_t start = 0;
    while (start <= list.size())
    {
        size_t end = list.find('|', start);
        if (list.compare(start, (end == std::string::npos ? list.size() : end) - start, word) == 0)
            return true;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return false;
}
}  // namespace

double FitResult::shipAttr(Attr a) const
{
    for (size_t i = 0; i < std::size(fitschema::ShipAttrs::kFields); ++i)
        if (fitschema::ShipAttrs::kFields[i] == a)
            return ship[i];
    return 0.0;
}

struct Engine::Impl
{
    const GameData& gd;
    const Character& ch;

    std::unordered_map<int, EffectInfo> effects;
    std::unordered_map<std::string, Attr> attrByName;
    std::unordered_map<int, Attr> attrByEve;
    std::array<int, fitschema::kAttrCount> capMin{}, capMax{};  // eve attributeIDs of cap attributes, 0 = none
    Layout shipLayout, emptyLayout;
    std::vector<Layout> layouts;
    std::unordered_map<int, const Layout*> layoutByGroup;
    std::vector<std::string> penaltyGroups{"default"};
    uint8_t preMulGroup = 0;  // the Reactive Armor Hardener's penalty group
    std::vector<const Type*> skills;
    size_t compiled = 0;

    // eve attribute IDs used by hand-written code
    int eveSpeed, eveDuration, eveDurationHighIsGood, eveReactivation, eveReloadTime, eveCapNeed, eveCapacity,
        eveVolume, eveChargeRate, eveCrystalsDamaged, eveHp, eveCrystalChance, eveCrystalDamage, eveShieldBonus,
        eveArmorDamage, eveStructureDamage, eveCapacitorBonus, eveChargedArmorMult, eveCpu, evePower, eveUpgradeCost,
        eveResistanceShift;
    std::array<int, 4> eveBurstDurations{};
    std::array<int, 4> eveArmorRes{}, eveShipArmorRes{};
    Attr attrCpu, attrPower, attrCpuOutput, attrPowerOutput;

    Impl(const GameData& g, const Character& c) : gd(g), ch(c)
    {
        for (size_t i = 0; i < fitschema::kAttrCount; ++i)
        {
            Attr a = static_cast<Attr>(i);
            attrByName[fitschema::kAttrName[i]] = a;
            attrByEve[fitschema::kAttrEveID[i]] = a;
            if (const Attribute* info = gd.attribute(fitschema::kAttrEveID[i]))
            {
                capMin[i] = info->minAttributeID.value_or(0);
                capMax[i] = info->maxAttributeID.value_or(0);
            }
        }
        auto makeLayout = [](const Attr* fields, int count) {
            if (count > kMaxFields)
                throw std::runtime_error("fit layout wider than kMaxFields; raise it");
            Layout l;
            l.fields = fields;
            l.count = count;
            l.fieldOf.fill(-1);
            for (int i = 0; i < count; ++i)
                l.fieldOf[static_cast<size_t>(fields[i])] = static_cast<int8_t>(i);
            return l;
        };
        shipLayout = makeLayout(fitschema::ShipAttrs::kFields,
                                static_cast<int>(std::size(fitschema::ShipAttrs::kFields)));
        emptyLayout = makeLayout(nullptr, 0);
        layouts.reserve(std::size(fitschema::kGroupLayouts));
        for (const auto& gl : fitschema::kGroupLayouts)
            layouts.push_back(makeLayout(gl.fields, gl.fieldCount));
        for (size_t i = 0; i < std::size(fitschema::kGroupLayouts); ++i)
            layoutByGroup[static_cast<int>(fitschema::kGroupLayouts[i].groupID)] = &layouts[i];

        auto eve = [&](const char* name) { return gd.attributeID(name); };
        eveSpeed = eve("speed");
        eveDuration = eve("duration");
        eveDurationHighIsGood = eve("durationHighisGood");
        eveBurstDurations = {eve("durationSensorDampeningBurstProjector"),
                             eve("durationTargetIlluminationBurstProjector"),
                             eve("durationECMJammerBurstProjector"),
                             eve("durationWeaponDisruptionBurstProjector")};
        eveReactivation = eve("moduleReactivationDelay");
        eveReloadTime = eve("reloadTime");
        eveCapNeed = eve("capacitorNeed");
        eveCapacity = eve("capacity");
        eveVolume = eve("volume");
        eveChargeRate = eve("chargeRate");
        eveCrystalsDamaged = eve("crystalsGetDamaged");
        eveHp = eve("hp");
        eveCrystalChance = eve("crystalVolatilityChance");
        eveCrystalDamage = eve("crystalVolatilityDamage");
        eveShieldBonus = eve("shieldBonus");
        eveArmorDamage = eve("armorDamageAmount");
        eveStructureDamage = eve("structureDamageAmount");
        eveChargedArmorMult = eve("chargedArmorDamageMultiplier");
        eveCpu = eve("cpu");
        evePower = eve("power");
        eveUpgradeCost = eve("upgradeCost");
        eveResistanceShift = eve("resistanceShiftAmount");
        eveCapacitorBonus = eve("capacitorBonus");
        const char* res[4] = {"Em", "Thermal", "Kinetic", "Explosive"};
        for (int i = 0; i < 4; ++i)
            eveArmorRes[i] = eveShipArmorRes[i] = eve((std::string("armor") + res[i] + "DamageResonance").c_str());
        attrCpu = attrByName.at("cpu");
        attrPower = attrByName.at("power");
        attrCpuOutput = attrByName.at("cpuOutput");
        attrPowerOutput = attrByName.at("powerOutput");

        compileEffects();
        preMulGroup = penaltyGroupID("preMul");
        for (const Type* s : ch.skills)
            if (!s->effects.empty())
                skills.push_back(s);
    }

    uint8_t penaltyGroupID(const std::string& name)
    {
        std::string n = name.empty() ? "default" : name;
        for (size_t i = 0; i < penaltyGroups.size(); ++i)
            if (penaltyGroups[i] == n)
                return static_cast<uint8_t>(i);
        penaltyGroups.push_back(n);
        return static_cast<uint8_t>(penaltyGroups.size() - 1);
    }

    std::optional<Mod> compile(const EffectModifier& m)
    {
        if (!m.isResolved() || m.sourceAttr.empty() || m.targetAttr.empty())
            return std::nullopt;
        auto ta = attrByName.find(m.targetAttr);
        if (ta == attrByName.end())
            return std::nullopt;  // not something any goal depends on

        Mod out;
        out.targetAttr = ta->second;
        if (m.domain == "ship" || m.domain == "extraAttributes")
            out.target = TargetSel::Ship;
        else if (m.domain == "modules")
            out.target = m.target == "charge" ? TargetSel::ModuleCharges : TargetSel::Modules;
        else if (m.domain == "self")
            out.target = m.target == "charge" ? TargetSel::SelfCharge : TargetSel::Self;
        else
            return std::nullopt;  // drones, fighters, implants, boosters, ...

        if (m.operation == "boost")
            out.op = Op::Multiply, out.boost = true;
        else if (m.operation == "multiply")
            out.op = Op::Multiply;
        else if (m.operation == "increase")
            out.op = m.position == "post" ? Op::PostIncrease : Op::PreIncrease;
        else if (m.operation == "force")
            out.op = Op::Force;
        else if (m.operation == "preAssign")
            out.op = Op::PreAssign;
        else
            return std::nullopt;
        out.penalized = m.stackingPenalized;
        out.penaltyGroup = penaltyGroupID(m.penaltyGroup);

        out.sourceEveAttr = gd.attributeID(m.sourceAttr);
        if (!out.sourceEveAttr)
            return std::nullopt;
        if (m.sourceOf.empty() || m.sourceOf == "owner")
            out.source = SourceSel::Owner;
        else if (m.sourceOf == "ship")
            out.source = SourceSel::Ship;
        else if (m.sourceOf == "charge")
            out.source = SourceSel::OwnerCharge;
        else if (m.sourceOf.rfind("skill:", 0) == 0)
            out.source = SourceSel::Skill, out.sourceSkillID = gd.skillID(m.sourceOf.substr(6));
        else
            return std::nullopt;

        out.factor = m.factor;
        out.invert = m.invert;
        out.perLevel = m.perLevel;
        out.levelSkillID = m.skill.empty() ? 0 : gd.skillID(m.skill);

        for (const auto& clause : m.filter)
        {
            std::vector<Term> terms;
            for (const FilterTerm& ft : clause)
            {
                Term t;
                t.kind = ft.kind;
                t.onCharge = ft.onCharge;
                if (ft.kind == FilterTerm::Kind::Skill)
                    t.id = gd.skillID(ft.value);
                else if (ft.kind == FilterTerm::Kind::HasAttr)
                    t.id = gd.attributeID(ft.value);
                else if (ft.kind == FilterTerm::Kind::Group)
                    t.group = ft.value;
                terms.push_back(std::move(t));
            }
            out.filter.push_back(std::move(terms));
        }
        for (const ShipCondition& sc : m.conditions)
        {
            Cond c;
            c.kind = sc.kind;
            c.negate = sc.negate;
            c.id = sc.kind == ShipCondition::Kind::Skill ? gd.skillID(sc.value) : gd.attributeID(sc.value);
            out.conds.push_back(c);
        }
        return out;
    }

    void compileEffects()
    {
        for (const auto& [eid, rows] : gd.allEffectModifiers())
        {
            EffectInfo& info = effects[eid];
            for (const EffectModifier& m : rows)
            {
                info.runTime = runTimeIndex(m.runTime);
                info.passive = hasWord(m.effectType, "passive");
                info.active = hasWord(m.effectType, "active");
                info.overheat = hasWord(m.effectType, "overheat");
                info.offline = hasWord(m.effectType, "offline");
                for (int ctx = 0; ctx < CtxCount; ++ctx)
                {
                    if (!m.appliesInContext(kContextNames[ctx]))
                        continue;
                    if (auto mod = compile(m))
                    {
                        info.mods[ctx].push_back(std::move(*mod));
                        ++compiled;
                    }
                }
            }
        }
        // Effects handled in code that may have no table rows.
        auto ensure = [&](int eid, uint8_t runTime) {
            EffectInfo& info = effects[eid];
            if (!info.passive && !info.active && !info.overheat && !info.offline)
            {
                info.active = true;
                info.runTime = runTime;
            }
        };
        ensure(kEffCapBooster, 1);
        for (int eid : {kEffShieldBoosting, kEffStructureRepair, kEffArmorRepair, kEffFueledShieldBoosting,
                        kEffFueledArmorRepair, kEffRAH})
            ensure(eid, 2);
    }

    // ------------------------------------------------------------ item setup

    int addItem(Scratch& s, const Type* type, const Layout* layout, ItemKind kind) const
    {
        if (s.count >= kMaxItems)
            throw std::runtime_error("too many items in fit");
        int idx = s.count++;
        Item& it = s.items[idx];
        it = Item{};
        it.type = type;
        it.layout = layout;
        it.group = gd.groupOf(*type);
        it.kind = kind;
        auto req = gd.requiredSkills(*type);
        it.reqCount = static_cast<int>(std::min(req.size(), it.reqSkills.size()));
        std::copy_n(req.begin(), it.reqCount, it.reqSkills.begin());
        return idx;
    }

    void initValues(Item& it) const
    {
        for (int f = 0; f < it.layout->count; ++f)
        {
            int eve = fitschema::kAttrEveID[static_cast<size_t>(it.layout->fields[f])];
            it.base[f] = baseOf(it, eve);
            it.preInc[f] = 0;
            it.mult[f] = 1;
            it.postInc[f] = 0;
        }
    }

    // The item type's own attribute; for mutated modules merged over the base item
    // (pyfa getItemWithBaseItemAttribute: {**base.attributes, **item.attributes}).
    static std::optional<double> typeAttr(const Item& it, int eve)
    {
        if (auto v = it.type->attr(eve))
            return v;
        return it.baseType ? it.baseType->attr(eve) : std::nullopt;
    }

    // Unmodified value: abyssal mutator, else the type's value, else the attribute default.
    double baseOf(const Item& it, int eve) const
    {
        if (it.mutators)
            for (const auto& [id, v] : *it.mutators)
                if (id == eve)
                    return v;
        if (auto v = typeAttr(it, eve))
            return *v;
        const Attribute* a = gd.attribute(eve);
        return a ? a->defaultValue : 0.0;
    }

    bool hasBase(const Item& it, int eve) const
    {
        if (it.mutators)
            for (const auto& [id, v] : *it.mutators)
                if (id == eve)
                    return true;
        return typeAttr(it, eve).has_value();
    }

    // ------------------------------------------------------------ attribute reads

    int fieldOf(const Item& it, int eve) const
    {
        auto a = attrByEve.find(eve);
        return a == attrByEve.end() ? -1 : it.layout->fieldOf[static_cast<size_t>(a->second)];
    }

    // pyfa getModifiedItemAttr(attr): modified value, or the attribute's dgmattribs
    // default when the item lacks it (eve.db has no NULL defaults, so never None).
    double read(Scratch& s, int item, int eve, int depth) const
    {
        const Item& it = s.items[item];
        int f = fieldOf(it, eve);
        return f >= 0 ? value(s, item, f, depth) : baseOf(it, eve);
    }

    bool hasAttrOrModified(const Scratch& s, int item, int eve) const
    {
        const Item& it = s.items[item];
        int f = fieldOf(it, eve);
        return hasBase(it, eve) || (f >= 0 && (it.touched >> f & 1));
    }

    // pyfa ModifiedAttributeDict.__getitem__ / __calculateValue
    double value(Scratch& s, int item, int field, int depth) const
    {
        const Item& it = s.items[item];
        // Attributes no modifier touched never reach __calculateValue: pyfa returns
        // the intermediary or original value as is - no caps, no cpu/power rounding.
        if (!(it.touched >> field & 1))
            return (it.hasIntermediary >> field & 1) ? it.intermediary[field] : it.base[field];
        double val;
        if (it.hasForce >> field & 1)
            val = it.force[field];
        else
        {
            // priority: intermediary > preAssign > original
            val = (it.hasIntermediary >> field & 1) ? it.intermediary[field]
                  : (it.hasPreAssign >> field & 1) ? it.preAssign[field]
                                                    : it.base[field];
            val += it.preInc[field];
            val *= it.mult[field];
            if (it.penalized >> field & 1)
                val = applyPenalties(s, item, field, val);
            val += it.postInc[field];
        }
        Attr a = it.layout->fields[field];
        size_t ai = static_cast<size_t>(a);
        if (depth < kMaxDepth)
        {
            if (capMin[ai])
                val = std::max(val, read(s, item, capMin[ai], depth + 1));
            if (capMax[ai])
                val = std::min(val, read(s, item, capMax[ai], depth + 1));
        }
        if (a == attrCpu || a == attrPower || a == attrCpuOutput || a == attrPowerOutput)
            val = pymath::round(val, 2);
        return val;
    }

    // Stacking penalties: each group separately, bonuses and maluses separately,
    // strongest first: 1 + (m - 1) * exp(-i^2 / 7.1289).
    double applyPenalties(const Scratch& s, int item, int field, double val) const
    {
        uint64_t groupsSeen = 0;
        for (int p = 0; p < s.penCount; ++p)
        {
            const Penalty& pen = s.pens[p];
            if (pen.item != item || pen.field != field || (groupsSeen >> pen.group & 1u))
                continue;
            groupsSeen |= uint64_t{1} << pen.group;
            double up[64], down[64];
            int nUp = 0, nDown = 0;
            for (int q = p; q < s.penCount; ++q)
            {
                const Penalty& o = s.pens[q];
                if (o.item != item || o.field != field || o.group != pen.group)
                    continue;
                if (o.value > 1 && nUp < 64)
                    up[nUp++] = o.value;
                else if (o.value < 1 && nDown < 64)
                    down[nDown++] = o.value;
            }
            auto byStrength = [](double a, double b) { return std::abs(a - 1) > std::abs(b - 1); };
            std::stable_sort(up, up + nUp, byStrength);
            std::stable_sort(down, down + nDown, byStrength);
            for (int i = 0; i < nUp; ++i)
                val *= 1 + (up[i] - 1) * std::exp(-static_cast<double>(i * i) / 7.1289);
            for (int i = 0; i < nDown; ++i)
                val *= 1 + (down[i] - 1) * std::exp(-static_cast<double>(i * i) / 7.1289);
        }
        return val;
    }

    // ------------------------------------------------------------ applying modifiers

    void applyTo(Scratch& s, int item, const Mod& m, double amount) const
    {
        Item& it = s.items[item];
        int f = it.layout->fieldOf[static_cast<size_t>(m.targetAttr)];
        if (f < 0)
            return;
        uint64_t bit = uint64_t{1} << f;
        it.touched |= bit;
        switch (m.op)
        {
        case Op::PreAssign:
            it.preAssign[f] = amount;
            it.hasPreAssign |= bit;
            break;
        case Op::PreIncrease:
            it.preInc[f] += amount;
            break;
        case Op::PostIncrease:
            it.postInc[f] += amount;
            break;
        case Op::Force:
            it.force[f] = amount;
            it.hasForce |= bit;
            break;
        case Op::Multiply:
        {
            double mult = m.boost ? 1.0 + amount / 100.0 : amount;
            if (m.penalized)
            {
                if (s.penCount >= kMaxPenalties)
                    throw std::runtime_error("too many stacking-penalized modifiers");
                s.pens[s.penCount++] = {static_cast<uint16_t>(item), static_cast<uint8_t>(f), m.penaltyGroup, mult};
                it.penalized |= bit;
            }
            else
                it.mult[f] *= mult;
            break;
        }
        }
    }

    bool needsSkill(const Item& it, int skillID) const
    {
        return skillID && std::find(it.reqSkills.begin(), it.reqSkills.begin() + it.reqCount, skillID) !=
                              it.reqSkills.begin() + it.reqCount;
    }

    bool filterMatches(const Scratch& s, const Mod& m, int module, const Type* ownerSkill) const
    {
        const Item& mod = s.items[module];
        for (const auto& clause : m.filter)
        {
            bool any = false;
            for (const Term& t : clause)
            {
                int target = t.onCharge ? mod.charge : module;
                if (target < 0)
                    continue;
                const Item& it = s.items[target];
                switch (t.kind)
                {
                case FilterTerm::Kind::Skill:
                    any = needsSkill(it, t.id);
                    break;
                case FilterTerm::Kind::OwnerSkill:
                    any = ownerSkill && needsSkill(it, ownerSkill->id);
                    break;
                case FilterTerm::Kind::Group:
                    any = it.group && it.group->name == t.group;
                    break;
                case FilterTerm::Kind::HasAttr:
                    any = hasAttrOrModified(s, target, t.id);
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

    bool conditionsHold(const Scratch& s, const Mod& m, int owner) const
    {
        const Item& ship = s.items[s.ship];
        for (const Cond& c : m.conds)
        {
            bool holds = false;
            switch (c.kind)
            {
            case ShipCondition::Kind::Skill:
                holds = needsSkill(ship, c.id);
                break;
            case ShipCondition::Kind::HasAttr:
                holds = ship.type->attr(c.id).has_value();
                break;
            case ShipCondition::Kind::SelfBaseNonNeg:
            {
                auto v = owner >= 0 ? typeAttr(s.items[owner], c.id) : std::nullopt;
                holds = v && *v >= 0;
                break;
            }
            }
            if (holds == c.negate)
                return false;
        }
        return true;
    }

    std::optional<double> amountOf(Scratch& s, const Mod& m, int owner, const Type* ownerSkill) const
    {
        double v = 0;
        switch (m.source)
        {
        case SourceSel::Owner:
            if (owner >= 0)
                v = read(s, owner, m.sourceEveAttr, 0);
            else if (ownerSkill)
            {
                const Attribute* a = gd.attribute(m.sourceEveAttr);
                v = ownerSkill->attr(m.sourceEveAttr).value_or(a ? a->defaultValue : 0.0);
            }
            break;
        case SourceSel::Ship:
            v = read(s, s.ship, m.sourceEveAttr, 0);
            break;
        case SourceSel::OwnerCharge:
            if (owner < 0 || s.items[owner].charge < 0)
                return std::nullopt;  // pyfa: getModifiedChargeAttr() is None
            v = read(s, s.items[owner].charge, m.sourceEveAttr, 0);
            break;
        case SourceSel::Skill:
        {
            const Type* skill = gd.type(m.sourceSkillID);
            const Attribute* a = gd.attribute(m.sourceEveAttr);
            v = skill ? skill->attr(m.sourceEveAttr).value_or(a ? a->defaultValue : 0.0) : 0.0;
            break;
        }
        }
        double level = 1.0;
        if (m.perLevel && ownerSkill)
            level *= ch.skillLevel(ownerSkill->id);
        if (m.levelSkillID)
            level *= ch.skillLevel(m.levelSkillID);
        double value = v * level;
        return m.invert ? (value != 0 ? m.factor / value : 0.0) : m.factor * value;
    }

    void applyMod(Scratch& s, const Mod& m, int owner, const Type* ownerSkill) const
    {
        if (!conditionsHold(s, m, owner))
            return;
        auto amount = amountOf(s, m, owner, ownerSkill);
        if (!amount)
            return;
        switch (m.target)
        {
        case TargetSel::Ship:
            applyTo(s, s.ship, m, *amount);
            break;
        case TargetSel::Self:
            if (owner >= 0)
                applyTo(s, owner, m, *amount);
            break;
        case TargetSel::SelfCharge:
            if (owner >= 0 && s.items[owner].charge >= 0)
                applyTo(s, s.items[owner].charge, m, *amount);
            break;
        case TargetSel::Modules:
            for (int i = 0; i < s.moduleCount; ++i)
                if (filterMatches(s, m, s.modules[i], ownerSkill))
                    applyTo(s, s.modules[i], m, *amount);
            break;
        case TargetSel::ModuleCharges:
            for (int i = 0; i < s.moduleCount; ++i)
            {
                int mod = s.modules[i];
                if (s.items[mod].charge >= 0 && filterMatches(s, m, mod, ownerSkill))
                    applyTo(s, s.items[mod].charge, m, *amount);
            }
            break;
        }
    }

    void runMods(Scratch& s, const EffectInfo& info, Context ctx, int owner, const Type* ownerSkill) const
    {
        for (const Mod& m : info.mods[ctx])
            applyMod(s, m, owner, ownerSkill);
    }

    // ------------------------------------------------------------ effects handled in code

    void runSpecial(Scratch& s, int eid, int module) const
    {
        Item& it = s.items[module];
        // Weapon / cap booster effects also set module.reloadTime in pyfa, but
        // pyfa's reloadTime property only falls back to that when the
        // 'reloadTime' attribute is None - and it never is (dgmattribs default
        // 10000). Cap boosters' forceReload doesn't matter either: reload is
        // always factored in here. So none of those need handling.
        switch (eid)
        {
        case kEffCapBooster:
            // capAmount = charge capacitorBonus; module.itemModifiedAttributes['capacitorNeed'] = -capAmount
            if (it.charge >= 0)
            {
                int f = fieldOf(it, eveCapNeed);
                if (f >= 0)
                {
                    it.intermediary[f] = -read(s, it.charge, eveCapacitorBonus, 0);
                    it.hasIntermediary |= uint64_t{1} << f;
                }
            }
            break;
        case kEffShieldBoosting:
        case kEffFueledShieldBoosting:
            it.repEffects |= kRepShield;
            break;
        case kEffArmorRepair:
            it.repEffects |= kRepArmor;
            break;
        case kEffFueledArmorRepair:
            it.repEffects |= kRepArmorFueled;
            break;
        case kEffStructureRepair:
            it.repEffects |= kRepHull;
            break;
        case kEffRAH:
            reactiveArmorHardener(s, module);
            break;
        default:
            break;
        }
    }

    // Port of pyfa effect 4928 (adaptiveArmorHardener) for the uniform damage profile.
    void reactiveArmorHardener(Scratch& s, int module) const
    {
        double baseDamageTaken[4], rah[4];
        for (int i = 0; i < 4; ++i)
        {
            baseDamageTaken[i] = 25.0 * read(s, s.ship, eveShipArmorRes[i], 0);
            rah[i] = read(s, module, eveArmorRes[i], 0);
        }
        double shift = read(s, module, eveResistanceShift, 0) / 100.0;

        std::vector<std::array<double, 4>> cycles;
        int loopStart = -20;
        for (int num = 0; num < 50; ++num)
        {
            // In-game tie order: EM, Explosive, Kinetic, Thermal; sorted by damage taken (stable).
            struct T
            {
                int idx;
                double dmg, res;
            };
            T t[4] = {{0, baseDamageTaken[0] * rah[0], rah[0]},
                      {3, baseDamageTaken[3] * rah[3], rah[3]},
                      {2, baseDamageTaken[2] * rah[2], rah[2]},
                      {1, baseDamageTaken[1] * rah[1], rah[1]}};
            std::stable_sort(t, t + 4, [](const T& a, const T& b) { return a.dmg < b.dmg; });
            double c0, c1, c2, c3;
            if (t[2].dmg == 0)
            {
                c0 = 1 - t[0].res;
                c1 = 1 - t[1].res;
                c2 = 1 - t[2].res;
                c3 = -(c0 + c1 + c2);
            }
            else if (t[1].dmg == 0)
            {
                c0 = 1 - t[0].res;
                c1 = 1 - t[1].res;
                c2 = -(c0 + c1) / 2;
                c3 = -(c0 + c1) / 2;
            }
            else
            {
                c0 = std::min(shift, 1 - t[0].res);
                c1 = std::min(shift, 1 - t[1].res);
                c2 = -(c0 + c1) / 2;
                c3 = -(c0 + c1) / 2;
            }
            rah[t[0].idx] = t[0].res + c0;
            rah[t[1].idx] = t[1].res + c1;
            rah[t[2].idx] = t[2].res + c2;
            rah[t[3].idx] = t[3].res + c3;

            for (size_t i = 0; i < cycles.size(); ++i)
            {
                const auto& v = cycles[i];
                if (std::abs(rah[0] - v[0]) <= 1e-6 && std::abs(rah[1] - v[1]) <= 1e-6 &&
                    std::abs(rah[2] - v[2]) <= 1e-6 && std::abs(rah[3] - v[3]) <= 1e-6)
                {
                    loopStart = static_cast<int>(i);
                    break;
                }
            }
            if (loopStart >= 0)
                break;
            cycles.push_back({rah[0], rah[1], rah[2], rah[3]});
        }
        // Python slicing: cycleList[loopStart:], negative = last 20.
        size_t from = loopStart >= 0 ? static_cast<size_t>(loopStart)
                                     : (cycles.size() > 20 ? cycles.size() - 20 : 0);
        double avg[4] = {0, 0, 0, 0};
        for (size_t c = from; c < cycles.size(); ++c)
            for (int i = 0; i < 4; ++i)
                avg[i] += cycles[c][i];
        double n = static_cast<double>(cycles.size() - from);
        for (int i = 0; i < 4; ++i)
        {
            avg[i] = pymath::round(avg[i] / n, 3);
            // module.increaseItemAttr(attr, avg - current); ship.multiplyItemAttr(attr, avg, penalized 'preMul')
            Mod inc;
            inc.op = Op::PreIncrease;
            inc.targetAttr = attrByEve.at(eveArmorRes[i]);
            applyTo(s, module, inc, avg[i] - read(s, module, eveArmorRes[i], 0));
            Mod mul;
            mul.op = Op::Multiply;
            mul.penalized = true;
            mul.penaltyGroup = preMulGroup;
            mul.targetAttr = inc.targetAttr;
            applyTo(s, s.ship, mul, avg[i]);
        }
    }

    // ------------------------------------------------------------ evaluation

    const EffectInfo* effect(int eid) const
    {
        auto it = effects.find(eid);
        return it == effects.end() ? nullptr : &it->second;
    }

    static bool runsInState(const EffectInfo& e, State st)
    {
        return e.offline || (e.passive && st >= State::Online) || (e.active && st >= State::Active);
    }

    void run(Scratch& s) const
    {
        for (uint8_t pass = 0; pass < 3; ++pass)
        {
            for (const Type* skill : skills)
                for (int eid : skill->effects)
                    if (const EffectInfo* e = effect(eid); e && e->passive && e->runTime == pass)
                        runMods(s, *e, CtxSkill, -1, skill);

            for (int eid : s.items[s.ship].type->effects)
                if (const EffectInfo* e = effect(eid); e && e->passive && e->runTime == pass)
                    runMods(s, *e, CtxShip, s.ship, nullptr);

            for (int i = 0; i < s.moduleCount; ++i)
            {
                int mod = s.modules[i];
                const Item& it = s.items[mod];
                if (it.charge >= 0)
                    for (int eid : s.items[it.charge].type->effects)
                        if (const EffectInfo* e = effect(eid); e && e->runTime == pass && runsInState(*e, it.state))
                            runMods(s, *e, CtxModuleCharge, mod, nullptr);
                if (it.state >= State::Overheated)
                    for (int eid : it.type->effects)
                        if (const EffectInfo* e = effect(eid); e && e->overheat && e->runTime == pass)
                            runMods(s, *e, CtxModule, mod, nullptr);
                for (int eid : it.type->effects)
                    if (const EffectInfo* e = effect(eid); e && e->runTime == pass && runsInState(*e, it.state))
                    {
                        runMods(s, *e, CtxModule, mod, nullptr);
                        runSpecial(s, eid, mod);
                    }
            }

            if (s.mode >= 0)
                for (int eid : s.items[s.mode].type->effects)
                    if (const EffectInfo* e = effect(eid); e && e->runTime == pass)
                        runMods(s, *e, CtxModule, s.mode, nullptr);
        }
    }

    // pyfa Module.numShots
    int numShots(Scratch& s, int module) const
    {
        const Item& it = s.items[module];
        if (it.charge < 0)
            return 0;
        const Item& charge = s.items[it.charge];
        auto volume = charge.type->attr(eveVolume);
        auto capacity = typeAttr(it, eveCapacity);
        if (!volume || !capacity || *volume == 0)
            return 0;
        int numCharges = static_cast<int>(pymath::floatUnerr(*capacity / *volume));
        if (numCharges <= 0)
            return 0;
        if (hasAttrOrModified(s, module, eveChargeRate))
            return static_cast<int>(std::floor(numCharges / read(s, module, eveChargeRate, 0)));
        if (hasAttrOrModified(s, it.charge, eveCrystalsDamaged))
        {
            if (read(s, it.charge, eveCrystalsDamaged, 0) != 1)
                return 0;
            double hp = read(s, it.charge, eveHp, 0);
            double chance = read(s, it.charge, eveCrystalChance, 0);
            double damage = read(s, it.charge, eveCrystalDamage, 0);
            return static_cast<int>(std::floor(numCharges * hp / (damage * chance)));
        }
        return 0;
    }

    void moduleResult(Scratch& s, int module, ModuleResult& r) const
    {
        const Item& it = s.items[module];
        r.typeID = it.type->id;
        r.chargeID = it.charge >= 0 ? s.items[it.charge].type->id : 0;
        r.state = it.state;
        for (int f = 0; f < it.layout->count; ++f)
            r.attrs.emplace_back(it.layout->fields[f], value(s, module, f, 0));

        double raw = std::max({read(s, module, eveSpeed, 0), read(s, module, eveDuration, 0),
                               read(s, module, eveDurationHighIsGood, 0)});
        for (int eve : eveBurstDurations)
            raw = std::max(raw, read(s, module, eve, 0));
        r.rawCycleTime = raw;
        r.reactivationDelay = read(s, module, eveReactivation, 0);
        r.reloadTime = read(s, module, eveReloadTime, 0);
        r.numShots = numShots(s, module);

        // pyfa Module.getCycleParameters(), reload factored in (or forced by the effect).
        r.averageCycleTime = 0;
        if (raw > 0)
        {
            double inactive = r.reactivationDelay;
            double reload = r.reloadTime;
            bool infinite = r.numShots == 0;
            if (infinite || inactive >= reload)
                r.averageCycleTime = raw + inactive;
            else if (r.numShots == 1)
                r.averageCycleTime = raw + reload;
            else
            {
                double early = r.numShots - 1;
                r.averageCycleTime = (early * (raw + inactive) + (raw + reload)) / (early + 1);
            }
        }
        double capNeed = read(s, module, eveCapNeed, 0);
        r.capUse = (capNeed != 0 && it.state >= State::Active && r.averageCycleTime > 0)
                       ? capNeed / (r.averageCycleTime / 1000.0)
                       : 0.0;
    }

    FitResult evaluate(const FitInput& in) const
    {
        thread_local std::unique_ptr<Scratch> scratch;
        if (!scratch)
            scratch = std::make_unique<Scratch>();
        Scratch& s = *scratch;
        s.count = s.penCount = s.moduleCount = 0;
        s.mode = -1;
        FitResult out;

        const Type* shipType = gd.type(in.shipTypeID);
        if (!shipType)
            throw std::invalid_argument("unknown ship type " + std::to_string(in.shipTypeID));
        s.ship = addItem(s, shipType, &shipLayout, ItemKind::Ship);
        initValues(s.items[s.ship]);

        if (in.modeTypeID)
            if (const Type* mode = gd.type(in.modeTypeID))
            {
                auto l = layoutByGroup.find(mode->groupID);
                s.mode = addItem(s, mode, l == layoutByGroup.end() ? &emptyLayout : l->second, ItemKind::Mode);
                initValues(s.items[s.mode]);
            }

        for (size_t i = 0; i < in.modules.size(); ++i)
        {
            const ModuleInput& mi = in.modules[i];
            const Type* t = gd.type(mi.typeID);
            if (!t)
            {
                out.warnings.push_back("module type " + std::to_string(mi.typeID) + " unknown, skipped");
                continue;
            }
            auto l = layoutByGroup.find(t->groupID);
            int mod = addItem(s, t, l == layoutByGroup.end() ? &emptyLayout : l->second, ItemKind::Module);
            Item& it = s.items[mod];
            it.state = mi.state;
            it.baseType = mi.baseTypeID ? gd.type(mi.baseTypeID) : nullptr;
            it.input = static_cast<int>(i);
            it.mutators = mi.mutators.empty() ? nullptr : &mi.mutators;
            initValues(it);
            s.modules[s.moduleCount++] = mod;
            if (mi.chargeID)
            {
                if (const Type* c = gd.type(mi.chargeID))
                {
                    auto cl = layoutByGroup.find(c->groupID);
                    int charge = addItem(s, c, cl == layoutByGroup.end() ? &emptyLayout : cl->second,
                                         ItemKind::Charge);
                    initValues(s.items[charge]);
                    s.items[charge].owner = mod;
                    s.items[mod].charge = charge;
                }
                else
                    out.warnings.push_back("charge type " + std::to_string(mi.chargeID) + " unknown, skipped");
            }
        }

        run(s);

        // --- ship
        for (int f = 0; f < shipLayout.count; ++f)
            out.ship[static_cast<size_t>(f)] = value(s, s.ship, f, 0);

        // --- modules
        out.modules.resize(static_cast<size_t>(s.moduleCount));
        for (int i = 0; i < s.moduleCount; ++i)
            moduleResult(s, s.modules[i], out.modules[static_cast<size_t>(i)]);

        // --- fitting (pyfa getItemAttrOnlineSum)
        for (int i = 0; i < s.moduleCount; ++i)
        {
            int mod = s.modules[i];
            if (s.items[mod].state < State::Online)
                continue;
            out.cpuUsed += read(s, mod, eveCpu, 0);
            out.powerUsed += read(s, mod, evePower, 0);
            out.calibrationUsed += read(s, mod, eveUpgradeCost, 0);
        }
        out.cpuUsed = pymath::round(out.cpuUsed, 2);
        out.powerUsed = pymath::round(out.powerUsed, 2);

        // --- capacitor (pyfa __generateDrain + calculateCapRecharge)
        double capacity = out.shipAttr(Attr::capacitorCapacity);
        double rechargeSeconds = out.shipAttr(Attr::rechargeRate) / 1000.0;
        out.capPeakRecharge = 10.0 / rechargeSeconds * std::sqrt(0.25) * (1 - std::sqrt(0.25)) * capacity;
        double capAdded = 0;
        for (int i = 0; i < s.moduleCount; ++i)
        {
            const ModuleResult& r = out.modules[static_cast<size_t>(i)];
            if (r.state < State::Active || read(s, s.modules[i], eveCapNeed, 0) == 0)
                continue;
            if (r.rawCycleTime + r.reactivationDelay <= 0)
                continue;
            if (r.capUse > 0)
                out.capUsed += r.capUse;
            else
                capAdded -= r.capUse;
        }
        out.capRecharge = capAdded + out.capPeakRecharge;

        // --- tank
        static const Attr hpAttr[LayerCount] = {Attr::shieldCapacity, Attr::armorHP, Attr::hp};
        static const Attr resAttr[LayerCount][4] = {
            {Attr::shieldEmDamageResonance, Attr::shieldThermalDamageResonance, Attr::shieldKineticDamageResonance,
             Attr::shieldExplosiveDamageResonance},
            {Attr::armorEmDamageResonance, Attr::armorThermalDamageResonance, Attr::armorKineticDamageResonance,
             Attr::armorExplosiveDamageResonance},
            {Attr::emDamageResonance, Attr::thermalDamageResonance, Attr::kineticDamageResonance,
             Attr::explosiveDamageResonance}};
        std::array<double, LayerCount> divider{};
        for (int l = 0; l < LayerCount; ++l)
        {
            for (Attr a : resAttr[l])
                divider[l] += kUniformShare * out.shipAttr(a);
            if (divider[l] == 0)
                divider[l] = 1;
            out.hp[l] = out.shipAttr(hpAttr[l]);
            out.ehp[l] = out.hp[l] / divider[l];
        }
        sustainableTank(s, out);
        for (int l = 0; l < LayerCount; ++l)
            out.effectiveRepair[l] = out.repair[l] / divider[l];
        return out;
    }

    // Port of pyfa Fit.calculateSustainableTank (local repairers only, reload factored).
    void sustainableTank(Scratch& s, FitResult& out) const
    {
        struct Rep
        {
            int module;  // index into s.modules / out.modules
            int layer;
            double amount;
        };
        std::array<double, LayerCount> extra{}, adjust{};
        std::vector<Rep> contributors[LayerCount];
        for (int i = 0; i < s.moduleCount; ++i)
        {
            int mod = s.modules[i];
            const Item& it = s.items[mod];
            if (!it.repEffects)
                continue;
            double speed = read(s, mod, eveDuration, 0) / 1000.0;
            if (it.repEffects & kRepShield)
            {
                double rps = read(s, mod, eveShieldBonus, 0) / speed;
                extra[Shield] += rps;
                if (rps != 0)
                    contributors[Shield].push_back({i, Shield, read(s, mod, eveShieldBonus, 0)});
            }
            if (it.repEffects & (kRepArmor | kRepArmorFueled))
            {
                bool paste = (it.repEffects & kRepArmorFueled) && it.charge >= 0 &&
                             s.items[it.charge].type->name == "Nanite Repair Paste";
                double rps = read(s, mod, eveArmorDamage, 0) * (paste ? 3 : 1) / speed;
                extra[Armor] += rps;
                if (rps != 0)
                    contributors[Armor].push_back({i, Armor, read(s, mod, eveArmorDamage, 0)});
            }
            if (it.repEffects & kRepHull)
            {
                double rps = read(s, mod, eveStructureDamage, 0) / speed;
                extra[Hull] += rps;
                if (rps != 0)
                    contributors[Hull].push_back({i, Hull, read(s, mod, eveStructureDamage, 0)});
            }
        }

        auto groupIs = [&](int i, const char* name) {
            const Group* g = s.items[s.modules[i]].group;
            return g && g->name == name;
        };
        auto pasteMultiplier = [&](int i) {
            int mod = s.modules[i];
            int charge = s.items[mod].charge;
            if (charge < 0 || s.items[charge].type->name != "Nanite Repair Paste")
                return 1.0;
            double m = read(s, mod, eveChargedArmorMult, 0);
            return m ? m : 1.0;
        };

        double capUsed = out.capUsed;
        std::vector<Rep> repairers;
        for (int layer = 0; layer < LayerCount; ++layer)
        {
            for (const Rep& rep : contributors[layer])
            {
                const ModuleResult& r = out.modules[static_cast<size_t>(rep.module)];
                bool hasCharge = s.items[s.modules[rep.module]].charge >= 0;
                double cycle = r.rawCycleTime / 1000.0;
                bool usesCap = r.capUse != 0;
                if (usesCap)
                    capUsed -= r.capUse;
                if (usesCap && !hasCharge)
                {
                    adjust[layer] -= rep.amount / cycle;
                    repairers.push_back(rep);
                }
                else if (usesCap && hasCharge)
                {
                    adjust[layer] -= rep.amount * pasteMultiplier(rep.module) / cycle;
                    repairers.push_back(rep);
                }
                else if (!usesCap && (groupIs(rep.module, "Ancillary Shield Booster") ||
                                      groupIs(rep.module, "Ancillary Remote Shield Booster")))
                {
                    double reload = hasCharge ? r.reloadTime : 0.0;
                    double offDuty = reload / (std::max(r.numShots, 1) * r.rawCycleTime + reload);
                    adjust[layer] -= rep.amount * offDuty / cycle;
                }
            }
        }

        // Most cap-efficient first (Python sort is stable, also with reverse=True).
        auto efficiency = [&](const Rep& rep) {
            int mod = s.modules[rep.module];
            double mult = read(s, mod, eveChargedArmorMult, 0);
            return rep.amount * (mult ? mult : 1.0) / read(s, mod, eveCapNeed, 0);
        };
        std::stable_sort(repairers.begin(), repairers.end(),
                         [&](const Rep& a, const Rep& b) { return efficiency(a) > efficiency(b); });

        double peak = out.capRecharge;
        for (const Rep& rep : repairers)
        {
            if (capUsed > peak)
                break;
            const ModuleResult& r = out.modules[static_cast<size_t>(rep.module)];
            bool hasCharge = s.items[s.modules[rep.module]].charge >= 0;
            double reload = hasCharge ? r.reloadTime : 0.0;
            double cycle = r.rawCycleTime;
            double capPerSec = r.capUse;
            double sustainability = std::min(1.0, (peak - capUsed) / capPerSec);
            if (!hasCharge)
                adjust[rep.layer] += sustainability * rep.amount / (cycle / 1000.0);
            else
            {
                double shots = std::max(r.numShots, 1) * cycle;
                double onDuty = shots / (shots + reload);
                adjust[rep.layer] += sustainability * rep.amount * onDuty * pasteMultiplier(rep.module) /
                                     (cycle / 1000.0);
            }
            capUsed += capPerSec;
        }
        for (int l = 0; l < LayerCount; ++l)
            out.repair[l] = extra[l] + adjust[l];
    }
};

Engine::Engine(const GameData& gd, const Character& character) : impl_(std::make_unique<Impl>(gd, character)) {}
Engine::~Engine() = default;

FitResult Engine::evaluate(const FitInput& fit) const
{
    return impl_->evaluate(fit);
}

size_t Engine::compiledModifierCount() const
{
    return impl_->compiled;
}
}
