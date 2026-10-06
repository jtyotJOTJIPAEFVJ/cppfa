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
constexpr int widestLayout()
{
    size_t w = std::size(fitschema::ShipAttrs::kFields);
    for (const auto& gl : fitschema::kGroupLayouts)
        w = std::max<size_t>(w, gl.fieldCount);
    return static_cast<int>(w);
}
constexpr int kMaxFields = widestLayout();  // from the generated layouts
static_assert(kMaxFields <= 64, "field bit masks are 64-bit");
constexpr int kMaxItems = 160;     // ship + mode + modules + their charges
constexpr int kMaxPenalties = 4096;
constexpr int kMaxDepth = 12;      // guards recursive reads (caps, chained amounts)
constexpr float kUniformShare = 0.25f;  // uniform damage profile: 25% of each type

enum Context
{
    CtxShip,
    CtxSkill,
    CtxModule,
    CtxModuleCharge,
    CtxImplant,
    CtxBooster,
    CtxCount
};
constexpr const char* kContextNames[CtxCount] = {"ship", "skill", "module", "moduleCharge", "implant", "booster"};

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
    Implants,       // every applied implant passing the filter (pyfa fit.appliedImplants)
    Boosters,       // every booster passing the filter
};

enum class SourceSel : uint8_t
{
    Owner,
    Ship,
    OwnerCharge,
    Skill,  // a character skill's attribute
    Buff,   // a command burst buff's value (warfare buff table)
};

enum class ItemKind : uint8_t
{
    Ship,
    Mode,
    Module,
    Charge,
    Implant,
    Booster,
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
    float factor = 1.0f;
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
    bool sideEffect = false;  // booster side effect: runs only when switched on
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
    // moduleBonusWarfareLink{Armor,Shield,Skirmish,Info,Mining}: command bursts
    kEffWarfareLinkFirst = 6732,
    kEffWarfareLinkLast = 6736,
};

// Attributes hand-written code reads (cycle times, shots, fitting, repairers):
// resolved per item at setup to a field index or a constant base value.
enum Hot : int
{
    HotSpeed,
    HotDuration,
    HotDurationHigh,
    HotBurst0,
    HotBurst1,
    HotBurst2,
    HotBurst3,
    HotReactivation,
    HotReload,
    HotChargeRate,
    HotCapNeed,
    HotCpu,
    HotPower,
    HotUpgradeCost,
    HotShieldBonus,
    HotArmorDamage,
    HotStructureDamage,
    HotChargedArmorMult,
    HotCrystalsDamaged,
    HotHp,
    HotCrystalChance,
    HotCrystalDamage,
    kHot
};

// A module's cycle time, reload and cap use (pyfa rawCycleTime, getCycleParameters, capUse).
struct CycleInfo
{
    float raw = 0, reactivation = 0, reload = 0, average = 0, capNeed = 0, capUse = 0;
    int numShots = 0;
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
    bool active = true;  // implants / boosters
    const std::vector<int>* sideEffects = nullptr;  // boosters: effectIDs switched on
    const std::vector<std::pair<int, float>>* mutators = nullptr;
    std::array<int, 6> reqSkills{};
    int reqCount = 0;

    // Field arrays, kArrays x layout->count floats in the scratch's arena:
    //   base         unmodified value
    //   preAssign / intermediary / force   pyfa's assignments (masks below)
    //   preInc / mult / postInc            pyfa's accumulators (penalized multipliers live in Scratch::pens)
    //   cache        computed values of touched fields; any write to the item clears
    //                it (values depend only on the item's own fields, penalties and caps)
    static constexpr int kArrays = 8;
    float* data = nullptr;
    int stride = 0;
    float* base() const { return data; }
    float* preAssign() const { return data + stride; }
    float* intermediary() const { return data + 2 * stride; }  // pyfa itemModifiedAttributes[key] = v
    float* preInc() const { return data + 3 * stride; }
    float* mult() const { return data + 4 * stride; }
    float* postInc() const { return data + 5 * stride; }
    float* force() const { return data + 6 * stride; }
    float* cache() const { return data + 7 * stride; }
    uint64_t hasPreAssign = 0, hasIntermediary = 0, hasForce = 0, touched = 0, penalized = 0, cached = 0;

    int numCharges = 0;  // module: charges per load, from the module and charge types (pyfa numCharges)
    int penHead = -1;    // newest of this item's stacking-penalized multipliers (Scratch::pens list)

    std::array<int8_t, kHot> hotField{};  // field index, -1 if not in the layout (then hotBase is final)
    std::array<float, kHot> hotBase{};
    uint32_t hotHas = 0;                  // the item itself has the attribute (pyfa 'x' in item.attributes)

    // Search plans: values that can't change between tests, computed at plan build.
    bool constCycle = false;
    CycleInfo cycle;
    bool constFitting = false;
    float cpu = 0, power = 0, calibration = 0;

    uint32_t repEffects = 0;   // which repairer effects ran (bit per kRep*)
    bool ancillaryShield = false;  // module group (Remote) Ancillary Shield Booster
    bool isPaste = false;          // charge is Nanite Repair Paste
};

struct Penalty
{
    uint16_t item;
    uint8_t field;
    uint8_t group;
    float value;
    int next;  // older penalty of the same item, -1 = none
};

enum : uint32_t
{
    kRepShield = 1,
    kRepArmor = 2,
    kRepArmorFueled = 4,
    kRepHull = 8,
};

// pyfa Fit.commandBonuses entry
struct CommandBonus
{
    int buffID;
    float value;
    uint8_t runTime;
};
constexpr int kMaxCommandBonuses = 64;

constexpr int kArenaFloats = kMaxItems * Item::kArrays * kMaxFields;

// Item index -> item. Normally the scratch's own storage; a search test points
// the items it doesn't write at the plan's shared ones.
struct ItemTable
{
    std::array<Item*, kMaxItems> ptr{};
    Item& operator[](size_t i) const { return *ptr[i]; }
};

struct Scratch
{
    std::array<Item, kMaxItems> storage;
    ItemTable items;
    std::array<float, kArenaFloats> arena;  // items' field arrays
    int arenaUsed = 0;
    std::array<CommandBonus, kMaxCommandBonuses> bonuses;
    int bonusCount = 0;
    int count = 0;
    std::array<Penalty, kMaxPenalties> pens;
    int penCount = 0;
    int ship = -1;
    int mode = -1;
    std::array<int, kMaxItems> modules;
    int moduleCount = 0;
    std::array<int, 32> implants;
    int implantCount = 0;
    std::array<int, 16> boosters;
    int boosterCount = 0;
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

float FitResult::shipAttr(Attr a) const
{
    for (size_t i = 0; i < std::size(fitschema::ShipAttrs::kFields); ++i)
        if (fitschema::ShipAttrs::kFields[i] == a)
            return ship[i];
    return 0.0f;
}

struct Engine::Impl
{
    const GameData& gd;
    const Character& ch;

    std::unordered_map<int, EffectInfo> effects;
    std::unordered_map<int, std::vector<Mod>> buffMods;  // warfare buff ID -> modifiers
    std::unordered_map<std::string, Attr> attrByName;
    std::vector<int16_t> attrByEve;  // eve attributeID -> Attr index, -1 if not in the schema
    std::vector<float> defaultByEve; // eve attributeID -> dgmattribs default
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
    std::array<int, 4> eveWarfareBuffID{}, eveWarfareBuffValue{};
    std::array<int, kHot> hotEve{};
    std::array<int, 4> eveArmorRes{}, eveShipArmorRes{};
    Attr attrCpu, attrPower, attrCpuOutput, attrPowerOutput;

    Impl(const GameData& g, const Character& c) : gd(g), ch(c)
    {
        for (size_t i = 0; i < fitschema::kAttrCount; ++i)
        {
            Attr a = static_cast<Attr>(i);
            attrByName[fitschema::kAttrName[i]] = a;
            size_t eveID = fitschema::kAttrEveID[i];
            if (attrByEve.size() <= eveID)
                attrByEve.resize(eveID + 1, -1);
            attrByEve[eveID] = static_cast<int16_t>(i);
            if (const Attribute* info = gd.attribute(fitschema::kAttrEveID[i]))
            {
                capMin[i] = info->minAttributeID.value_or(0);
                capMax[i] = info->maxAttributeID.value_or(0);
            }
        }
        for (const auto& [id, a] : gd.allAttributes())
        {
            if (defaultByEve.size() <= static_cast<size_t>(id))
                defaultByEve.resize(static_cast<size_t>(id) + 1, 0.0f);
            defaultByEve[static_cast<size_t>(id)] = a.defaultValue;
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
        for (int x = 0; x < 4; ++x)
        {
            eveWarfareBuffID[x] = eve(("warfareBuff" + std::to_string(x + 1) + "ID").c_str());
            eveWarfareBuffValue[x] = eve(("warfareBuff" + std::to_string(x + 1) + "Value").c_str());
        }
        const char* res[4] = {"Em", "Thermal", "Kinetic", "Explosive"};
        for (int i = 0; i < 4; ++i)
            eveArmorRes[i] = eveShipArmorRes[i] = eve((std::string("armor") + res[i] + "DamageResonance").c_str());
        hotEve = {eveSpeed, eveDuration, eveDurationHighIsGood, eveBurstDurations[0], eveBurstDurations[1],
                  eveBurstDurations[2], eveBurstDurations[3], eveReactivation, eveReloadTime, eveChargeRate,
                  eveCapNeed, eveCpu, evePower, eveUpgradeCost, eveShieldBonus, eveArmorDamage, eveStructureDamage,
                  eveChargedArmorMult, eveCrystalsDamaged, eveHp, eveCrystalChance, eveCrystalDamage};
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
        else if (m.domain == "appliedImplants")
            out.target = TargetSel::Implants;
        else if (m.domain == "boosters")
            out.target = TargetSel::Boosters;
        else
            return std::nullopt;  // drones, fighters, projected targets, ...

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
        if (m.sourceOf == "buff")
            out.source = SourceSel::Buff;
        else if (!out.sourceEveAttr)
            return std::nullopt;
        else if (m.sourceOf.empty() || m.sourceOf == "owner")
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
                info.sideEffect = hasWord(m.effectType, "boosterSideEffect");
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
        for (const auto& [buffID, rows] : gd.allWarfareBuffModifiers())
            for (const EffectModifier& m : rows)
                if (auto mod = compile(m))
                {
                    buffMods[buffID].push_back(std::move(*mod));
                    ++compiled;
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
        for (int eid = kEffWarfareLinkFirst; eid <= kEffWarfareLinkLast; ++eid)
            ensure(eid, 1);
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
        Item& it = s.storage[static_cast<size_t>(idx)];
        s.items.ptr[static_cast<size_t>(idx)] = &it;
        it = Item{};
        it.type = type;
        it.layout = layout;
        it.stride = layout->count;
        it.data = allocFields(s, it.stride);
        it.group = gd.groupOf(*type);
        it.kind = kind;
        auto req = gd.requiredSkills(*type);
        it.reqCount = static_cast<int>(std::min(req.size(), it.reqSkills.size()));
        it.ancillaryShield = it.group && (it.group->name == "Ancillary Shield Booster" ||
                                          it.group->name == "Ancillary Remote Shield Booster");
        it.isPaste = type->name == "Nanite Repair Paste";
        std::copy_n(req.begin(), it.reqCount, it.reqSkills.begin());
        return idx;
    }

    static float* allocFields(Scratch& s, int stride)
    {
        int n = Item::kArrays * stride;
        if (s.arenaUsed + n > kArenaFloats)
            throw std::runtime_error("fit scratch arena full");
        float* p = s.arena.data() + s.arenaUsed;
        s.arenaUsed += n;
        return p;
    }

    void initValues(Item& it) const
    {
        for (int f = 0; f < it.layout->count; ++f)
        {
            int eve = fitschema::kAttrEveID[static_cast<size_t>(it.layout->fields[f])];
            it.base()[f] = baseOf(it, eve);
            it.preInc()[f] = 0;
            it.mult()[f] = 1;
            it.postInc()[f] = 0;
        }
        for (int h = 0; h < kHot; ++h)
        {
            int eve = hotEve[static_cast<size_t>(h)];
            it.hotField[static_cast<size_t>(h)] = static_cast<int8_t>(fieldOf(it, eve));
            it.hotBase[static_cast<size_t>(h)] = baseOf(it, eve);
            if (hasBase(it, eve))
                it.hotHas |= 1u << h;
        }
    }

    // read() / hasAttrOrModified() for a hot attribute.
    float readHot(Scratch& s, int item, Hot h) const
    {
        const Item& it = s.items[static_cast<size_t>(item)];
        int f = it.hotField[static_cast<size_t>(h)];
        return f >= 0 ? value(s, item, f, 0) : it.hotBase[static_cast<size_t>(h)];
    }
    bool hasHot(const Scratch& s, int item, Hot h) const
    {
        const Item& it = s.items[static_cast<size_t>(item)];
        int f = it.hotField[static_cast<size_t>(h)];
        return (it.hotHas >> h & 1u) || (f >= 0 && (it.touched >> f & 1));
    }

    // The item type's own attribute; for mutated modules merged over the base item
    // (pyfa getItemWithBaseItemAttribute: {**base.attributes, **item.attributes}).
    static std::optional<float> typeAttr(const Item& it, int eve)
    {
        if (auto v = it.type->attr(eve))
            return v;
        return it.baseType ? it.baseType->attr(eve) : std::nullopt;
    }

    // Unmodified value: abyssal mutator, else the type's value, else the attribute default.
    float baseOf(const Item& it, int eve) const
    {
        if (it.mutators)
            for (const auto& [id, v] : *it.mutators)
                if (id == eve)
                    return v;
        if (auto v = typeAttr(it, eve))
            return *v;
        return eve > 0 && static_cast<size_t>(eve) < defaultByEve.size() ? defaultByEve[static_cast<size_t>(eve)]
                                                                          : 0.0f;
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
        if (eve <= 0 || static_cast<size_t>(eve) >= attrByEve.size() || attrByEve[static_cast<size_t>(eve)] < 0)
            return -1;
        return it.layout->fieldOf[static_cast<size_t>(attrByEve[static_cast<size_t>(eve)])];
    }

    // pyfa getModifiedItemAttr(attr): modified value, or the attribute's dgmattribs
    // default when the item lacks it (eve.db has no NULL defaults, so never None).
    float read(Scratch& s, int item, int eve, int depth) const
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
    float value(Scratch& s, int item, int field, int depth) const
    {
        Item& it = s.items[item];
        // Attributes no modifier touched never reach __calculateValue: pyfa returns
        // the intermediary or original value as is - no caps, no cpu/power rounding.
        if (!(it.touched >> field & 1))
            return (it.hasIntermediary >> field & 1) ? it.intermediary()[field] : it.base()[field];
        if (it.cached >> field & 1)
            return it.cache()[field];
        float val;
        if (it.hasForce >> field & 1)
            val = it.force()[field];
        else
        {
            // priority: intermediary > preAssign > original
            val = (it.hasIntermediary >> field & 1) ? it.intermediary()[field]
                  : (it.hasPreAssign >> field & 1) ? it.preAssign()[field]
                                                    : it.base()[field];
            val += it.preInc()[field];
            val *= it.mult()[field];
            if (it.penalized >> field & 1)
                val = applyPenalties(s, item, field, val);
            val += it.postInc()[field];
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
        Item& w = s.items[item];  // (caps may have read other fields of this item)
        w.cache()[field] = val;
        w.cached |= uint64_t{1} << field;
        return val;
    }

    // Stacking penalties: each group separately, bonuses and maluses separately,
    // strongest first: 1 + (m - 1) * exp(-i^2 / 7.1289).
    float applyPenalties(const Scratch& s, int item, int field, float val) const
    {
        // This field's multipliers in application order (the list is newest first).
        constexpr int kMax = 64;
        float vals[kMax];
        uint8_t groups[kMax];
        int n = 0;
        for (int p = s.items[item].penHead; p >= 0 && n < kMax; p = s.pens[p].next)
            if (s.pens[p].field == field)
            {
                vals[n] = s.pens[p].value;
                groups[n++] = s.pens[p].group;
            }
        std::reverse(vals, vals + n);
        std::reverse(groups, groups + n);

        // Stable insertion sort, strongest first (lists are short).
        auto sortByStrength = [](float* a, int count) {
            for (int k = 1; k < count; ++k)
            {
                float x = a[k];
                float strength = std::abs(x - 1);
                int j = k - 1;
                for (; j >= 0 && std::abs(a[j] - 1) < strength; --j)
                    a[j + 1] = a[j];
                a[j + 1] = x;
            }
        };
        static const float falloff[] = {1.0f,
                                        std::exp(-1.0f / 7.1289f),
                                        std::exp(-4.0f / 7.1289f),
                                        std::exp(-9.0f / 7.1289f),
                                        std::exp(-16.0f / 7.1289f),
                                        std::exp(-25.0f / 7.1289f),
                                        std::exp(-36.0f / 7.1289f),
                                        std::exp(-49.0f / 7.1289f)};
        auto factor = [&](int i) {
            return i < 8 ? falloff[i] : std::exp(-static_cast<float>(i * i) / 7.1289f);
        };

        uint64_t groupsSeen = 0;
        for (int i = 0; i < n; ++i)
        {
            if (groupsSeen >> groups[i] & 1u)
                continue;
            groupsSeen |= uint64_t{1} << groups[i];
            float up[kMax], down[kMax];
            int nUp = 0, nDown = 0;
            for (int j = i; j < n; ++j)
            {
                if (groups[j] != groups[i])
                    continue;
                if (vals[j] > 1)
                    up[nUp++] = vals[j];
                else if (vals[j] < 1)
                    down[nDown++] = vals[j];
            }
            sortByStrength(up, nUp);
            sortByStrength(down, nDown);
            for (int k = 0; k < nUp; ++k)
                val *= 1 + (up[k] - 1) * factor(k);
            for (int k = 0; k < nDown; ++k)
                val *= 1 + (down[k] - 1) * factor(k);
        }
        return val;
    }

    // ------------------------------------------------------------ applying modifiers

    void applyTo(Scratch& s, int item, const Mod& m, float amount) const
    {
        Item& it = s.items[item];
        int f = it.layout->fieldOf[static_cast<size_t>(m.targetAttr)];
        if (f < 0)
            return;
        uint64_t bit = uint64_t{1} << f;
        it.touched |= bit;
        it.cached = 0;
        switch (m.op)
        {
        case Op::PreAssign:
            it.preAssign()[f] = amount;
            it.hasPreAssign |= bit;
            break;
        case Op::PreIncrease:
            it.preInc()[f] += amount;
            break;
        case Op::PostIncrease:
            it.postInc()[f] += amount;
            break;
        case Op::Force:
            it.force()[f] = amount;
            it.hasForce |= bit;
            break;
        case Op::Multiply:
        {
            float mult = m.boost ? 1.0f + amount / 100.0f : amount;
            if (m.penalized)
            {
                if (s.penCount >= kMaxPenalties)
                    throw std::runtime_error("too many stacking-penalized modifiers");
                s.pens[s.penCount] = {static_cast<uint16_t>(item), static_cast<uint8_t>(f), m.penaltyGroup, mult,
                                      it.penHead};
                it.penHead = s.penCount++;
                it.penalized |= bit;
            }
            else
                it.mult()[f] *= mult;
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

    std::optional<float> amountOf(Scratch& s, const Mod& m, int owner, const Type* ownerSkill,
                                   float buffValue) const
    {
        float v = 0;
        switch (m.source)
        {
        case SourceSel::Buff:
            v = buffValue;
            break;
        case SourceSel::Owner:
            if (owner >= 0)
                v = read(s, owner, m.sourceEveAttr, 0);
            else if (ownerSkill)
            {
                const Attribute* a = gd.attribute(m.sourceEveAttr);
                v = ownerSkill->attr(m.sourceEveAttr).value_or(a ? a->defaultValue : 0.0f);
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
            v = skill ? skill->attr(m.sourceEveAttr).value_or(a ? a->defaultValue : 0.0f) : 0.0f;
            break;
        }
        }
        float level = 1.0f;
        if (m.perLevel && ownerSkill)
            level *= ch.skillLevel(ownerSkill->id);
        if (m.levelSkillID)
            level *= ch.skillLevel(m.levelSkillID);
        float value = v * level;
        return m.invert ? (value != 0 ? m.factor / value : 0.0f) : m.factor * value;
    }

    void applyMod(Scratch& s, const Mod& m, int owner, const Type* ownerSkill, float buffValue = 0) const
    {
        if (!conditionsHold(s, m, owner))
            return;
        auto amount = amountOf(s, m, owner, ownerSkill, buffValue);
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
        case TargetSel::Implants:
            for (int i = 0; i < s.implantCount; ++i)
                if (filterMatches(s, m, s.implants[i], ownerSkill))
                    applyTo(s, s.implants[i], m, *amount);
            break;
        case TargetSel::Boosters:
            for (int i = 0; i < s.boosterCount; ++i)
                if (filterMatches(s, m, s.boosters[i], ownerSkill))
                    applyTo(s, s.boosters[i], m, *amount);
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
        if (eid >= kEffWarfareLinkFirst && eid <= kEffWarfareLinkLast)
        {
            // for x in 1..4: if charge warfareBuff{x}ID: addCommandBonus(id, module warfareBuff{x}Value)
            if (it.charge < 0)
                return;
            for (int x = 0; x < 4; ++x)
            {
                int id = static_cast<int>(read(s, it.charge, eveWarfareBuffID[x], 0));
                if (id)
                    addCommandBonus(s, id, read(s, module, eveWarfareBuffValue[x], 0), 1);
            }
            return;
        }
        switch (eid)
        {
        case kEffCapBooster:
            // capAmount = charge capacitorBonus; module.itemModifiedAttributes['capacitorNeed'] = -capAmount
            if (it.charge >= 0)
            {
                int f = fieldOf(it, eveCapNeed);
                if (f >= 0)
                {
                    it.intermediary()[f] = -read(s, it.charge, eveCapacitorBonus, 0);
                    it.hasIntermediary |= uint64_t{1} << f;
                    it.cached = 0;
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
        // Internals in double like pyfa: the loop sorts, compares and rounds, where
        // float noise could flip a decision.
        // Inputs are snapped to 6 decimals: float noise would otherwise split values
        // pyfa sees as exactly equal and flip the loop's tie-breaking order.
        auto snap = [](float v) { return std::round(static_cast<double>(v) * 1e6) / 1e6; };
        double baseDamageTaken[4], rah[4];
        for (int i = 0; i < 4; ++i)
        {
            baseDamageTaken[i] = 25.0 * snap(read(s, s.ship, eveShipArmorRes[i], 0));
            rah[i] = snap(read(s, module, eveArmorRes[i], 0));
        }
        double shift = snap(read(s, module, eveResistanceShift, 0)) / 100.0;

        std::array<std::array<double, 4>, 50> cycles;
        size_t cycleCount = 0;
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

            for (size_t i = 0; i < cycleCount; ++i)
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
            cycles[cycleCount++] = {rah[0], rah[1], rah[2], rah[3]};
        }
        // Python slicing: cycleList[loopStart:], negative = last 20.
        size_t from = loopStart >= 0 ? static_cast<size_t>(loopStart)
                                     : (cycleCount > 20 ? cycleCount - 20 : 0);
        double avg[4] = {0, 0, 0, 0};
        for (size_t c = from; c < cycleCount; ++c)
            for (int i = 0; i < 4; ++i)
                avg[i] += cycles[c][i];
        double n = static_cast<double>(cycleCount - from);
        for (int i = 0; i < 4; ++i)
        {
            avg[i] = pymath::round(avg[i] / n, 3);
            // module.increaseItemAttr(attr, avg - current); ship.multiplyItemAttr(attr, avg, penalized 'preMul')
            Mod inc;
            inc.op = Op::PreIncrease;
            inc.targetAttr = static_cast<Attr>(attrByEve.at(static_cast<size_t>(eveArmorRes[i])));
            applyTo(s, module, inc, static_cast<float>(avg[i] - read(s, module, eveArmorRes[i], 0)));
            Mod mul;
            mul.op = Op::Multiply;
            mul.penalized = true;
            mul.penaltyGroup = preMulGroup;
            mul.targetAttr = inc.targetAttr;
            applyTo(s, s.ship, mul, static_cast<float>(avg[i]));
        }
    }


    // ------------------------------------------------------------ traversal

    const EffectInfo* effect(int eid) const
    {
        auto it = effects.find(eid);
        return it == effects.end() ? nullptr : &it->second;
    }

    static bool runsInState(const EffectInfo& e, State st)
    {
        return e.offline || (e.passive && st >= State::Online) || (e.active && st >= State::Active);
    }

    // pyfa's evaluation order (Fit.calculateModifiedAttributes), handing each step to a sink:
    //   sink.apply(s, mod, owner, ownerSkill)  one modifier from one source
    //   sink.special(s, eid, module)           an effect handled in code
    //   sink.boosts(s, pass)                   end of pass: command burst buffs
    template <typename Sink>
    void traverse(Scratch& s, Sink& sink) const
    {
        for (uint8_t pass = 0; pass < 3; ++pass)
        {
            for (const Type* skill : skills)
                for (int eid : skill->effects)
                    if (const EffectInfo* e = effect(eid); e && e->passive && e->runTime == pass)
                        for (const Mod& m : e->mods[CtxSkill])
                            sink.apply(s, m, -1, skill);

            for (int eid : s.items[s.ship].type->effects)
                if (const EffectInfo* e = effect(eid); e && e->passive && e->runTime == pass)
                    for (const Mod& m : e->mods[CtxShip])
                        sink.apply(s, m, s.ship, nullptr);

            // (pyfa runs drones and fighters here; not modeled.)
            for (int i = 0; i < s.boosterCount; ++i)
            {
                int b = s.boosters[i];
                const Item& it = s.items[b];
                if (!it.active)
                    continue;
                for (int eid : it.type->effects)
                    if (const EffectInfo* e = effect(eid); e && e->runTime == pass &&
                                                           (e->passive || (e->sideEffect && sideEffectOn(it, eid))))
                        for (const Mod& m : e->mods[CtxBooster])
                            sink.apply(s, m, b, nullptr);
            }
            for (int i = 0; i < s.implantCount; ++i)
            {
                int imp = s.implants[i];
                if (!s.items[imp].active)
                    continue;
                for (int eid : s.items[imp].type->effects)
                    if (const EffectInfo* e = effect(eid); e && e->passive && e->runTime == pass)
                        for (const Mod& m : e->mods[CtxImplant])
                            sink.apply(s, m, imp, nullptr);
            }

            for (int i = 0; i < s.moduleCount; ++i)
            {
                int mod = s.modules[i];
                const Item& it = s.items[mod];
                if (it.charge >= 0)
                    for (int eid : s.items[it.charge].type->effects)
                        if (const EffectInfo* e = effect(eid); e && e->runTime == pass && runsInState(*e, it.state))
                            for (const Mod& m : e->mods[CtxModuleCharge])
                                sink.apply(s, m, mod, nullptr);
                if (it.state >= State::Overheated)
                    for (int eid : it.type->effects)
                        if (const EffectInfo* e = effect(eid); e && e->overheat && e->runTime == pass)
                            for (const Mod& m : e->mods[CtxModule])
                                sink.apply(s, m, mod, nullptr);
                for (int eid : it.type->effects)
                    if (const EffectInfo* e = effect(eid); e && e->runTime == pass && runsInState(*e, it.state))
                    {
                        for (const Mod& m : e->mods[CtxModule])
                            sink.apply(s, m, mod, nullptr);
                        sink.special(s, eid, mod);
                    }
            }

            if (s.mode >= 0)
                for (int eid : s.items[s.mode].type->effects)
                    if (const EffectInfo* e = effect(eid); e && e->runTime == pass)
                        for (const Mod& m : e->mods[CtxModule])
                            sink.apply(s, m, s.mode, nullptr);

            sink.boosts(s, pass);
        }
    }

    static bool sideEffectOn(const Item& booster, int eid)
    {
        return booster.sideEffects &&
               std::find(booster.sideEffects->begin(), booster.sideEffects->end(), eid) != booster.sideEffects->end();
    }

    struct Executor
    {
        const Impl& e;
        void apply(Scratch& s, const Mod& m, int owner, const Type* skill) const { e.applyMod(s, m, owner, skill); }
        void special(Scratch& s, int eid, int module) const { e.runSpecial(s, eid, module); }
        void boosts(Scratch& s, uint8_t pass) const { e.applyBoosts(s, pass); }
    };

    void run(Scratch& s) const
    {
        Executor ex{*this};
        traverse(s, ex);
    }

    // pyfa Fit.__runCommandBoosts(runTime), in commandBonuses insertion order
    void applyBoosts(Scratch& s, uint8_t pass) const
    {
        for (int b = 0; b < s.bonusCount; ++b)
        {
            const CommandBonus& cb = s.bonuses[b];
            if (cb.runTime != pass)
                continue;
            auto mods = buffMods.find(cb.buffID);
            if (mods != buffMods.end())
                for (const Mod& m : mods->second)
                    applyMod(s, m, -1, nullptr, cb.value);
        }
    }

    // pyfa Fit.addCommandBonus: per buff, the value with the largest magnitude wins.
    static void addCommandBonus(Scratch& s, int buffID, float value, uint8_t runTime)
    {
        for (int b = 0; b < s.bonusCount; ++b)
            if (s.bonuses[b].buffID == buffID)
            {
                if (std::abs(s.bonuses[b].value) < std::abs(value))
                    s.bonuses[b] = {buffID, value, runTime};
                return;
            }
        if (s.bonusCount >= kMaxCommandBonuses)
            throw std::runtime_error("too many command bonuses");
        s.bonuses[s.bonusCount++] = {buffID, value, runTime};
    }

    // ------------------------------------------------------------ fit setup

    void resetScratch(Scratch& s) const
    {
        s.count = s.penCount = s.moduleCount = s.bonusCount = s.implantCount = s.boosterCount = s.arenaUsed = 0;
        s.ship = s.mode = -1;
    }

    void addShipAndMode(Scratch& s, const FitInput& in) const
    {
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
    }

    // Adds the fit's implants and boosters.
    void addImplantsAndBoosters(Scratch& s, const FitInput& in, std::vector<std::string>* warnings) const
    {
        auto layoutOf = [&](const Type* t) {
            auto l = layoutByGroup.find(t->groupID);
            return l == layoutByGroup.end() ? &emptyLayout : l->second;
        };
        for (const ImplantInput& ii : in.implants)
        {
            const Type* t = gd.type(ii.typeID);
            if (!t || s.implantCount >= static_cast<int>(s.implants.size()))
            {
                if (warnings)
                    warnings->push_back("implant " + std::to_string(ii.typeID) + " skipped");
                continue;
            }
            int item = addItem(s, t, layoutOf(t), ItemKind::Implant);
            s.items[item].active = ii.active;
            initValues(s.items[item]);
            s.implants[s.implantCount++] = item;
        }
        for (const BoosterInput& bi : in.boosters)
        {
            const Type* t = gd.type(bi.typeID);
            if (!t || s.boosterCount >= static_cast<int>(s.boosters.size()))
            {
                if (warnings)
                    warnings->push_back("booster " + std::to_string(bi.typeID) + " skipped");
                continue;
            }
            int item = addItem(s, t, layoutOf(t), ItemKind::Booster);
            s.items[item].active = bi.active;
            s.items[item].sideEffects = bi.sideEffects.empty() ? nullptr : &bi.sideEffects;
            initValues(s.items[item]);
            s.boosters[s.boosterCount++] = item;
        }
    }

    // Adds a module (and its charge) to s.modules; returns the module item, -1 if unknown.
    int addModule(Scratch& s, const ModuleInput& mi, int input, std::vector<std::string>* warnings) const
    {
        const Type* t = gd.type(mi.typeID);
        if (!t)
        {
            if (warnings)
                warnings->push_back("module type " + std::to_string(mi.typeID) + " unknown, skipped");
            return -1;
        }
        auto l = layoutByGroup.find(t->groupID);
        int mod = addItem(s, t, l == layoutByGroup.end() ? &emptyLayout : l->second, ItemKind::Module);
        Item& it = s.items[mod];
        it.state = mi.state;
        it.baseType = mi.baseTypeID ? gd.type(mi.baseTypeID) : nullptr;
        it.input = input;
        it.mutators = mi.mutators.empty() ? nullptr : &mi.mutators;
        initValues(it);
        s.modules[s.moduleCount++] = mod;
        if (mi.chargeID)
        {
            if (const Type* c = gd.type(mi.chargeID))
            {
                auto cl = layoutByGroup.find(c->groupID);
                int charge = addItem(s, c, cl == layoutByGroup.end() ? &emptyLayout : cl->second, ItemKind::Charge);
                initValues(s.items[charge]);
                s.items[charge].owner = mod;
                s.items[mod].charge = charge;
                // pyfa getNumCharges: int(floatUnerr(capacity / volume)), from the types' own attributes
                auto volume = c->attr(eveVolume);
                auto capacity = typeAttr(s.items[mod], eveCapacity);
                s.items[mod].numCharges = volume && capacity && *volume != 0
                                              ? static_cast<int>(pymath::floatUnerr(static_cast<double>(*capacity) / *volume))
                                              : 0;
            }
            else if (warnings)
                warnings->push_back("charge type " + std::to_string(mi.chargeID) + " unknown, skipped");
        }
        return mod;
    }

    // ------------------------------------------------------------ results

    using Cycle = CycleInfo;

    struct Totals
    {
        float cpu = 0, power = 0, calibration = 0;
        float cpuOutput = 0, powerOutput = 0, calibrationOutput = 0;
        float capPeak = 0, capRecharge = 0, capUsed = 0;
        std::array<float, LayerCount> hp{}, ehp{}, repair{}, effRepair{};
    };

    float shipValue(Scratch& s, Attr a) const
    {
        int f = shipLayout.fieldOf[static_cast<size_t>(a)];
        return value(s, s.ship, f, 0);
    }

    // floor() of a ratio pyfa computes in double from exact inputs; our float inputs
    // can put an exact integer (1000) just below it (999.9999), so allow 1e-5 relative.
    static int floorTolerant(double x) { return static_cast<int>(std::floor(x * (1 + 1e-5))); }

    // pyfa Module.numShots
    int numShots(Scratch& s, int module) const
    {
        const Item& it = s.items[module];
        if (it.charge < 0)
            return 0;
        int numCharges = it.numCharges;
        if (numCharges <= 0)
            return 0;
        if (hasHot(s, module, HotChargeRate))
            return floorTolerant(numCharges / static_cast<double>(readHot(s, module, HotChargeRate)));
        if (hasHot(s, it.charge, HotCrystalsDamaged))
        {
            if (readHot(s, it.charge, HotCrystalsDamaged) != 1)
                return 0;
            float hp = readHot(s, it.charge, HotHp);
            float chance = readHot(s, it.charge, HotCrystalChance);
            float damage = readHot(s, it.charge, HotCrystalDamage);
            return floorTolerant(numCharges * static_cast<double>(hp) / (static_cast<double>(damage) * chance));
        }
        return 0;
    }

    // Cycle time, reload and cap use of a module (pyfa rawCycleTime, getCycleParameters, capUse).
    Cycle cycleOf(Scratch& s, int module) const
    {
        const Item& it = s.items[module];
        Cycle c;
        c.raw = std::max({readHot(s, module, HotSpeed), readHot(s, module, HotDuration),
                          readHot(s, module, HotDurationHigh), readHot(s, module, HotBurst0),
                          readHot(s, module, HotBurst1), readHot(s, module, HotBurst2),
                          readHot(s, module, HotBurst3)});
        c.reactivation = readHot(s, module, HotReactivation);
        c.reload = readHot(s, module, HotReload);
        c.numShots = numShots(s, module);
        if (c.raw > 0)
        {
            // Reload always factored in.
            if (c.numShots == 0 || c.reactivation >= c.reload)
                c.average = c.raw + c.reactivation;
            else if (c.numShots == 1)
                c.average = c.raw + c.reload;
            else
            {
                float early = static_cast<float>(c.numShots - 1);
                c.average = (early * (c.raw + c.reactivation) + (c.raw + c.reload)) / (early + 1);
            }
        }
        c.capNeed = readHot(s, module, HotCapNeed);
        c.capUse = (c.capNeed != 0 && it.state >= State::Active && c.average > 0) ? c.capNeed / (c.average / 1000.0f)
                                                                                   : 0.0f;
        return c;
    }

    // Everything derived from the final attributes. cycles[i] belongs to s.modules[i].
    void totals(Scratch& s, Cycle* cycles, Totals& t, bool withRepair) const
    {
        t = Totals{};
        for (int i = 0; i < s.moduleCount; ++i)
        {
            int mod = s.modules[i];
            const Item& it = s.items[mod];
            cycles[i] = it.constCycle ? it.cycle : cycleOf(s, mod);
            if (it.state < State::Online)
                continue;
            // pyfa getItemAttrOnlineSum
            if (it.constFitting)
            {
                t.cpu += it.cpu;
                t.power += it.power;
                t.calibration += it.calibration;
            }
            else
            {
                t.cpu += readHot(s, mod, HotCpu);
                t.power += readHot(s, mod, HotPower);
                t.calibration += readHot(s, mod, HotUpgradeCost);
            }
        }
        t.cpu = pymath::round(t.cpu, 2);
        t.power = pymath::round(t.power, 2);
        t.cpuOutput = shipValue(s, Attr::cpuOutput);
        t.powerOutput = shipValue(s, Attr::powerOutput);
        t.calibrationOutput = shipValue(s, Attr::upgradeCapacity);

        // pyfa __generateDrain + calculateCapRecharge
        float capacity = shipValue(s, Attr::capacitorCapacity);
        float rechargeSeconds = shipValue(s, Attr::rechargeRate) / 1000.0f;
        t.capPeak = 10.0f / rechargeSeconds * std::sqrt(0.25f) * (1 - std::sqrt(0.25f)) * capacity;
        float capAdded = 0;
        for (int i = 0; i < s.moduleCount; ++i)
        {
            const Cycle& c = cycles[i];
            if (s.items[s.modules[i]].state < State::Active || c.capNeed == 0 || c.raw + c.reactivation <= 0)
                continue;
            if (c.capUse > 0)
                t.capUsed += c.capUse;
            else
                capAdded -= c.capUse;
        }
        t.capRecharge = capAdded + t.capPeak;

        static const Attr hpAttr[LayerCount] = {Attr::shieldCapacity, Attr::armorHP, Attr::hp};
        static const Attr resAttr[LayerCount][4] = {
            {Attr::shieldEmDamageResonance, Attr::shieldThermalDamageResonance, Attr::shieldKineticDamageResonance,
             Attr::shieldExplosiveDamageResonance},
            {Attr::armorEmDamageResonance, Attr::armorThermalDamageResonance, Attr::armorKineticDamageResonance,
             Attr::armorExplosiveDamageResonance},
            {Attr::emDamageResonance, Attr::thermalDamageResonance, Attr::kineticDamageResonance,
             Attr::explosiveDamageResonance}};
        std::array<float, LayerCount> divider{};
        for (int l = 0; l < LayerCount; ++l)
        {
            for (Attr a : resAttr[l])
                divider[l] += kUniformShare * shipValue(s, a);
            if (divider[l] == 0)
                divider[l] = 1;
            t.hp[l] = shipValue(s, hpAttr[l]);
            t.ehp[l] = t.hp[l] / divider[l];
        }
        if (withRepair)
        {
            sustainableTank(s, cycles, t);
            for (int l = 0; l < LayerCount; ++l)
                t.effRepair[l] = t.repair[l] / divider[l];
        }
    }

    // Port of pyfa Fit.calculateSustainableTank (local repairers only, reload factored).
    void sustainableTank(Scratch& s, const Cycle* cycles, Totals& t) const
    {
        struct Rep
        {
            int module;  // index into s.modules / cycles
            int layer;
            double amount;
        };
        // In double: the adjustment subtracts and re-adds large, nearly equal terms.
        std::array<double, LayerCount> extra{}, adjust{};
        Rep contributors[LayerCount][kMaxItems];
        int contributorCount[LayerCount] = {};
        for (int i = 0; i < s.moduleCount; ++i)
        {
            int mod = s.modules[i];
            const Item& it = s.items[mod];
            if (!it.repEffects)
                continue;
            double speed = readHot(s, mod, HotDuration) / 1000.0;
            if (it.repEffects & kRepShield)
            {
                double amount = readHot(s, mod, HotShieldBonus);
                extra[Shield] += amount / speed;
                if (amount / speed != 0)
                    contributors[Shield][contributorCount[Shield]++] = {i, Shield, amount};
            }
            if (it.repEffects & (kRepArmor | kRepArmorFueled))
            {
                bool paste = (it.repEffects & kRepArmorFueled) && it.charge >= 0 && s.items[it.charge].isPaste;
                double amount = readHot(s, mod, HotArmorDamage);
                double rps = amount * (paste ? 3 : 1) / speed;
                extra[Armor] += rps;
                if (rps != 0)
                    contributors[Armor][contributorCount[Armor]++] = {i, Armor, amount};
            }
            if (it.repEffects & kRepHull)
            {
                double amount = readHot(s, mod, HotStructureDamage);
                extra[Hull] += amount / speed;
                if (amount / speed != 0)
                    contributors[Hull][contributorCount[Hull]++] = {i, Hull, amount};
            }
        }

        auto pasteMultiplier = [&](int i) {
            int mod = s.modules[i];
            int charge = s.items[mod].charge;
            if (charge < 0 || !s.items[charge].isPaste)
                return 1.0;
            double m = readHot(s, mod, HotChargedArmorMult);
            return m ? m : 1.0;
        };

        double capUsed = t.capUsed;
        Rep repairers[LayerCount * kMaxItems];
        double efficiency[LayerCount * kMaxItems];
        int repairerCount = 0;
        for (int layer = 0; layer < LayerCount; ++layer)
        {
            for (int k = 0; k < contributorCount[layer]; ++k)
            {
                const Rep& rep = contributors[layer][k];
                const Cycle& c = cycles[rep.module];
                bool hasCharge = s.items[s.modules[rep.module]].charge >= 0;
                double cycle = c.raw / 1000.0;
                bool usesCap = c.capUse != 0;
                if (usesCap)
                    capUsed -= c.capUse;
                if (usesCap)
                {
                    adjust[layer] -= rep.amount * (hasCharge ? pasteMultiplier(rep.module) : 1.0) / cycle;
                    repairers[repairerCount++] = rep;
                }
                else if (s.items[s.modules[rep.module]].ancillaryShield)
                {
                    double reload = hasCharge ? c.reload : 0.0;
                    double offDuty = reload / (std::max(c.numShots, 1) * static_cast<double>(c.raw) + reload);
                    adjust[layer] -= rep.amount * offDuty / cycle;
                }
            }
        }

        // Most cap-efficient first (Python sort is stable, also with reverse=True).
        int order[LayerCount * kMaxItems];
        for (int r = 0; r < repairerCount; ++r)
        {
            int mod = s.modules[repairers[r].module];
            double mult = readHot(s, mod, HotChargedArmorMult);
            efficiency[r] = repairers[r].amount * (mult ? mult : 1.0) / readHot(s, mod, HotCapNeed);
            order[r] = r;
        }
        std::stable_sort(order, order + repairerCount, [&](int a, int b) { return efficiency[a] > efficiency[b]; });

        double peak = t.capRecharge;
        for (int k = 0; k < repairerCount; ++k)
        {
            if (capUsed > peak)
                break;
            const Rep& rep = repairers[order[k]];
            const Cycle& c = cycles[rep.module];
            bool hasCharge = s.items[s.modules[rep.module]].charge >= 0;
            double reload = hasCharge ? c.reload : 0.0;
            double sustainability = std::min(1.0, (peak - capUsed) / c.capUse);
            if (!hasCharge)
                adjust[rep.layer] += sustainability * rep.amount / (c.raw / 1000.0);
            else
            {
                double shots = std::max(c.numShots, 1) * static_cast<double>(c.raw);
                double onDuty = shots / (shots + reload);
                adjust[rep.layer] +=
                    sustainability * rep.amount * onDuty * pasteMultiplier(rep.module) / (c.raw / 1000.0);
            }
            capUsed += c.capUse;
        }
        for (int l = 0; l < LayerCount; ++l)
            t.repair[l] = static_cast<float>(extra[l] + adjust[l]);
    }

    // ------------------------------------------------------------ full evaluation

    static Scratch& threadScratch()
    {
        thread_local std::unique_ptr<Scratch> scratch;
        if (!scratch)
            scratch = std::make_unique<Scratch>();
        return *scratch;
    }

    FitResult evaluate(const FitInput& in) const
    {
        Scratch& s = threadScratch();
        resetScratch(s);
        FitResult out;
        addShipAndMode(s, in);
        addImplantsAndBoosters(s, in, &out.warnings);
        for (size_t i = 0; i < in.modules.size(); ++i)
            addModule(s, in.modules[i], static_cast<int>(i), &out.warnings);

        run(s);

        for (int f = 0; f < shipLayout.count; ++f)
            out.ship[static_cast<size_t>(f)] = value(s, s.ship, f, 0);

        Cycle cycles[kMaxItems];
        Totals t;
        totals(s, cycles, t, true);

        out.modules.resize(static_cast<size_t>(s.moduleCount));
        for (int i = 0; i < s.moduleCount; ++i)
        {
            int mod = s.modules[i];
            const Item& it = s.items[mod];
            ModuleResult& r = out.modules[static_cast<size_t>(i)];
            r.typeID = it.type->id;
            r.chargeID = it.charge >= 0 ? s.items[it.charge].type->id : 0;
            r.state = it.state;
            for (int f = 0; f < it.layout->count; ++f)
                r.attrs.emplace_back(it.layout->fields[f], value(s, mod, f, 0));
            r.rawCycleTime = cycles[i].raw;
            r.reactivationDelay = cycles[i].reactivation;
            r.reloadTime = cycles[i].reload;
            r.averageCycleTime = cycles[i].average;
            r.capUse = cycles[i].capUse;
            r.numShots = cycles[i].numShots;
        }
        out.cpuUsed = t.cpu;
        out.powerUsed = t.power;
        out.calibrationUsed = t.calibration;
        out.capPeakRecharge = t.capPeak;
        out.capRecharge = t.capRecharge;
        out.capUsed = t.capUsed;
        out.hp = t.hp;
        out.ehp = t.ehp;
        out.repair = t.repair;
        out.effectiveRepair = t.effRepair;
        return out;
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

TestResult toTestResult(const FitResult& r, Goal goal)
{
    TestResult t;
    switch (goal)
    {
    case Goal::ShieldPassive: t.goal = r.ehp[Shield]; break;
    case Goal::ArmorPassive: t.goal = r.ehp[Armor]; break;
    case Goal::HullPassive: t.goal = r.ehp[Hull]; break;
    case Goal::ShieldActive: t.goal = r.effectiveRepair[Shield]; break;
    case Goal::ArmorActive: t.goal = r.effectiveRepair[Armor]; break;
    }
    t.cpuUsed = r.cpuUsed;
    t.powerUsed = r.powerUsed;
    t.calibrationUsed = r.calibrationUsed;
    t.cpuOutput = r.shipAttr(Attr::cpuOutput);
    t.powerOutput = r.shipAttr(Attr::powerOutput);
    t.calibrationOutput = r.shipAttr(Attr::upgradeCapacity);
    t.capUsed = r.capUsed;
    t.capRecharge = r.capRecharge;
    return t;
}

// ==================================================================== SearchPlan

namespace
{
using Impl = Engine::Impl;

// One step of pyfa's evaluation order on the superset fit (every option present).
struct Instr
{
    enum Kind : uint8_t
    {
        Apply,    // one modifier from one source onto its targets
        Special,  // an effect handled in code (burst registration, cap booster, RAH)
        Boosts,   // end-of-pass command burst buffs
    };
    Kind kind = Apply;
    uint8_t pass = 0;
    const Mod* mod = nullptr;
    int owner = -1;  // item (Apply/Special), -1 for skills / boosts
    const Type* ownerSkill = nullptr;
    int eid = 0;
    int option = -1;  // the source only exists when this option is chosen (-1: always)
    bool burst = false;
    bool assign = false;  // writes are last-write-wins (preAssign / force / intermediary)
    bool dynamic = false;
    std::vector<int> targets;                // superset items (Apply)
    std::vector<std::pair<int, int>> reads;  // (item, field) the instruction's values depend on
    std::vector<std::pair<int, int>> writes; // (item, field) it modifies
};
}

struct SearchPlan::Data
{
    const Impl& e;
    SearchSpec spec;  // owns the ModuleInputs the items point to (mutators)
    std::unique_ptr<Scratch> base;  // superset with the static instructions baked in
    std::vector<int> itemOption;    // superset item -> option (-1: static)
    std::vector<int> slotFirstOption;
    std::vector<int> optionModule;  // option -> superset module item
    std::vector<bool> dynItem;      // superset item -> some replayed instruction writes to it
    int staticModuleCount = 0;      // s.modules[0..n) are the pinned modules
    std::vector<Instr> instrs;      // dynamic ones only, after construction
    Stats stats;

    Data(const Impl& impl, const SearchSpec& sp) : e(impl), spec(sp) {}
};

namespace
{
// Every (item, field) whose value an attribute read depends on: the field, plus
// its min/max cap attributes (read from the same item), recursively.
void addReadDeps(const Impl& e, const Scratch& s, int item, int eve, std::vector<std::pair<int, int>>& out, int depth = 0)
{
    const Item& it = s.items[item];
    int f = e.fieldOf(it, eve);
    if (f < 0)
        return;  // not in the layout: nothing can modify it
    out.emplace_back(item, f);
    if (depth > 4)
        return;
    size_t ai = static_cast<size_t>(it.layout->fields[f]);
    if (e.capMin[ai])
        addReadDeps(e, s, item, e.capMin[ai], out, depth + 1);
    if (e.capMax[ai])
        addReadDeps(e, s, item, e.capMax[ai], out, depth + 1);
}

// Resolves an Apply's targets the way applyMod does.
void resolveTargets(const Impl& e, const Scratch& s, const Mod& m, int owner, const Type* skill, std::vector<int>& out)
{
    switch (m.target)
    {
    case TargetSel::Ship:
        out.push_back(s.ship);
        break;
    case TargetSel::Self:
        if (owner >= 0)
            out.push_back(owner);
        break;
    case TargetSel::SelfCharge:
        if (owner >= 0 && s.items[owner].charge >= 0)
            out.push_back(s.items[owner].charge);
        break;
    case TargetSel::Modules:
        for (int i = 0; i < s.moduleCount; ++i)
            if (e.filterMatches(s, m, s.modules[i], skill))
                out.push_back(s.modules[i]);
        break;
    case TargetSel::ModuleCharges:
        for (int i = 0; i < s.moduleCount; ++i)
        {
            int mod = s.modules[i];
            if (s.items[mod].charge >= 0 && e.filterMatches(s, m, mod, skill))
                out.push_back(s.items[mod].charge);
        }
        break;
    case TargetSel::Implants:
        for (int i = 0; i < s.implantCount; ++i)
            if (e.filterMatches(s, m, s.implants[i], skill))
                out.push_back(s.implants[i]);
        break;
    case TargetSel::Boosters:
        for (int i = 0; i < s.boosterCount; ++i)
            if (e.filterMatches(s, m, s.boosters[i], skill))
                out.push_back(s.boosters[i]);
        break;
    }
}

void addWrites(const Impl&, const Scratch& s, const Mod& m, const std::vector<int>& targets, Instr& in)
{
    for (int t : targets)
    {
        int f = s.items[t].layout->fieldOf[static_cast<size_t>(m.targetAttr)];
        if (f >= 0)
            in.writes.emplace_back(t, f);
    }
}

struct Recorder
{
    const Impl& e;
    const std::vector<int>& itemOption;
    std::vector<Instr>& out;
    uint8_t pass = 0;

    int optionOf(int item) const { return item >= 0 ? itemOption[static_cast<size_t>(item)] : -1; }

    void apply(Scratch& s, const Mod& m, int owner, const Type* skill)
    {
        if (!e.conditionsHold(s, m, owner))
            return;
        Instr in;
        in.kind = Instr::Apply;
        in.pass = pass;
        in.mod = &m;
        in.owner = owner;
        in.ownerSkill = skill;
        in.option = optionOf(owner);
        in.assign = m.op == Op::PreAssign || m.op == Op::Force;
        switch (m.source)
        {
        case SourceSel::Owner:
            if (owner >= 0)
                addReadDeps(e, s, owner, m.sourceEveAttr, in.reads);
            break;
        case SourceSel::Ship:
            addReadDeps(e, s, s.ship, m.sourceEveAttr, in.reads);
            break;
        case SourceSel::OwnerCharge:
            if (owner < 0 || s.items[owner].charge < 0)
                return;  // pyfa: no charge, no amount
            addReadDeps(e, s, s.items[owner].charge, m.sourceEveAttr, in.reads);
            break;
        case SourceSel::Skill:
        case SourceSel::Buff:
            break;
        }
        resolveTargets(e, s, m, owner, skill, in.targets);
        if (in.targets.empty())
            return;
        addWrites(e, s, m, in.targets, in);
        out.push_back(std::move(in));
    }

    void special(Scratch& s, int eid, int module)
    {
        Item& it = s.items[module];
        Instr in;
        in.kind = Instr::Special;
        in.pass = pass;
        in.eid = eid;
        in.owner = module;
        in.option = optionOf(module);
        if (eid >= kEffWarfareLinkFirst && eid <= kEffWarfareLinkLast)
        {
            if (it.charge < 0)
                return;
            in.burst = true;
            for (int x = 0; x < 4; ++x)
            {
                addReadDeps(e, s, it.charge, e.eveWarfareBuffID[x], in.reads);
                addReadDeps(e, s, module, e.eveWarfareBuffValue[x], in.reads);
            }
        }
        else if (eid == kEffCapBooster)
        {
            if (it.charge < 0)
                return;
            addReadDeps(e, s, it.charge, e.eveCapacitorBonus, in.reads);
            int f = e.fieldOf(it, e.eveCapNeed);
            if (f >= 0)
                in.writes.emplace_back(module, f);
            in.assign = true;
        }
        else if (eid == kEffRAH)
        {
            for (int i = 0; i < 4; ++i)
            {
                addReadDeps(e, s, s.ship, e.eveShipArmorRes[i], in.reads);
                addReadDeps(e, s, module, e.eveArmorRes[i], in.reads);
                if (int f = e.fieldOf(s.items[s.ship], e.eveShipArmorRes[i]); f >= 0)
                    in.writes.emplace_back(s.ship, f);
                if (int f = e.fieldOf(it, e.eveArmorRes[i]); f >= 0)
                    in.writes.emplace_back(module, f);
            }
            addReadDeps(e, s, module, e.eveResistanceShift, in.reads);
        }
        else
        {
            // Repairer flags don't depend on anything: set them on the superset now.
            e.runSpecial(s, eid, module);
            return;
        }
        out.push_back(std::move(in));
    }

    void boosts(Scratch& s, uint8_t p)
    {
        // Every buff any burst in the superset could register, onto all its possible targets.
        Instr in;
        in.kind = Instr::Boosts;
        in.pass = p;
        for (int i = 0; i < s.moduleCount; ++i)
        {
            const Item& it = s.items[s.modules[i]];
            if (it.charge < 0)
                continue;
            for (int x = 0; x < 4; ++x)
            {
                int id = static_cast<int>(e.read(s, it.charge, e.eveWarfareBuffID[x], 0));
                auto mods = id ? e.buffMods.find(id) : e.buffMods.end();
                if (mods == e.buffMods.end())
                    continue;
                for (const Mod& m : mods->second)
                {
                    if (!e.conditionsHold(s, m, -1))
                        continue;
                    std::vector<int> targets;
                    resolveTargets(e, s, m, -1, nullptr, targets);
                    addWrites(e, s, m, targets, in);
                    in.assign |= m.op == Op::PreAssign || m.op == Op::Force;
                }
            }
        }
        out.push_back(std::move(in));
        pass = static_cast<uint8_t>(p + 1);
    }
};
}

SearchPlan::SearchPlan(const Engine& engine, const SearchSpec& spec)
    : d_(std::make_unique<Data>(*engine.impl_, spec))
{
    const Impl& e = d_->e;
    Data& d = *d_;
    d.base = std::make_unique<Scratch>();
    Scratch& s = *d.base;
    e.resetScratch(s);

    // Superset: ship, mode, pinned modules, then every option of every slot.
    e.addShipAndMode(s, d.spec.base);
    e.addImplantsAndBoosters(s, d.spec.base, nullptr);
    for (size_t i = 0; i < d.spec.base.modules.size(); ++i)
        e.addModule(s, d.spec.base.modules[i], static_cast<int>(i), nullptr);
    d.staticModuleCount = s.moduleCount;
    d.itemOption.assign(static_cast<size_t>(s.count), -1);
    int option = 0;
    for (const auto& slot : d.spec.slots)
    {
        d.slotFirstOption.push_back(option);
        for (const ModuleInput& mi : slot)
        {
            int mod = e.addModule(s, mi, -1, nullptr);
            d.optionModule.push_back(mod);
            d.itemOption.resize(static_cast<size_t>(s.count), option);
            ++option;
        }
    }

    // Record pyfa's evaluation order.
    std::vector<Instr> all;
    Recorder rec{e, d.itemOption, all};
    e.traverse(s, rec);
    d.stats.instructions = all.size();

    // Dynamic: the source depends on the choice, or a value it reads (or an
    // assignment it makes) involves a field some dynamic instruction modifies.
    std::vector<uint64_t> dyn(static_cast<size_t>(s.count), 0);
    auto touchesDyn = [&](const std::vector<std::pair<int, int>>& fields) {
        for (const auto& [item, f] : fields)
            if (dyn[static_cast<size_t>(item)] >> f & 1)
                return true;
        return false;
    };
    for (Instr& in : all)
        in.dynamic = in.option >= 0;
    for (bool changed = true; changed;)
    {
        changed = false;
        bool burstDynamic = false;
        for (const Instr& in : all)
            burstDynamic |= in.burst && in.dynamic;
        for (Instr& in : all)
        {
            if (!in.dynamic && (touchesDyn(in.reads) || (in.assign && touchesDyn(in.writes)) ||
                                (in.kind == Instr::Boosts && burstDynamic)))
            {
                in.dynamic = true;
                changed = true;
            }
            if (in.dynamic)
                for (const auto& [item, f] : in.writes)
                    if (!(dyn[static_cast<size_t>(item)] >> f & 1))
                    {
                        dyn[static_cast<size_t>(item)] |= uint64_t{1} << f;
                        changed = true;
                    }
        }
    }

    // Order check: a dynamic instruction reading a field that a baked instruction
    // modifies later would see that later modification; pyfa wouldn't.
    for (size_t k = 0; k < all.size(); ++k)
    {
        if (!all[k].dynamic)
            continue;
        for (size_t j = k + 1; j < all.size(); ++j)
        {
            if (all[j].dynamic)
                continue;
            bool hit = false;
            for (const auto& w : all[j].writes)
                for (const auto& r : all[k].reads)
                    hit |= w == r;
            if (hit)
            {
                ++d.stats.orderSensitive;
                break;
            }
        }
    }

    // Bake the static instructions, in order.
    for (const Instr& in : all)
    {
        if (in.dynamic)
            continue;
        switch (in.kind)
        {
        case Instr::Apply:
            if (auto amount = e.amountOf(s, *in.mod, in.owner, in.ownerSkill, 0))
                for (int t : in.targets)
                    e.applyTo(s, t, *in.mod, *amount);
            break;
        case Instr::Special:
            e.runSpecial(s, in.eid, in.owner);
            break;
        case Instr::Boosts:
            e.applyBoosts(s, in.pass);
            break;
        }
    }

    for (Instr& in : all)
        if (in.dynamic)
        {
            in.reads.clear();
            in.writes.clear();
            d.instrs.push_back(std::move(in));
        }
    d.stats.dynamic = d.instrs.size();

    // Cycle / cap / fitting values of modules whose inputs no replayed instruction
    // writes are the same in every test.
    auto anyDyn = [&](const std::vector<std::pair<int, int>>& fields) {
        for (const auto& [item, f] : fields)
            if (dyn[static_cast<size_t>(item)] >> f & 1)
                return true;
        return false;
    };
    for (int i = 0; i < s.moduleCount; ++i)
    {
        int mod = s.modules[i];
        Item& it = s.items[mod];
        std::vector<std::pair<int, int>> cycleDeps, fittingDeps;
        for (int eve : {e.eveSpeed, e.eveDuration, e.eveDurationHighIsGood, e.eveReactivation, e.eveReloadTime,
                        e.eveChargeRate, e.eveCapNeed})
            addReadDeps(e, s, mod, eve, cycleDeps);
        for (int eve : e.eveBurstDurations)
            addReadDeps(e, s, mod, eve, cycleDeps);
        if (it.charge >= 0)
            for (int eve : {e.eveCrystalsDamaged, e.eveHp, e.eveCrystalChance, e.eveCrystalDamage})
                addReadDeps(e, s, it.charge, eve, cycleDeps);
        for (int eve : {e.eveCpu, e.evePower, e.eveUpgradeCost})
            addReadDeps(e, s, mod, eve, fittingDeps);
        ++d.stats.modules;
        if (!anyDyn(cycleDeps))
        {
            it.cycle = e.cycleOf(s, mod);
            it.constCycle = true;
        }
        else
            ++d.stats.variableCycle;
        if (anyDyn(fittingDeps))
            ++d.stats.variableFitting;
        if (!anyDyn(fittingDeps))
        {
            it.cpu = e.read(s, mod, e.eveCpu, 0);
            it.power = e.read(s, mod, e.evePower, 0);
            it.calibration = e.read(s, mod, e.eveUpgradeCost, 0);
            it.constFitting = true;
        }
    }

    // Items no replayed instruction writes to are shared read-only by all tests:
    // compute every touched field now so reading them never writes the cache.
    d.dynItem.assign(static_cast<size_t>(s.count), false);
    for (int item = 0; item < s.count; ++item)
    {
        d.dynItem[static_cast<size_t>(item)] = dyn[static_cast<size_t>(item)] != 0;
        ++d.stats.items;
        if (dyn[static_cast<size_t>(item)])
        {
            ++d.stats.itemsWritten;
            continue;
        }
        const Item& it = s.items[item];
        for (int f = 0; f < it.stride; ++f)
            if (it.touched >> f & 1)
                e.value(s, item, f, 0);
    }
}

SearchPlan::~SearchPlan() = default;

SearchPlan::Stats SearchPlan::stats() const
{
    return d_->stats;
}

FitInput SearchPlan::fitFor(const int* choice) const
{
    FitInput fit = d_->spec.base;
    for (size_t slot = 0; slot < d_->spec.slots.size(); ++slot)
        if (choice[slot] >= 0)
            fit.modules.push_back(d_->spec.slots[slot][static_cast<size_t>(choice[slot])]);
    return fit;
}

TestResult SearchPlan::evaluate(const int* choice, Goal goal) const
{
    const Impl& e = d_->e;
    const Data& d = *d_;
    const Scratch& b = *d.base;
    Scratch& s = Impl::threadScratch();
    e.resetScratch(s);

    // Same item numbering as the plan. Items no replayed instruction writes are
    // read in place from the plan (all their values are already cached); written
    // ones get a private copy, with their stacking penalties.
    s.count = b.count;
    s.ship = b.ship;
    s.mode = b.mode;
    for (int i = 0; i < b.count; ++i)
        s.items.ptr[static_cast<size_t>(i)] = b.items.ptr[static_cast<size_t>(i)];
    bool live[kMaxItems];
    std::fill(live, live + b.count, false);
    auto useItem = [&](int item) {
        live[item] = true;
        if (!d.dynItem[static_cast<size_t>(item)])
            return;
        const Item& src = b.items[static_cast<size_t>(item)];
        Item& it = s.storage[static_cast<size_t>(item)];
        it = src;
        it.data = Impl::allocFields(s, it.stride);
        std::copy_n(src.data, Item::kArrays * it.stride, it.data);
        // Penalties: same order as in the plan (oldest first), relinked.
        int chain[kMaxPenalties];
        int n = 0;
        for (int p = src.penHead; p >= 0; p = b.pens[p].next)
            chain[n++] = p;
        it.penHead = -1;
        for (int k = n - 1; k >= 0; --k)
        {
            s.pens[s.penCount] = b.pens[chain[k]];
            s.pens[s.penCount].next = it.penHead;
            it.penHead = s.penCount++;
        }
        s.items.ptr[static_cast<size_t>(item)] = &it;
    };
    auto useModule = [&](int mod) {
        useItem(mod);
        s.modules[s.moduleCount++] = mod;
        if (b.items[static_cast<size_t>(mod)].charge >= 0)
            useItem(b.items[static_cast<size_t>(mod)].charge);
    };
    useItem(b.ship);
    if (b.mode >= 0)
        useItem(b.mode);
    for (int i = 0; i < b.implantCount; ++i)
    {
        useItem(b.implants[i]);
        s.implants[s.implantCount++] = b.implants[i];
    }
    for (int i = 0; i < b.boosterCount; ++i)
    {
        useItem(b.boosters[i]);
        s.boosters[s.boosterCount++] = b.boosters[i];
    }
    for (int i = 0; i < d.staticModuleCount; ++i)
        useModule(b.modules[i]);
    for (size_t slot = 0; slot < d.spec.slots.size(); ++slot)
        if (choice[slot] >= 0)
        {
            int mod = d.optionModule[static_cast<size_t>(d.slotFirstOption[slot] + choice[slot])];
            if (mod >= 0)
                useModule(mod);
        }
    s.bonusCount = b.bonusCount;
    std::copy(b.bonuses.begin(), b.bonuses.begin() + b.bonusCount, s.bonuses.begin());

    // Replay the dynamic instructions.
    for (const Instr& in : d.instrs)
    {
        if (in.option >= 0 && !live[in.owner])
            continue;  // source not chosen
        switch (in.kind)
        {
        case Instr::Apply:
            if (auto amount = e.amountOf(s, *in.mod, in.owner, in.ownerSkill, 0))
                for (int t : in.targets)
                    if (live[t])
                        e.applyTo(s, t, *in.mod, *amount);
            break;
        case Instr::Special:
            e.runSpecial(s, in.eid, in.owner);
            break;
        case Instr::Boosts:
            e.applyBoosts(s, in.pass);
            break;
        }
    }

    Impl::Cycle cycles[kMaxItems];
    Impl::Totals t;
    bool active = goal == Goal::ShieldActive || goal == Goal::ArmorActive;
    e.totals(s, cycles, t, active);

    TestResult r;
    switch (goal)
    {
    case Goal::ShieldPassive: r.goal = t.ehp[Shield]; break;
    case Goal::ArmorPassive: r.goal = t.ehp[Armor]; break;
    case Goal::HullPassive: r.goal = t.ehp[Hull]; break;
    case Goal::ShieldActive: r.goal = t.effRepair[Shield]; break;
    case Goal::ArmorActive: r.goal = t.effRepair[Armor]; break;
    }
    r.cpuUsed = t.cpu;
    r.powerUsed = t.power;
    r.calibrationUsed = t.calibration;
    r.cpuOutput = t.cpuOutput;
    r.powerOutput = t.powerOutput;
    r.calibrationOutput = t.calibrationOutput;
    r.capUsed = t.capUsed;
    r.capRecharge = t.capRecharge;
    return r;
}
}
