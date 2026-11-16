#include "FitEngine.h"

#include "PyMath.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string_view>
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
static_assert(kMaxFields <= 127, "field masks hold 128 fields, Layout::fieldOf is int8_t");
static_assert(std::size(fitschema::ShipAttrs::kFields) <= 64, "Influence::shipFields is 64-bit");

// pyfa's runTimes, the passes of Fit.calculateModifiedAttributes in order
enum Pass : uint8_t
{
    kPassEarliest,
    kPassEarly,
    kPassNormal,
    kPassLate,
    kPassCount,
};

// One bit per field of an item's layout.
struct FieldMask
{
    std::array<uint64_t, 2> w{};
    bool test(int f) const { return w[static_cast<size_t>(f >> 6)] >> (f & 63) & 1; }
    void set(int f) { w[static_cast<size_t>(f >> 6)] |= uint64_t{1} << (f & 63); }
    void reset() { w = {}; }
    bool any() const { return (w[0] | w[1]) != 0; }
};
constexpr int kMaxItems = 512;     // ship + mode + modules + their charges (search plans: every option)
constexpr int kMaxPenalties = 16384;  // plans with many weapon / charge options
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
    CtxDrone,
    CtxCount
};
constexpr const char* kContextNames[CtxCount] = {"ship", "skill", "module", "moduleCharge", "implant", "booster", "drone"};

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
    Drones,         // every drone passing the filter (pyfa fit.drones)
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
    Drone,
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
    uint8_t runTime = kPassNormal;  // Pass
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
    HotSpoolMax,
    HotSpoolStep,
    kHot
};

// A module's cycle time, reload and cap use (pyfa rawCycleTime, getCycleParameters, capUse).
struct CycleInfo
{
    float raw = 0, reactivation = 0, reload = 0, average = 0, capNeed = 0, capUse = 0;
    int numShots = 0;
};

// What an item takes from its type alone (Engine::Impl::protoOf), so setting up an
// item doesn't look its attributes up again.
struct TypeProto
{
    const Layout* layout = nullptr;
    const Group* group = nullptr;
    std::array<int, 6> reqSkills{};
    int reqCount = 0;
    bool ancillaryShield = false, isPaste = false;
    int8_t remoteLayer = -1;             // remote repairers: the layer they repair (Shield, Armor)
    bool ancillaryRemoteArmor = false;  // group Ancillary Remote Armor Repairer
    WeaponKind weapon = WeaponKind::None;
    bool fofCharge = false, breacherCharge = false;  // charges: FoF missile, breacher pod
    // Tackle on the damage target (pyfa graphs/.../cache/projected.py, calc/projected.py)
    uint8_t tackle = 0;        // TackleKind
    bool scramScript = false;  // charges: the focused warp scrambling script (HIC disruptors)
    std::vector<float> base;  // per layout field: the unmutated base value
    std::array<int8_t, kHot> hotField{};
    std::array<float, kHot> hotBase{};
    uint32_t hotHas = 0;
    std::vector<std::pair<int, const EffectInfo*>> effects;  // the type's effects the engine knows, in order
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
    int amount = 1;   // drone: drones in space
    State state = State::Online;
    bool active = true;  // implants / boosters
    const std::vector<int>* sideEffects = nullptr;  // boosters: effectIDs switched on
    const std::vector<std::pair<int, float>>* mutators = nullptr;
    const TypeProto* proto = nullptr;  // the type's own values (valid while baseType and mutators are unset)
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
    FieldMask hasPreAssign, hasIntermediary, hasForce, touched, penalized, cached;

    int numCharges = 0;  // module: charges per load, from the module and charge types (pyfa numCharges)
    int penHead = -1;    // newest of this item's stacking-penalized multipliers (Scratch::pens list)

    std::array<int8_t, kHot> hotField{};  // field index, -1 if not in the layout (then hotBase is final)
    std::array<float, kHot> hotBase{};
    uint32_t hotHas = 0;                  // the item itself has the attribute (pyfa 'x' in item.attributes)

    // Search plans: values that can't change between tests, computed at plan build.
    bool forceReload = false;  // pyfa Module.forceReload: active capacitor boosters (effect 48)
    SpoolType spoolType = SpoolType::Default;
    float spoolAmount = 0;
    bool constCycle = false;
    CycleInfo cycle;
    bool constFitting = false;
    float cpu = 0, power = 0, calibration = 0;

    uint32_t repEffects = 0;   // which repairer effects ran (RepEffect bits)
    bool ancillaryShield = false;  // module group (Remote) Ancillary Shield Booster
    bool isPaste = false;          // charge is Nanite Repair Paste
    int8_t remoteLayer = -1;             // TypeProto::remoteLayer
    bool ancillaryRemoteArmor = false;
};

struct Penalty
{
    uint16_t item;
    uint8_t field;
    uint8_t group;
    float value;
    int next;  // older penalty of the same item, -1 = none
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
    bool factorReload = true;  // FitInput::factorReload
    float spoolup = 1;         // FitInput::spoolup
    std::array<int, kMaxItems> modules;
    int moduleCount = 0;
    std::array<ModuleStats, kMaxItems> moduleStats;  // totals(): per s.modules entry
    std::array<int, 32> implants;
    int implantCount = 0;
    std::array<int, 16> boosters;
    int boosterCount = 0;
    std::array<int, 64> drones;
    int droneCount = 0;
};

uint8_t runTimeIndex(const std::string& rt)
{
    if (rt == "earliest")
        return kPassEarliest;
    if (rt == "early")
        return kPassEarly;
    if (rt == "normal" || rt.empty())
        return kPassNormal;
    if (rt == "late")
        return kPassLate;
    throw std::runtime_error("unknown effect runTime: " + rt);
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
    // Per pass: the skills' passive modifiers in traversal order (constant for the character).
    std::array<std::vector<std::pair<const Mod*, const Type*>>, kPassCount> skillMods;
    size_t compiled = 0;
    // Item prototypes per type, built on first use (thread-safe: search plans run in parallel).
    mutable std::shared_mutex protoM;
    struct ProtoKeyHash
    {
        size_t operator()(const std::pair<const Type*, const Layout*>& k) const
        {
            return std::hash<const void*>()(k.first) * 31 + std::hash<const void*>()(k.second);
        }
    };
    mutable std::unordered_map<std::pair<const Type*, const Layout*>, std::unique_ptr<TypeProto>, ProtoKeyHash> protos;

    // eve attribute IDs used by hand-written code
    int eveSpeed, eveDuration, eveDurationHighIsGood, eveReactivation, eveReloadTime, eveCapNeed, eveCapacity,
        eveVolume, eveChargeRate, eveCrystalsDamaged, eveHp, eveCrystalChance, eveCrystalDamage, eveShieldBonus,
        eveArmorDamage, eveStructureDamage, eveCapacitorBonus, eveChargedArmorMult, eveCpu, evePower, eveUpgradeCost,
        eveResistanceShift, eveSpoolMax, eveSpoolStep;
    // Damage (pyfa Module/Drone.getVolleyParameters, the damage graph's application)
    std::array<int, 4> eveDamage{};  // em, thermal, kinetic, explosive
    int eveDamageMult, eveDamageSpoolMax, eveDamageSpoolStep, eveMaxRange, eveFalloff, eveTracking, eveOptimalSig,
        eveAoeCloud, eveAoeVelocity, eveAoeDrf, eveEmpFieldRange, eveMaxVelocity, eveExplosionDelay, eveMass,
        eveAgility, eveMaxFof, eveRadius, eveMaxTargetRange;
    int effTurretFitted, effLauncherFitted, effChainLightning, effFofMissile, effDotMissile;
    int eveSpeedFactor, eveSigBonus, eveFalloffEff, eveActivationBlocked, eveWarpScrambleRange;
    std::array<int, 2> effWebModule{}, effPaintModule{}, effScram{};
    int effDisruptSphere, effScramScript, effWebDrone, effPaintDrone;
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
        eveSpoolMax = eve("repairMultiplierBonusMax");
        eveSpoolStep = eve("repairMultiplierBonusPerCycle");
        eveDamage = {eve("emDamage"), eve("thermalDamage"), eve("kineticDamage"), eve("explosiveDamage")};
        eveDamageMult = eve("damageMultiplier");
        eveDamageSpoolMax = eve("damageMultiplierBonusMax");
        eveDamageSpoolStep = eve("damageMultiplierBonusPerCycle");
        eveMaxRange = eve("maxRange");
        eveFalloff = eve("falloff");
        eveTracking = eve("trackingSpeed");
        eveOptimalSig = eve("optimalSigRadius");
        eveAoeCloud = eve("aoeCloudSize");
        eveAoeVelocity = eve("aoeVelocity");
        eveAoeDrf = eve("aoeDamageReductionFactor");
        eveEmpFieldRange = eve("empFieldRange");
        eveMaxVelocity = eve("maxVelocity");
        eveExplosionDelay = eve("explosionDelay");
        eveMass = eve("mass");
        eveAgility = eve("agility");
        eveMaxFof = eve("maxFOFTargetRange");
        eveRadius = eve("radius");
        eveMaxTargetRange = eve("maxTargetRange");
        effTurretFitted = gd.effectID("turretFitted");
        effLauncherFitted = gd.effectID("launcherFitted");
        effChainLightning = gd.effectID("ChainLightning");
        effFofMissile = gd.effectID("fofMissileLaunching");
        effDotMissile = gd.effectID("dotMissileLaunching");
        eveSpeedFactor = eve("speedFactor");
        eveSigBonus = eve("signatureRadiusBonus");
        eveFalloffEff = eve("falloffEffectiveness");
        eveActivationBlocked = eve("activationBlockedStrenght");  // (sic)
        eveWarpScrambleRange = eve("warpScrambleRange");
        effWebModule = {gd.effectID("remoteWebifierFalloff"), gd.effectID("structureModuleEffectStasisWebifier")};
        effPaintModule = {gd.effectID("remoteTargetPaintFalloff"), gd.effectID("structureModuleEffectTargetPainter")};
        effScram = {gd.effectID("warpScrambleBlockMWDWithNPCEffect"),
                    gd.effectID("structureWarpScrambleBlockMWDWithNPCEffect")};
        effDisruptSphere = gd.effectID("warpDisruptSphere");
        effScramScript = gd.effectID("shipModuleFocusedWarpScramblingScript");
        effWebDrone = gd.effectID("remoteWebifierEntity");
        effPaintDrone = gd.effectID("remoteTargetPaintEntity");
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
                  eveChargedArmorMult, eveCrystalsDamaged, eveHp, eveCrystalChance, eveCrystalDamage,
                  eveSpoolMax, eveSpoolStep};
        attrCpu = attrByName.at("cpu");
        attrPower = attrByName.at("power");
        attrCpuOutput = attrByName.at("cpuOutput");
        attrPowerOutput = attrByName.at("powerOutput");

        compileEffects();
        preMulGroup = penaltyGroupID("preMul");
        for (const Type* s : ch.skills)
            if (!s->effects.empty())
                skills.push_back(s);
        for (uint8_t pass = 0; pass < kPassCount; ++pass)
            for (const Type* skill : skills)
                for (int eid : skill->effects)
                    if (const EffectInfo* e = effect(eid); e && e->passive && e->runTime == pass)
                        for (const Mod& m : e->mods[CtxSkill])
                            skillMods[pass].emplace_back(&m, skill);
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
        else if (m.domain == "drones")
            out.target = TargetSel::Drones;
        else
            return std::nullopt;  // fighters, projected targets, ...

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
        ensure(kEffCapBooster, kPassNormal);
        for (int eid = kEffWarfareLinkFirst; eid <= kEffWarfareLinkLast; ++eid)
            ensure(eid, kPassNormal);
        for (int eid : {kEffShieldBoosting, kEffStructureRepair, kEffArmorRepair, kEffFueledShieldBoosting,
                        kEffFueledArmorRepair, kEffRAH})
            ensure(eid, kPassLate);
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
        it.kind = kind;
        const TypeProto& p = protoOf(type, layout);
        it.proto = &p;
        it.group = p.group;
        it.reqSkills = p.reqSkills;
        it.reqCount = p.reqCount;
        it.ancillaryShield = p.ancillaryShield;
        it.isPaste = p.isPaste;
        it.remoteLayer = p.remoteLayer;
        it.ancillaryRemoteArmor = p.ancillaryRemoteArmor;
        return idx;
    }

    const TypeProto& protoOf(const Type* type, const Layout* layout) const
    {
        {
            std::shared_lock lock(protoM);
            if (auto it = protos.find({type, layout}); it != protos.end())
                return *it->second;
        }
        auto p = std::make_unique<TypeProto>();
        p->layout = layout;
        p->group = gd.groupOf(*type);
        auto req = gd.requiredSkills(*type);
        p->reqCount = static_cast<int>(std::min(req.size(), p->reqSkills.size()));
        std::copy_n(req.begin(), p->reqCount, p->reqSkills.begin());
        p->ancillaryShield = p->group && (p->group->name == "Ancillary Shield Booster" ||
                                          p->group->name == "Ancillary Remote Shield Booster");
        p->isPaste = type->name == "Nanite Repair Paste";
        // pyfa Module.getRepAmountParameters (hull and capacitor transfer aren't goals)
        if (p->group)
        {
            const std::string& g = p->group->name;
            if (g == "Remote Shield Booster" || g == "Ancillary Remote Shield Booster")
                p->remoteLayer = Shield;
            else if (g == "Remote Armor Repairer" || g == "Ancillary Remote Armor Repairer" ||
                     g == "Mutadaptive Remote Armor Repairer")
                p->remoteLayer = Armor;
            p->ancillaryRemoteArmor = g == "Ancillary Remote Armor Repairer";
        }
        p->weapon = weaponKind(*type, p->group);
        p->tackle = tackleKind(*type);
        p->scramScript = effScramScript && type->hasEffect(effScramScript);
        p->fofCharge = type->hasEffect(effFofMissile);
        p->breacherCharge = type->hasEffect(effDotMissile);
        for (int eid : type->effects)
            if (const EffectInfo* e = effect(eid))
                p->effects.emplace_back(eid, e);
        Item it;  // the type alone: no base type, no mutators
        it.type = type;
        it.layout = layout;
        p->base.resize(static_cast<size_t>(layout->count));
        for (int f = 0; f < layout->count; ++f)
            p->base[static_cast<size_t>(f)] = baseOf(it, fitschema::kAttrEveID[static_cast<size_t>(layout->fields[f])]);
        for (int h = 0; h < kHot; ++h)
        {
            int eve = hotEve[static_cast<size_t>(h)];
            p->hotField[static_cast<size_t>(h)] = static_cast<int8_t>(fieldOf(it, eve));
            p->hotBase[static_cast<size_t>(h)] = baseOf(it, eve);
            if (hasBase(it, eve))
                p->hotHas |= 1u << h;
        }
        std::unique_lock lock(protoM);
        auto& slot = protos[{type, layout}];
        if (!slot)
            slot = std::move(p);  // else another thread built it first
        return *slot;
    }

    // pyfa's projected-data collection (getProjModData, getProjDroneData, getScramRange)
    enum TackleKind : uint8_t
    {
        TackleNone,
        TackleWeb,        // modules: remoteWebifierFalloff (webs, grapplers); drones: remoteWebifierEntity
        TacklePainter,    // remoteTargetPaintFalloff / remoteTargetPaintEntity
        TackleScram,      // warpScrambleBlockMWDWithNPCEffect, with activationBlockedStrenght
        TackleDisruptor,  // warpDisruptSphere: a scrambler with the focused scrambling script
    };
    uint8_t tackleKind(const Type& type) const
    {
        auto has = [&](int eid) { return eid && type.hasEffect(eid); };
        if (has(effWebModule[0]) || has(effWebModule[1]) || has(effWebDrone))
            return TackleWeb;
        if (has(effPaintModule[0]) || has(effPaintModule[1]) || has(effPaintDrone))
            return TacklePainter;
        if (has(effScram[0]) || has(effScram[1]))
            return TackleScram;
        if (has(effDisruptSphere))
            return TackleDisruptor;
        return TackleNone;
    }

    // pyfa getApplicationPerKey's weapon branches
    WeaponKind weaponKind(const Type& type, const Group* group) const
    {
        if (type.hasEffect(effChainLightning))
            return WeaponKind::Vorton;
        if (type.hasEffect(effTurretFitted))
            return WeaponKind::Turret;
        if (type.hasEffect(effLauncherFitted) || type.id == 32461)  // civilian launcher
            return WeaponKind::Launcher;
        if (!group)
            return WeaponKind::None;
        const std::string& g = group->name;
        if (g == "Smart Bomb" || g == "Structure Area Denial Module")
            return WeaponKind::Smartbomb;
        if (g == "Missile Launcher Bomb" || g == "Super Weapon" || g == "Structure Doomsday Weapon" ||
            g == "Structure Guided Bomb Launcher" || g == "Missile Launcher Defender")
            return WeaponKind::Other;
        return WeaponKind::None;
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
        if (it.proto && !it.baseType && !it.mutators)
        {
            const TypeProto& p = *it.proto;
            const int n = it.layout->count;
            std::copy_n(p.base.data(), n, it.base());
            std::fill_n(it.preInc(), n, 0.0f);
            std::fill_n(it.mult(), n, 1.0f);
            std::fill_n(it.postInc(), n, 0.0f);
            it.hotField = p.hotField;
            it.hotBase = p.hotBase;
            it.hotHas |= p.hotHas;
            return;
        }
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
        return (it.hotHas >> h & 1u) || (f >= 0 && (it.touched.test(f)));
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
        return hasBase(it, eve) || (f >= 0 && (it.touched.test(f)));
    }

    // pyfa ModifiedAttributeDict.__getitem__ / __calculateValue
    float value(Scratch& s, int item, int field, int depth) const
    {
        Item& it = s.items[item];
        // Attributes no modifier touched never reach __calculateValue: pyfa returns
        // the intermediary or original value as is - no caps, no cpu/power rounding.
        if (!(it.touched.test(field)))
            return (it.hasIntermediary.test(field)) ? it.intermediary()[field] : it.base()[field];
        if (it.cached.test(field))
            return it.cache()[field];
        float val;
        if (it.hasForce.test(field))
            val = it.force()[field];
        else
        {
            // priority: intermediary > preAssign > original
            val = (it.hasIntermediary.test(field)) ? it.intermediary()[field]
                  : (it.hasPreAssign.test(field)) ? it.preAssign()[field]
                                                    : it.base()[field];
            val += it.preInc()[field];
            val *= it.mult()[field];
            if (it.penalized.test(field))
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
        w.cached.set(field);
        return val;
    }

    // The pieces value() combines (see AttrParts); reads only.
    void attrParts(Scratch& s, int item, int field, AttrParts& out) const
    {
        out = AttrParts{};
        if (field < 0)
            return;
        out.value = value(s, item, field, 0);
        const Item& it = s.items[item];
        if (!(it.touched.test(field)))
        {
            out.base = (it.hasIntermediary.test(field)) ? it.intermediary()[field] : it.base()[field];
            return;
        }
        if (it.hasForce.test(field))
        {
            out.forced = true;
            return;
        }
        out.base = (it.hasIntermediary.test(field)) ? it.intermediary()[field]
                   : (it.hasPreAssign.test(field))  ? it.preAssign()[field]
                                                      : it.base()[field];
        out.preInc = it.preInc()[field];
        out.mult = it.mult()[field];
        out.post = it.postInc()[field];
        if (it.penalized.test(field))
        {
            for (int p = it.penHead; p >= 0; p = s.pens[p].next)
                if (s.pens[p].field == field)
                {
                    if (out.penCount == AttrParts::kMaxPens)
                    {
                        out.truncated = true;
                        break;
                    }
                    out.pens[static_cast<size_t>(out.penCount)] = s.pens[p].value;
                    out.groups[static_cast<size_t>(out.penCount++)] = s.pens[p].group;
                }
            std::reverse(out.pens.begin(), out.pens.begin() + out.penCount);
            std::reverse(out.groups.begin(), out.groups.begin() + out.penCount);
        }
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
        it.touched.set(f);
        it.cached.reset();
        switch (m.op)
        {
        case Op::PreAssign:
            it.preAssign()[f] = amount;
            it.hasPreAssign.set(f);
            break;
        case Op::PreIncrease:
            it.preInc()[f] += amount;
            break;
        case Op::PostIncrease:
            it.postInc()[f] += amount;
            break;
        case Op::Force:
            it.force()[f] = amount;
            it.hasForce.set(f);
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
                it.penalized.set(f);
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
        case TargetSel::Drones:
            for (int i = 0; i < s.droneCount; ++i)
                if (filterMatches(s, m, s.drones[i], ownerSkill))
                    applyTo(s, s.drones[i], m, *amount);
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
        // 10000). Cap boosters' forceReload is set at item setup (Item::forceReload).
        // So none of those need handling here.
        if (eid >= kEffWarfareLinkFirst && eid <= kEffWarfareLinkLast)
        {
            // for x in 1..4: if charge warfareBuff{x}ID: addCommandBonus(id, module warfareBuff{x}Value)
            if (it.charge < 0)
                return;
            for (int x = 0; x < 4; ++x)
            {
                int id = static_cast<int>(read(s, it.charge, eveWarfareBuffID[x], 0));
                if (id)
                    addCommandBonus(s, id, read(s, module, eveWarfareBuffValue[x], 0), kPassNormal);
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
                    it.hasIntermediary.set(f);
                    it.cached.reset();
                }
            }
            break;
        case kEffShieldBoosting:
        case kEffFueledShieldBoosting:
            it.repEffects |= RepShield;
            break;
        case kEffArmorRepair:
            it.repEffects |= RepArmor;
            break;
        case kEffFueledArmorRepair:
            it.repEffects |= RepArmorFueled;
            break;
        case kEffStructureRepair:
            it.repEffects |= RepHull;
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
        for (uint8_t pass = 0; pass < kPassCount; ++pass)
        {
            for (const auto& [m, skill] : skillMods[pass])
                sink.apply(s, *m, -1, skill);

            for (const auto& [eid, e] : s.items[s.ship].proto->effects)
                if (e->passive && e->runTime == pass)
                    for (const Mod& m : e->mods[CtxShip])
                        sink.apply(s, m, s.ship, nullptr);

            // Drones: passive effects, once per drone in space (pyfa Drone.calculateModifiedAttributes;
            // no drone has a 'grouped' passive effect). Fighters would follow; not modeled.
            for (int i = 0; i < s.droneCount; ++i)
            {
                int dr = s.drones[i];
                const Item& it = s.items[dr];
                for (int k = 0; k < it.amount; ++k)
                    for (const auto& [eid, e] : it.proto->effects)
                        if (e->passive && e->runTime == pass)
                            for (const Mod& m : e->mods[CtxDrone])
                                sink.apply(s, m, dr, nullptr);
            }
            for (int i = 0; i < s.boosterCount; ++i)
            {
                int b = s.boosters[i];
                const Item& it = s.items[b];
                if (!it.active)
                    continue;
                for (const auto& [eid, e] : it.proto->effects)
                    if (e->runTime == pass && (e->passive || (e->sideEffect && sideEffectOn(it, eid))))
                        for (const Mod& m : e->mods[CtxBooster])
                            sink.apply(s, m, b, nullptr);
            }
            for (int i = 0; i < s.implantCount; ++i)
            {
                int imp = s.implants[i];
                if (!s.items[imp].active)
                    continue;
                for (const auto& [eid, e] : s.items[imp].proto->effects)
                    if (e->passive && e->runTime == pass)
                        for (const Mod& m : e->mods[CtxImplant])
                            sink.apply(s, m, imp, nullptr);
            }

            for (int i = 0; i < s.moduleCount; ++i)
            {
                int mod = s.modules[i];
                const Item& it = s.items[mod];
                if (it.charge >= 0)
                    for (const auto& [eid, e] : s.items[it.charge].proto->effects)
                        if (e->runTime == pass && runsInState(*e, it.state))
                            for (const Mod& m : e->mods[CtxModuleCharge])
                                sink.apply(s, m, mod, nullptr);
                if (it.state >= State::Overheated)
                    for (const auto& [eid, e] : it.proto->effects)
                        if (e->overheat && e->runTime == pass)
                            for (const Mod& m : e->mods[CtxModule])
                                sink.apply(s, m, mod, nullptr);
                for (const auto& [eid, e] : it.proto->effects)
                    if (e->runTime == pass && runsInState(*e, it.state))
                    {
                        for (const Mod& m : e->mods[CtxModule])
                            sink.apply(s, m, mod, nullptr);
                        sink.special(s, eid, mod);
                    }
            }

            if (s.mode >= 0)
                for (const auto& [eid, e] : s.items[s.mode].proto->effects)
                    if (e->runTime == pass)
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
        s.count = s.penCount = s.moduleCount = s.bonusCount = s.implantCount = s.boosterCount = s.droneCount =
            s.arenaUsed = 0;
        s.ship = s.mode = -1;
        s.factorReload = true;
        s.spoolup = 1;
    }

    void addShipAndMode(Scratch& s, const FitInput& in) const
    {
        const Type* shipType = gd.type(in.shipTypeID);
        if (!shipType)
            throw std::invalid_argument("unknown ship type " + std::to_string(in.shipTypeID));
        s.ship = addItem(s, shipType, &shipLayout, ItemKind::Ship);
        initValues(s.items[s.ship]);
        // pyfa Ship.EXTRA_ATTRIBUTES (not in eve.db)
        if (int f = shipLayout.fieldOf[static_cast<size_t>(Attr::droneControlRange)]; f >= 0)
            s.items[s.ship].base()[f] = 20000.0f;
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

    // Adds the fit's drones in space (amount > 0).
    void addDrones(Scratch& s, const FitInput& in, std::vector<std::string>* warnings) const
    {
        for (const DroneInput& di : in.drones)
        {
            const Type* t = gd.type(di.typeID);
            if (di.amount <= 0)
                continue;
            if (!t || s.droneCount >= static_cast<int>(s.drones.size()))
            {
                if (warnings)
                    warnings->push_back("drone " + std::to_string(di.typeID) + " skipped");
                continue;
            }
            auto l = layoutByGroup.find(t->groupID);
            int item = addItem(s, t, l == layoutByGroup.end() ? &emptyLayout : l->second, ItemKind::Drone);
            Item& it = s.items[item];
            it.amount = di.amount;
            it.baseType = di.baseTypeID ? gd.type(di.baseTypeID) : nullptr;
            it.mutators = di.mutators.empty() ? nullptr : &di.mutators;
            initValues(it);
            s.drones[s.droneCount++] = item;
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
        it.spoolType = mi.spoolType;
        it.spoolAmount = mi.spoolAmount;
        // pyfa effect 48 (powerBooster) sets forceReload when it runs, i.e. when active
        it.forceReload = mi.state >= State::Active && t->hasEffect(kEffCapBooster);
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
        CapSums cap;
        std::array<float, LayerCount> hp{}, ehp{}, divider{}, repair{}, effRepair{}, remoteRepair{};
        std::array<std::array<BalancedParts, 2>, 2> balanced{};
        float dps = 0, appliedDps = 0;            // modules + drones
        float droneDps = 0, droneAppliedDps = 0;  // the drones' part
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
            // pyfa getCycleParameters: reload factored when the fit says so (capacitor
            // boosters always), and only if the module has to reload in active time.
            if (!(s.factorReload || it.forceReload) || c.numShots == 0 || c.reactivation >= c.reload)
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

    // ------------------------------------------------------------ damage
    // pyfa Module/Drone.getVolleyParameters + getDps, and the damage graph's application
    // (graphs/data/fitDamageStats/calc/application.py) with the attacker stationary and
    // the target moving fully transversal. In double like pyfa.

    // eos.calc.calculateRangeFactor
    static double rangeFactor(double optimal, double falloff, double distance, bool restricted)
    {
        if (falloff > 0)
        {
            if (restricted && distance > optimal + 3 * falloff)
                return 0;
            double x = std::max(0.0, distance - optimal) / falloff;
            return std::pow(0.5, x * x);
        }
        return distance <= optimal ? 1 : 0;
    }

    // _calcTurretMult: average damage of the hits, wrecking shots included
    static double turretDamageMult(double chance)
    {
        double wrecking = std::min(chance, 0.01);
        double normal = chance - wrecking;
        double normalPart = normal > 0 ? normal * ((0.01 + chance) / 2 + 0.49) : 0;
        return normalPart + wrecking * 3;
    }

    // _calcTurretChanceToHit, attacker speed 0, target angle 90°
    static double turretChance(double tracking, double optimalSig, double optimal, double falloff, double atkRadius,
                               double distance, const DamageTarget& t)
    {
        double tgtSpeed = static_cast<double>(t.maxVelocity) * t.speedFactor;
        double ctc = atkRadius + distance + t.radius;
        double angular = ctc == 0 ? (tgtSpeed == 0 ? 0 : std::numeric_limits<double>::infinity()) : tgtSpeed / ctc;
        double range = rangeFactor(optimal, falloff, distance, false);
        double sig = t.signatureRadius;
        double trackingFactor;
        if (angular == 0 || std::isinf(sig))
            trackingFactor = 1;
        else if (tracking <= 0 || sig <= 0)
            trackingFactor = 0;
        else
        {
            double x = angular * optimalSig / (tracking * sig);
            trackingFactor = std::pow(0.5, x * x);
        }
        return range * trackingFactor;
    }

    // _calcMissileFactor
    static double missileFactor(double er, double ev, double drf, const DamageTarget& t)
    {
        double sig = t.signatureRadius, tgtSpeed = static_cast<double>(t.maxVelocity) * t.speedFactor;
        if (std::isinf(sig))
            return 1;
        double f = 1;
        if (er > 0)
            f = std::min(f, sig / er);
        if (tgtSpeed > 0)
            f = er > 0 ? std::min(f, std::pow(ev * sig / (er * tgtSpeed), drf)) : f;
        return f;
    }

    // pyfa Module.missileMaxRangeData: (lower range, higher range, chance of the higher one)
    bool missileRanges(Scratch& s, int mod, double& lower, double& higher, double& higherChance) const
    {
        int charge = s.items[mod].charge;
        if (charge < 0)
            return false;
        double velocity = read(s, charge, eveMaxVelocity, 0);
        if (velocity == 0)
            return false;
        double shipRadius = read(s, s.ship, eveRadius, 0);
        double flightTime = pymath::floatUnerr(read(s, charge, eveExplosionDelay, 0) / 1000.0 + shipRadius / velocity);
        double mass = read(s, charge, eveMass, 0), agility = read(s, charge, eveAgility, 0);
        auto range = [&](double time) {
            double accel = std::min(time, mass * agility / 1000000.0);
            return velocity / 2 * accel + velocity * (time - accel);
        };
        double lowerTime = std::floor(flightTime), higherTime = std::ceil(flightTime);
        lower = range(lowerTime);
        higher = range(higherTime);
        if (s.items[charge].proto->fofCharge)
            if (double limit = read(s, charge, eveMaxFof, 0); limit != 0)
            {
                lower = std::min(lower, limit);
                higher = std::min(higher, limit);
            }
        lower = std::max(0.0, lower - shipRadius);
        higher = std::max(0.0, higher - shipRadius);
        higherChance = flightTime - lowerTime;
        return true;
    }

    // eos.calc.calculateMultiplier for one stacking group: bonuses and penalties separately,
    // strongest first, 1 + (m - 1) * exp(-i^2 / 7.1289).
    static double stackedMultiplier(const double* mults, int n)
    {
        double up[64], down[64];
        int nu = 0, nd = 0;
        for (int i = 0; i < n && i < 64; ++i)
        {
            if (mults[i] > 1)
                up[nu++] = mults[i];
            else if (mults[i] < 1)
                down[nd++] = mults[i];
        }
        auto strongest = [](double a, double b) { return std::abs(a - 1) > std::abs(b - 1); };
        std::stable_sort(up, up + nu, strongest);
        std::stable_sort(down, down + nd, strongest);
        double v = 1;
        for (int i = 0; i < nu; ++i)
            v *= 1 + (up[i] - 1) * std::exp(-static_cast<double>(i * i) / 7.1289);
        for (int i = 0; i < nd; ++i)
            v *= 1 + (down[i] - 1) * std::exp(-static_cast<double>(i * i) / 7.1289);
        return v;
    }

    // pyfa Module.maxRange for tackle modules: maxRange, else warpScrambleRange (HIC disruptors)
    double tackleRange(Scratch& s, int mod) const
    {
        double r = read(s, mod, eveMaxRange, 0);
        return r != 0 ? r : read(s, mod, eveWarpScrambleRange, 0);
    }
    // pyfa Module.falloff (falloffEffectiveness, else falloff) and Drone.falloff (the other way round)
    double moduleFalloff(Scratch& s, int mod) const
    {
        double f = read(s, mod, eveFalloffEff, 0);
        return f != 0 ? f : read(s, mod, eveFalloff, 0);
    }
    double droneFalloff(Scratch& s, int drone) const
    {
        double f = read(s, drone, eveFalloff, 0);
        return f != 0 ? f : read(s, drone, eveFalloffEff, 0);
    }

    // The damage target as the fit's tackle leaves it: pyfa getScramRange, getTackledSpeed and
    // getSigRadiusMult (graphs/data/fitDamageStats/calc/projected.py, fighters aside). Speed
    // becomes the tackled speed (speedFactor folded in), signature the painted one.
    DamageTarget tackled(Scratch& s, const DamageTarget& t) const
    {
        DamageTarget out = t;
        out.speedFactor = 1;
        const double d = t.distance;
        const double untackled = static_cast<double>(t.maxVelocity) * t.speedFactor;
        out.maxVelocity = static_cast<float>(untackled);
        if (t.immune)
            return out;
        const bool inLock = d <= shipValue(s, Attr::maxTargetRange);
        const bool inDrone = d <= shipValue(s, Attr::droneControlRange);
        const double atkRadius = read(s, s.ship, eveRadius, 0);

        // Scrambled: the target's MWD / MJD off
        double scramRange = -1;
        for (int i = 0; i < s.moduleCount; ++i)
        {
            const Item& it = s.items[s.modules[i]];
            if (it.state < State::Active)
                continue;
            const uint8_t k = it.proto->tackle;
            const bool scram = (k == TackleScram && read(s, s.modules[i], eveActivationBlocked, 0) != 0) ||
                               (k == TackleDisruptor && it.charge >= 0 && s.items[it.charge].proto->scramScript);
            if (scram)
                scramRange = std::max(scramRange, std::max(0.0, tackleRange(s, s.modules[i])));
        }
        const bool scrammed = inLock && scramRange >= 0 && d <= scramRange;
        const double baseSpeed = scrammed && t.maxVelocityScrammed >= 0 ? t.maxVelocityScrammed : t.maxVelocity;
        const double baseSig = scrammed && t.signatureRadiusScrammed >= 0 ? t.signatureRadiusScrammed
                                                                          : t.signatureRadius;
        auto resisted = [](double mult, double resistance) {
            return resistance == 1 ? mult : (mult - 1) * resistance + 1;
        };

        struct Mobile
        {
            double boost, optimal, falloff, speed, radius;
        };
        // Webs
        double webs[64];
        int nWebs = 0;
        double current = untackled;
        if (t.maxVelocity != 0)
        {
            const double ratio = t.speedFactor;
            if (inLock)
                for (int i = 0; i < s.moduleCount && nWebs < 64; ++i)
                {
                    int mod = s.modules[i];
                    const Item& it = s.items[mod];
                    if (it.state < State::Active || it.proto->tackle != TackleWeb)
                        continue;
                    double boost = read(s, mod, eveSpeedFactor, 0) *
                                   rangeFactor(tackleRange(s, mod), moduleFalloff(s, mod), d, true);
                    if (boost != 0)
                        webs[nWebs++] = resisted(1 + boost / 100, t.webResistance);
                }
            auto speedNow = [&] { return baseSpeed * stackedMultiplier(webs, nWebs) * ratio; };
            current = speedNow();
            std::vector<Mobile> mobile;
            if (inLock && inDrone)
                for (int i = 0; i < s.droneCount; ++i)
                {
                    int dr = s.drones[i];
                    if (s.items[dr].proto->tackle != TackleWeb)
                        continue;
                    Mobile m{read(s, dr, eveSpeedFactor, 0), read(s, dr, eveMaxRange, 0), droneFalloff(s, dr),
                             read(s, dr, eveMaxVelocity, 0), read(s, dr, eveRadius, 0)};
                    for (int k = 0; k < s.items[dr].amount; ++k)
                        mobile.push_back(m);
                }
            // Within a mobile web's optimal it applies regardless
            bool any = false;
            for (size_t k = 0; k < mobile.size();)
                if (d <= mobile[k].optimal - atkRadius + mobile[k].radius)
                {
                    if (nWebs < 64)
                        webs[nWebs++] = resisted(1 + mobile[k].boost / 100, t.webResistance);
                    mobile.erase(mobile.begin() + static_cast<long>(k));
                    any = true;
                }
                else
                    ++k;
            if (any)
                current = speedNow();
            // The rest fastest first: full when faster than the target, else from the ship's center
            while (!mobile.empty())
            {
                double fastest = 0;
                for (const Mobile& m : mobile)
                    fastest = std::max(fastest, m.speed);
                for (size_t k = 0; k < mobile.size();)
                {
                    if (mobile[k].speed != fastest)
                    {
                        ++k;
                        continue;
                    }
                    const Mobile& m = mobile[k];
                    double boost = m.speed >= current
                                       ? m.boost
                                       : m.boost * rangeFactor(m.optimal, m.falloff, d + atkRadius - m.radius, true);
                    if (nWebs < 64)
                        webs[nWebs++] = resisted(1 + boost / 100, t.webResistance);
                    mobile.erase(mobile.begin() + static_cast<long>(k));
                }
                current = speedNow();
            }
            current = pymath::floatUnerr(current);
        }
        else
            current = 0;
        out.maxVelocity = static_cast<float>(current);

        // Target painters
        double paints[64];
        int nPaints = 0;
        if (inLock)
            for (int i = 0; i < s.moduleCount && nPaints < 64; ++i)
            {
                int mod = s.modules[i];
                const Item& it = s.items[mod];
                if (it.state < State::Active || it.proto->tackle != TacklePainter)
                    continue;
                double boost =
                    read(s, mod, eveSigBonus, 0) * rangeFactor(tackleRange(s, mod), moduleFalloff(s, mod), d, true);
                if (boost != 0)
                    paints[nPaints++] = resisted(1 + boost / 100, t.painterResistance);
            }
        if (inLock && inDrone)
            for (int i = 0; i < s.droneCount; ++i)
            {
                int dr = s.drones[i];
                if (s.items[dr].proto->tackle != TacklePainter)
                    continue;
                double boost = read(s, dr, eveSigBonus, 0);
                if (read(s, dr, eveMaxVelocity, 0) < current)
                    boost *= rangeFactor(read(s, dr, eveMaxRange, 0), droneFalloff(s, dr),
                                         d + atkRadius - read(s, dr, eveRadius, 0), true);
                for (int k = 0; k < s.items[dr].amount && nPaints < 64; ++k)
                    paints[nPaints++] = resisted(1 + boost / 100, t.painterResistance);
            }
        const double initSig = t.signatureRadius;
        const double modified = baseSig * stackedMultiplier(paints, nPaints);
        double mult = std::isinf(modified) && std::isinf(initSig) ? 1.0 : pymath::floatUnerr(modified / initSig);
        out.signatureRadius = static_cast<float>(initSig * mult);
        return out;
    }

    // Application multiplier of an active weapon module (getApplicationPerKey).
    double moduleApplication(Scratch& s, int mod, WeaponKind kind, const DamageTarget& t) const
    {
        const double d = t.distance;
        const bool inLock = d <= shipValue(s, Attr::maxTargetRange);
        switch (kind)
        {
        case WeaponKind::Vorton:
            if (!inLock)
                return 0;
            return rangeFactor(read(s, mod, eveMaxRange, 0), 0, d, true) *
                   missileFactor(read(s, mod, eveAoeCloud, 0), read(s, mod, eveAoeVelocity, 0),
                                 read(s, mod, eveAoeDrf, 0), t);
        case WeaponKind::Turret:
            if (!inLock)
                return 0;
            return turretDamageMult(turretChance(read(s, mod, eveTracking, 0), read(s, mod, eveOptimalSig, 0),
                                                 read(s, mod, eveMaxRange, 0), read(s, mod, eveFalloff, 0),
                                                 read(s, s.ship, eveRadius, 0), d, t));
        case WeaponKind::Launcher:
        {
            int charge = s.items[mod].charge;
            if (!inLock && !(charge >= 0 && s.items[charge].proto->fofCharge))
                return 0;
            double lower, higher, chance;
            if (!missileRanges(s, mod, lower, higher, chance))
                return 0;
            double distanceFactor = d <= lower ? 1 : d <= higher ? chance : 0;
            return distanceFactor * missileFactor(read(s, charge, eveAoeCloud, 0), read(s, charge, eveAoeVelocity, 0),
                                                  read(s, charge, eveAoeDrf, 0), t);
        }
        case WeaponKind::Smartbomb:
        {
            // Module.maxRange: the first non-zero of its range attributes
            double range = read(s, mod, eveMaxRange, 0);
            if (range == 0)
                range = read(s, mod, eveEmpFieldRange, 0);
            return range != 0 && d <= range ? 1 : 0;
        }
        default:
            return 0;
        }
    }

    // Raw and applied damage per second of an active module; averageCycle in ms (reload factored).
    void moduleDamage(Scratch& s, int mod, float averageCycle, const DamageTarget* target, ModuleStats& m) const
    {
        const Item& it = s.items[mod];
        m.weapon = it.proto->weapon;
        m.dps = m.appliedDps = 0;
        if (m.weapon == WeaponKind::None || it.state < State::Active || averageCycle <= 0)
            return;
        if (it.charge >= 0 && s.items[it.charge].proto->breacherCharge)
            return;  // damage over time against the target's HP: not modeled
        const int source = it.charge >= 0 ? it.charge : mod;
        double mult = read(s, mod, eveDamageMult, 0);
        mult *= 1 + spoolOf(s, mod, read(s, mod, eveDamageSpoolMax, 0), read(s, mod, eveDamageSpoolStep, 0),
                            cycleRaw(s, mod));
        double volley[4], total = 0;
        for (int k = 0; k < 4; ++k)
        {
            volley[k] = read(s, source, eveDamage[static_cast<size_t>(k)], 0) * mult;
            total += volley[k];
        }
        if (total <= 0)
            return;
        const double perSecond = 1000.0 / averageCycle;
        m.dps = static_cast<float>(total * perSecond);
        if (target && m.weapon != WeaponKind::Other)  // Other: raw damage only (bombs, doomsdays, defenders)
        {
            double application = pymath::floatUnerr(moduleApplication(s, mod, m.weapon, *target));
            m.appliedDps = static_cast<float>(resisted(volley, *target) * perSecond * application);
        }
    }

    static double resisted(const double* volley, const DamageTarget& t)
    {
        return volley[0] * (1 - static_cast<double>(t.em)) + volley[1] * (1 - static_cast<double>(t.thermal)) +
               volley[2] * (1 - static_cast<double>(t.kinetic)) + volley[3] * (1 - static_cast<double>(t.explosive));
    }

    // All drones of one item together: raw and applied damage per second.
    void droneDamage(Scratch& s, int drone, const DamageTarget* target, float& dps, float& applied) const
    {
        dps = applied = 0;
        const Item& it = s.items[drone];
        // Drone.cycleTime: speed, else duration, else durationHighisGood
        double cycle = readHot(s, drone, HotSpeed);
        if (cycle == 0)
            cycle = readHot(s, drone, HotDuration);
        if (cycle == 0)
            cycle = readHot(s, drone, HotDurationHigh);
        if (cycle <= 0)
            return;
        double mult = it.amount * static_cast<double>(read(s, drone, eveDamageMult, 0));
        double volley[4], total = 0;
        for (int k = 0; k < 4; ++k)
        {
            volley[k] = read(s, drone, eveDamage[static_cast<size_t>(k)], 0) * mult;
            total += volley[k];
        }
        if (total <= 0)
            return;
        const double perSecond = 1000.0 / cycle;
        dps = static_cast<float>(total * perSecond);
        if (!target)
            return;
        const DamageTarget& t = *target;
        const double d = t.distance;
        // getApplicationPerKey + getDroneMult: lock range and drone control range, then the
        // drone catches up with the target (mobileDroneMode auto) or shoots from the ship
        if (d > shipValue(s, Attr::droneControlRange) || d > shipValue(s, Attr::maxTargetRange))
            return;
        double droneSpeed = read(s, drone, eveMaxVelocity, 0);
        double chance;
        if (droneSpeed > 1 && droneSpeed >= static_cast<double>(t.maxVelocity) * t.speedFactor)
            chance = 1;
        else
        {
            double droneRadius = read(s, drone, eveRadius, 0);
            chance = turretChance(read(s, drone, eveTracking, 0), read(s, drone, eveOptimalSig, 0),
                                  read(s, drone, eveMaxRange, 0), read(s, drone, eveFalloff, 0), droneRadius,
                                  d + read(s, s.ship, eveRadius, 0) - droneRadius, t);
        }
        double application = pymath::floatUnerr(turretDamageMult(chance));
        applied = static_cast<float>(resisted(volley, t) * perSecond * application);
    }

    // pyfa resolveSpoolOptions + calculateSpoolup: the module's own setting, else the fit's default.
    double spoolOf(const Scratch& s, int mod, double maxValue, double stepValue, double rawCycleMs) const
    {
        const Item& it = s.items[mod];
        if (it.spoolType == SpoolType::Default)
            return spoolBonus(maxValue, stepValue, rawCycleMs / 1000.0, SpoolType::SpoolScale, s.spoolup);
        return spoolBonus(maxValue, stepValue, rawCycleMs / 1000.0, it.spoolType, it.spoolAmount);
    }

    // pyfa Module.rawCycleTime
    double cycleRaw(Scratch& s, int mod) const
    {
        return std::max({readHot(s, mod, HotSpeed), readHot(s, mod, HotDuration), readHot(s, mod, HotDurationHigh),
                         readHot(s, mod, HotBurst0), readHot(s, mod, HotBurst1), readHot(s, mod, HotBurst2),
                         readHot(s, mod, HotBurst3)});
    }

    // A module's inputs to the totals, from its final attributes.
    void moduleStats(Scratch& s, int mod, ModuleStats& m, bool damage, const DamageTarget* target) const
    {
        const Item& it = s.items[mod];
        Cycle computed;
        if (!it.constCycle)
            computed = cycleOf(s, mod);
        const Cycle& c = it.constCycle ? it.cycle : computed;
        m.state = it.state;
        m.rawCycle = c.raw;
        m.reactivation = c.reactivation;
        m.reload = c.reload;
        m.averageCycle = c.average;
        m.capNeed = c.capNeed;
        m.capUse = c.capUse;
        m.numShots = c.numShots;
        m.factorReload = s.factorReload;
        m.cpu = m.power = m.calibration = 0;
        m.repEffects = 0;
        if (it.state >= State::Online)
        {
            // pyfa getItemAttrOnlineSum
            if (it.constFitting)
            {
                m.cpu = it.cpu;
                m.power = it.power;
                m.calibration = it.calibration;
            }
            else
            {
                m.cpu = readHot(s, mod, HotCpu);
                m.power = readHot(s, mod, HotPower);
                m.calibration = readHot(s, mod, HotUpgradeCost);
            }
        }
        if (it.repEffects)
        {
            m.repEffects = static_cast<uint8_t>(it.repEffects);
            m.ancillaryShield = it.ancillaryShield;
            m.hasCharge = it.charge >= 0;
            m.pasteCharge = it.charge >= 0 && s.items[it.charge].isPaste;
            m.duration = readHot(s, mod, HotDuration);
            m.shieldBonus = (it.repEffects & RepShield) ? readHot(s, mod, HotShieldBonus) : 0.0f;
            m.armorDamage = (it.repEffects & (RepArmor | RepArmorFueled)) ? readHot(s, mod, HotArmorDamage) : 0.0f;
            m.structureDamage = (it.repEffects & RepHull) ? readHot(s, mod, HotStructureDamage) : 0.0f;
            m.chargedArmorMult = readHot(s, mod, HotChargedArmorMult);
        }
        m.remoteLayer = -1;
        m.remoteAmount = 0;
        if (it.remoteLayer >= 0 && it.state >= State::Active)
        {
            // pyfa Module.getRepAmountParameters at full spool-up (globalDefaultSpoolupPercentage 1).
            double amount;
            if (it.remoteLayer == Shield)
                amount = readHot(s, mod, HotShieldBonus);
            else
            {
                double mult = 1;
                if (it.ancillaryRemoteArmor && it.charge >= 0 && hasHot(s, mod, HotChargedArmorMult))
                    mult = readHot(s, mod, HotChargedArmorMult);
                amount = readHot(s, mod, HotArmorDamage) * mult;
            }
            amount *= 1 + spoolOf(s, mod, readHot(s, mod, HotSpoolMax), readHot(s, mod, HotSpoolStep), c.raw);
            m.remoteLayer = it.remoteLayer;
            m.remoteAmount = static_cast<float>(amount);
        }
        m.weapon = WeaponKind::None;
        m.dps = m.appliedDps = 0;
        if (damage)
            moduleDamage(s, mod, c.average, target, m);
    }

    // Everything derived from the final attributes. s.moduleStats[i] belongs to s.modules[i].
    // balancedMask: the balanced goals' tests to run, bit 2 * local layer + remote layer.
    // damage: also the damage per second (applied when target is given).
    void totals(Scratch& s, Totals& t, bool withRepair, unsigned balancedMask = 0, bool damage = false,
                const DamageTarget* target = nullptr) const
    {
        t = Totals{};
        ModuleStats* stats = s.moduleStats.data();
        DamageTarget tackledTarget;
        if (damage && target)
        {
            tackledTarget = tackled(s, *target);
            target = &tackledTarget;
        }
        if (damage)
            for (int i = 0; i < s.droneCount; ++i)
            {
                float dps, applied;
                droneDamage(s, s.drones[i], target, dps, applied);
                t.droneDps += dps;
                t.droneAppliedDps += applied;
            }
        t.dps = t.droneDps;
        t.appliedDps = t.droneAppliedDps;
        for (int i = 0; i < s.moduleCount; ++i)
        {
            moduleStats(s, s.modules[i], stats[i], damage, target);
            t.dps += stats[i].dps;
            t.appliedDps += stats[i].appliedDps;
            if (stats[i].state < State::Online)
                continue;
            t.cpu += stats[i].cpu;
            t.power += stats[i].power;
            t.calibration += stats[i].calibration;
        }
        t.cpu = pymath::round(t.cpu, 2);
        t.power = pymath::round(t.power, 2);
        t.cpuOutput = shipValue(s, Attr::cpuOutput);
        t.powerOutput = shipValue(s, Attr::powerOutput);
        t.calibrationOutput = shipValue(s, Attr::upgradeCapacity);

        // pyfa __generateDrain + calculateCapRecharge
        t.capPeak = capPeakRecharge(shipValue(s, Attr::capacitorCapacity), shipValue(s, Attr::rechargeRate));
        t.cap = capSums(stats, s.moduleCount);
        t.capUsed = t.cap.used;
        t.capRecharge = t.cap.added + t.capPeak;

        static const Attr hpAttr[LayerCount] = {Attr::shieldCapacity, Attr::armorHP, Attr::hp};
        static const Attr resAttr[LayerCount][4] = {
            {Attr::shieldEmDamageResonance, Attr::shieldThermalDamageResonance, Attr::shieldKineticDamageResonance,
             Attr::shieldExplosiveDamageResonance},
            {Attr::armorEmDamageResonance, Attr::armorThermalDamageResonance, Attr::armorKineticDamageResonance,
             Attr::armorExplosiveDamageResonance},
            {Attr::emDamageResonance, Attr::thermalDamageResonance, Attr::kineticDamageResonance,
             Attr::explosiveDamageResonance}};
        for (int l = 0; l < LayerCount; ++l)
        {
            float divider = 0;
            for (Attr a : resAttr[l])
                divider += kUniformShare * shipValue(s, a);
            if (divider == 0)
                divider = 1;
            t.divider[l] = divider;
            t.hp[l] = shipValue(s, hpAttr[l]);
            t.ehp[l] = t.hp[l] / divider;
        }
        if (withRepair)
        {
            t.repair = makeRepProfile(stats, s.moduleCount, t.capUsed, t.capRecharge).repairAll();
            for (int l = 0; l < LayerCount; ++l)
                t.effRepair[l] = t.repair[l] / t.divider[l];
            for (int l : {Shield, Armor})
                t.remoteRepair[static_cast<size_t>(l)] =
                    makeRemoteProfile(stats, s.moduleCount, t.capUsed, t.capRecharge, l).repair(l);
            for (int local : {Shield, Armor})
                for (int remote : {Shield, Armor})
                    if (balancedMask >> (2 * local + remote) & 1u)
                        t.balanced[static_cast<size_t>(local)][static_cast<size_t>(remote)] =
                            balancedParts(stats, s.moduleCount, t.capUsed, t.capRecharge, local, remote,
                                          t.divider[static_cast<size_t>(local)]);
        }
    }

    // ------------------------------------------------------------ full evaluation

    static Scratch& threadScratch()
    {
        thread_local std::unique_ptr<Scratch> scratch;
        if (!scratch)
            scratch = std::make_unique<Scratch>();
        return *scratch;
    }

    FitResult evaluate(const FitInput& in, const DamageTarget* target) const
    {
        Scratch& s = threadScratch();
        resetScratch(s);
        FitResult out;
        s.factorReload = in.factorReload;
        s.spoolup = in.spoolup;
        addShipAndMode(s, in);
        addImplantsAndBoosters(s, in, &out.warnings);
        addDrones(s, in, &out.warnings);
        for (size_t i = 0; i < in.modules.size(); ++i)
            addModule(s, in.modules[i], static_cast<int>(i), &out.warnings);

        run(s);

        for (int f = 0; f < shipLayout.count; ++f)
            out.ship[static_cast<size_t>(f)] = value(s, s.ship, f, 0);

        Totals t;
        totals(s, t, true, 15u, true, target);

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
            const ModuleStats& ms = s.moduleStats[static_cast<size_t>(i)];
            r.rawCycleTime = ms.rawCycle;
            r.reactivationDelay = ms.reactivation;
            r.reloadTime = ms.reload;
            r.averageCycleTime = ms.averageCycle;
            r.capUse = ms.capUse;
            r.numShots = ms.numShots;
            r.dps = ms.dps;
            r.appliedDps = ms.appliedDps;
        }
        DamageTarget tackledTarget;
        const DamageTarget* droneTarget = target;
        if (target)
        {
            tackledTarget = tackled(s, *target);
            droneTarget = &tackledTarget;
        }
        for (int i = 0; i < s.droneCount; ++i)
        {
            DroneResult dr;
            dr.typeID = s.items[s.drones[i]].type->id;
            dr.amount = s.items[s.drones[i]].amount;
            droneDamage(s, s.drones[i], droneTarget, dr.dps, dr.appliedDps);
            out.drones.push_back(dr);
        }
        out.dps = t.dps;
        out.appliedDps = t.appliedDps;
        out.cpuUsed = t.cpu;
        out.powerUsed = t.power;
        out.calibrationUsed = t.calibration;
        out.capPeakRecharge = t.capPeak;
        out.capRecharge = t.capRecharge;
        out.capUsed = t.capUsed;
        out.hp = t.hp;
        out.ehp = t.ehp;
        out.divider = t.divider;
        out.repair = t.repair;
        out.effectiveRepair = t.effRepair;
        out.remoteRepair = t.remoteRepair;
        out.balanced = t.balanced;
        return out;
    }
};

Engine::Engine(const GameData& gd, const Character& character) : impl_(std::make_unique<Impl>(gd, character)) {}
Engine::~Engine() = default;

FitResult Engine::evaluate(const FitInput& fit, const DamageTarget* target) const
{
    return impl_->evaluate(fit, target);
}

size_t Engine::compiledModifierCount() const
{
    return impl_->compiled;
}

Engine::Reach Engine::reach(int typeID, bool asCharge) const
{
    Reach r;
    const Type* t = impl_->gd.type(typeID);
    if (!t)
        return r;
    if (!asCharge)
    {
        r.weapon = impl_->weaponKind(*t, impl_->gd.groupOf(*t));
        r.tackle = impl_->tackleKind(*t) != 0;
    }
    for (int eid : t->effects)
    {
        const EffectInfo* e = impl_->effect(eid);
        if (!e)
            continue;
        r.activatable |= e->active;
        bool code = eid == kEffShieldBoosting || eid == kEffCapBooster || eid == kEffStructureRepair ||
                    eid == kEffArmorRepair || eid == kEffRAH || eid == kEffFueledShieldBoosting ||
                    eid == kEffFueledArmorRepair || (eid >= kEffWarfareLinkFirst && eid <= kEffWarfareLinkLast);
        r.special |= code;
        for (const Mod& m : e->mods[asCharge ? CtxModuleCharge : CtxModule])
        {
            switch (m.target)
            {
            case TargetSel::Ship: r.ship = true; break;
            case TargetSel::Self:
            case TargetSel::SelfCharge: r.self = true; break;
            default: r.others = true; break;
            }
        }
    }
    return r;
}

TestResult toTestResult(const FitResult& r, const Objective& objective)
{
    TestResult t;
    if (isBalancedGoal(objective.goal))
    {
        const BalancedParts& b = r.balanced[static_cast<size_t>(balancedLocalLayer(objective.goal))]
                                           [static_cast<size_t>(balancedRemoteLayer(objective.goal))];
        t.localRep = b.local;
        t.remoteRep = b.remote;
        t.goal = static_cast<float>(balancedScore(b.local, b.remote, objective.remoteWeight));
    }
    switch (objective.goal)
    {
    case Goal::ShieldPassive: t.goal = r.ehp[Shield]; break;
    case Goal::ArmorPassive: t.goal = r.ehp[Armor]; break;
    case Goal::HullPassive: t.goal = r.ehp[Hull]; break;
    case Goal::ShieldActive: t.goal = r.effectiveRepair[Shield]; break;
    case Goal::ArmorActive: t.goal = r.effectiveRepair[Armor]; break;
    case Goal::ShieldResists: t.goal = 1 / r.divider[Shield]; break;
    case Goal::ArmorResists: t.goal = 1 / r.divider[Armor]; break;
    case Goal::ShieldRemote: t.goal = r.remoteRepair[Shield]; break;
    case Goal::ArmorRemote: t.goal = r.remoteRepair[Armor]; break;
    case Goal::AppliedDps: t.goal = r.appliedDps; break;  // r evaluated with objective.target
    default: break;
    }
    t.dps = r.dps;
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

// ==================================================================== shared totals

double spoolBonus(double maxValue, double stepValue, double cycleSeconds, SpoolType type, double amount)
{
    if (!maxValue || !stepValue)
        return 0;
    const double toMax = std::ceil(pymath::floatUnerr(maxValue / stepValue));
    double cycles;
    switch (type)
    {
    case SpoolType::CycleScale:
        cycles = std::nearbyint(amount * toMax);  // Python round(): half to even
        break;
    case SpoolType::Time:
        if (cycleSeconds <= 0)
            return 0;
        cycles = std::min(std::floor(pymath::floatUnerr(amount / cycleSeconds)), toMax);
        break;
    case SpoolType::Cycles:
        cycles = std::min(std::floor(amount), toMax);
        break;
    default:  // SpoolScale (and Default: the caller resolves it)
        cycles = std::ceil(pymath::floatUnerr(maxValue * amount / stepValue));
        break;
    }
    return std::min(maxValue, cycles * stepValue);
}

CapSums capSums(const ModuleStats* modules, int count)
{
    CapSums out;
    for (int i = 0; i < count; ++i)
    {
        const ModuleStats& m = modules[i];
        if (m.state < State::Active || m.capNeed == 0 || m.rawCycle + m.reactivation <= 0)
            continue;
        if (m.capUse > 0)
            out.used += m.capUse;
        else
            out.added -= m.capUse;
    }
    return out;
}

float capPeakRecharge(float capacity, float rechargeRateMs)
{
    float rechargeSeconds = rechargeRateMs / 1000.0f;
    return 10.0f / rechargeSeconds * std::sqrt(0.25f) * (1 - std::sqrt(0.25f)) * capacity;
}

// Port of pyfa Fit.calculateSustainableTank (local repairers only, reload factored),
// up to the loop that shares the capacitor between repairers (RepProfile::repair).
// In double: the adjustment subtracts and re-adds large, nearly equal terms.
RepProfile makeRepProfile(const ModuleStats* modules, int count, float capUsed, float capRecharge)
{
    RepProfile p;
    p.capRecharge = capRecharge;
    auto pasteMultiplier = [](const ModuleStats& m) {
        if (!m.pasteCharge)
            return 1.0;
        double mult = m.chargedArmorMult;
        return mult ? mult : 1.0;
    };
    // Amount of a repairer for a layer, 0 if it doesn't repair it (pyfa's contributors).
    auto amountFor = [](const ModuleStats& m, int layer, double& rps) {
        double speed = m.duration / 1000.0;
        double amount = 0;
        rps = 0;
        if (layer == Shield && (m.repEffects & RepShield))
        {
            amount = m.shieldBonus;
            rps = amount / speed;
        }
        else if (layer == Armor && (m.repEffects & (RepArmor | RepArmorFueled)))
        {
            bool paste = (m.repEffects & RepArmorFueled) && m.pasteCharge;
            amount = m.armorDamage;
            rps = amount * (paste ? 3 : 1) / speed;
        }
        else if (layer == Hull && (m.repEffects & RepHull))
        {
            amount = m.structureDamage;
            rps = amount / speed;
        }
        return amount;
    };

    struct Candidate
    {
        int module;
        uint8_t layer;
        double amount;
    };
    Candidate repairers[RepProfile::kMaxReps];
    double efficiency[RepProfile::kMaxReps];
    int repairerCount = 0;
    double used = capUsed;
    for (int layer = 0; layer < LayerCount; ++layer)
    {
        for (int i = 0; i < count; ++i)
        {
            const ModuleStats& m = modules[i];
            if (!m.repEffects)
                continue;
            double rps;
            double amount = amountFor(m, layer, rps);
            p.extra[static_cast<size_t>(layer)] += rps;
            if (rps == 0)
                continue;
            double cycle = m.rawCycle / 1000.0;
            bool usesCap = m.capUse != 0;
            if (usesCap)
                used -= m.capUse;
            if (usesCap)
            {
                p.adjust[static_cast<size_t>(layer)] -= amount * (m.hasCharge ? pasteMultiplier(m) : 1.0) / cycle;
                if (repairerCount >= RepProfile::kMaxReps)
                    throw std::runtime_error("too many repairers");
                double mult = m.chargedArmorMult;
                efficiency[repairerCount] = amount * (mult ? mult : 1.0) / m.capNeed;
                repairers[repairerCount++] = {i, static_cast<uint8_t>(layer), amount};
            }
            else if (m.ancillaryShield)
            {
                double reload = m.hasCharge && m.factorReload ? m.reload : 0.0;
                double offDuty = reload / (std::max(m.numShots, 1) * static_cast<double>(m.rawCycle) + reload);
                p.adjust[static_cast<size_t>(layer)] -= amount * offDuty / cycle;
            }
        }
    }
    p.capUsed = used;

    // Most cap-efficient first (Python sort is stable, also with reverse=True).
    int order[RepProfile::kMaxReps];
    for (int r = 0; r < repairerCount; ++r)
        order[r] = r;
    std::stable_sort(order, order + repairerCount, [&](int a, int b) { return efficiency[a] > efficiency[b]; });
    for (int k = 0; k < repairerCount; ++k)
    {
        const Candidate& c = repairers[order[k]];
        const ModuleStats& m = modules[c.module];
        RepProfile::Rep& r = p.reps[static_cast<size_t>(k)];
        r.capUse = m.capUse;
        r.amount = c.amount;
        r.layer = c.layer;
        r.hasCharge = m.hasCharge;
        r.cycleSeconds = m.rawCycle / 1000.0;
        double reload = m.hasCharge && m.factorReload ? m.reload : 0.0;
        double shots = std::max(m.numShots, 1) * static_cast<double>(m.rawCycle);
        r.onDuty = shots / (shots + reload);
        r.paste = pasteMultiplier(m);
    }
    p.repCount = repairerCount;
    return p;
}

float RepProfile::repair(int layer, double extraCapUse) const
{
    double used = capUsed + extraCapUse;
    double adj = adjust[static_cast<size_t>(layer)];
    for (int k = 0; k < repCount; ++k)
    {
        if (used > capRecharge)
            break;
        const Rep& r = reps[static_cast<size_t>(k)];
        if (r.layer == layer)
        {
            double sustainability = std::min(1.0, (capRecharge - used) / r.capUse);
            if (!r.hasCharge)
                adj += sustainability * r.amount / r.cycleSeconds;
            else
                adj += sustainability * r.amount * r.onDuty * r.paste / r.cycleSeconds;
        }
        used += r.capUse;
    }
    return static_cast<float>(extra[static_cast<size_t>(layer)] + adj);
}

std::array<float, LayerCount> RepProfile::repairAll(double extraCapUse) const
{
    std::array<float, LayerCount> out{};
    for (int l = 0; l < LayerCount; ++l)
        out[static_cast<size_t>(l)] = repair(l, extraCapUse);
    return out;
}

// Outgoing remote repair of one layer, sustained like the local tank: pyfa's
// Module.getRemoteReps (reload factored in) per repairer, the capacitor left after
// every other module shared between the layer's remote repairers, most cap-efficient
// first. Repairers that use no capacitor (charged ancillary shield boosters) count in full.
RepProfile makeRemoteProfile(const ModuleStats* modules, int count, float capUsed, float capRecharge, int layer)
{
    RepProfile p;
    p.capRecharge = capRecharge;
    double used = capUsed;
    double efficiency[RepProfile::kMaxReps];
    for (int i = 0; i < count; ++i)
    {
        const ModuleStats& m = modules[i];
        if (m.remoteLayer != layer || m.averageCycle <= 0)
            continue;
        double seconds = m.averageCycle / 1000.0;
        if (m.capUse <= 0)
        {
            p.extra[static_cast<size_t>(layer)] += m.remoteAmount / seconds;
            continue;
        }
        if (p.repCount >= RepProfile::kMaxReps)
            throw std::runtime_error("too many repairers");
        used -= m.capUse;
        RepProfile::Rep& r = p.reps[static_cast<size_t>(p.repCount)];
        r.capUse = m.capUse;
        r.amount = m.remoteAmount;
        r.onDuty = r.paste = 1;
        r.cycleSeconds = seconds;
        r.hasCharge = false;
        r.layer = static_cast<uint8_t>(layer);
        efficiency[p.repCount++] = m.remoteAmount / seconds / m.capUse;
    }
    p.capUsed = used;
    int order[RepProfile::kMaxReps];
    for (int r = 0; r < p.repCount; ++r)
        order[r] = r;
    std::stable_sort(order, order + p.repCount, [&](int a, int b) { return efficiency[a] > efficiency[b]; });
    std::array<RepProfile::Rep, RepProfile::kMaxReps> sorted = p.reps;
    for (int k = 0; k < p.repCount; ++k)
        sorted[static_cast<size_t>(k)] = p.reps[static_cast<size_t>(order[k])];
    p.reps = sorted;
    return p;
}

double localRepCap(const ModuleStats* modules, int count, int layer)
{
    const uint8_t bits = layer == Shield ? RepShield : layer == Armor ? (RepArmor | RepArmorFueled) : RepHull;
    double cap = 0;
    for (int i = 0; i < count; ++i)
        if ((modules[i].repEffects & bits) && modules[i].capUse > 0)
            cap += modules[i].capUse;
    return cap;
}

double remoteRepCap(const ModuleStats* modules, int count, int layer)
{
    double cap = 0;
    for (int i = 0; i < count; ++i)
        if (modules[i].remoteLayer == layer && modules[i].capUse > 0)
            cap += modules[i].capUse;
    return cap;
}

BalancedParts balancedParts(const ModuleStats* modules, int count, float capUsed, float capRecharge, int localLayer,
                            int remoteLayer, float localDivider)
{
    // A repairer that is off uses no capacitor: its use comes off the total.
    BalancedParts b;
    const float withoutRemote = static_cast<float>(capUsed - remoteRepCap(modules, count, remoteLayer));
    const float withoutLocal = static_cast<float>(capUsed - localRepCap(modules, count, localLayer));
    b.local = makeRepProfile(modules, count, withoutRemote, capRecharge).repair(localLayer) / localDivider;
    b.remote = makeRemoteProfile(modules, count, withoutLocal, capRecharge, remoteLayer).repair(remoteLayer);
    return b;
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

namespace
{
// Plan base scratches are ~1.3 MB: allocating (and zeroing) a fresh one per plan
// cost about a third of a plan build, so destroyed plans hand theirs back.
std::mutex scratchPoolM;
std::vector<std::unique_ptr<Scratch>> scratchPool;
constexpr size_t kScratchPoolMax = 32;

std::unique_ptr<Scratch> takeScratch()
{
    {
        std::lock_guard lock(scratchPoolM);
        if (!scratchPool.empty())
        {
            auto s = std::move(scratchPool.back());
            scratchPool.pop_back();
            return s;
        }
    }
    return std::unique_ptr<Scratch>(new Scratch);  // default-init: resetScratch makes it usable
}

void giveScratch(std::unique_ptr<Scratch> s)
{
    std::lock_guard lock(scratchPoolM);
    if (scratchPool.size() < kScratchPoolMax)
        scratchPool.push_back(std::move(s));
}
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
    std::vector<Instr> graph;       // every recorded instruction with reads/writes (keepInfluence)
    Stats stats;

    Data(const Impl& impl, const SearchSpec& sp) : e(impl), spec(sp) {}
    ~Data()
    {
        if (base)
            giveScratch(std::move(base));
    }
    Data(const Data&) = delete;
    Data& operator=(const Data&) = delete;
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
    case TargetSel::Drones:
        for (int i = 0; i < s.droneCount; ++i)
            if (e.filterMatches(s, m, s.drones[i], skill))
                out.push_back(s.drones[i]);
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
    // Module indexes for target filters: the superset has every option of every slot,
    // so scanning all modules for each modifier dominated recording.
    //   bySkill[c]: (skill typeID, s.modules index) for every skill a module (c = 0)
    //               or its charge (c = 1) requires
    //   byGroup[c]: (group name, s.modules index) of the module or its charge
    // Both sorted, so equal keys list modules in s.modules order.
    std::vector<std::pair<int, int>> bySkill[2];
    std::vector<std::pair<std::string_view, int>> byGroup[2];
    std::vector<int> cand;

    void indexModules(const Scratch& s)
    {
        for (int i = 0; i < s.moduleCount; ++i)
        {
            const Item& mod = s.items[s.modules[i]];
            for (int c = 0; c < 2; ++c)
            {
                if (c == 1 && mod.charge < 0)
                    break;
                const Item& it = c ? s.items[mod.charge] : mod;
                for (int k = 0; k < it.reqCount; ++k)
                    bySkill[c].emplace_back(it.reqSkills[static_cast<size_t>(k)], i);
                if (it.group)
                    byGroup[c].emplace_back(it.group->name, i);
            }
        }
        for (auto& v : bySkill)
        {
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
        }
        for (auto& v : byGroup)
            std::sort(v.begin(), v.end());
    }

    // resolveTargets for Modules / ModuleCharges through the indexes, when some filter
    // clause has only skill and group terms: every match satisfies one of them.
    bool moduleTargets(const Scratch& s, const Mod& m, const Type* skill, std::vector<int>& targets)
    {
        if (m.target != TargetSel::Modules && m.target != TargetSel::ModuleCharges)
            return false;
        auto indexable = [&](const Term& t) {
            return t.kind == FilterTerm::Kind::Skill || t.kind == FilterTerm::Kind::Group ||
                   (t.kind == FilterTerm::Kind::OwnerSkill && skill);
        };
        const std::vector<Term>* key = nullptr;
        for (const auto& clause : m.filter)
            if (std::all_of(clause.begin(), clause.end(), indexable) && (!key || clause.size() < key->size()))
                key = &clause;
        if (!key)
            return false;
        cand.clear();
        for (const Term& t : *key)
        {
            const int c = t.onCharge ? 1 : 0;
            if (t.kind == FilterTerm::Kind::Group)
            {
                const auto& v = byGroup[c];
                for (auto it = std::lower_bound(v.begin(), v.end(), std::make_pair(std::string_view(t.group), -1));
                     it != v.end() && it->first == t.group; ++it)
                    cand.push_back(it->second);
            }
            else
            {
                const int id = t.kind == FilterTerm::Kind::Skill ? t.id : skill->id;
                if (!id)
                    continue;  // needsSkill(0) never matches
                const auto& v = bySkill[c];
                for (auto it = std::lower_bound(v.begin(), v.end(), std::make_pair(id, -1)); it != v.end() && it->first == id;
                     ++it)
                    cand.push_back(it->second);
            }
        }
        if (key->size() > 1)
        {
            std::sort(cand.begin(), cand.end());
            cand.erase(std::unique(cand.begin(), cand.end()), cand.end());
        }
        for (int i : cand)
        {
            int mod = s.modules[i];
            if (m.target == TargetSel::ModuleCharges && s.items[mod].charge < 0)
                continue;
            if (e.filterMatches(s, m, mod, skill))
                targets.push_back(m.target == TargetSel::Modules ? mod : s.items[mod].charge);
        }
        return true;
    }

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
        if (!moduleTargets(s, m, skill, in.targets))
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
    d.base = takeScratch();
    Scratch& s = *d.base;
    e.resetScratch(s);

    // Superset: ship, mode, pinned modules, then every option of every slot.
    s.factorReload = d.spec.base.factorReload;
    s.spoolup = d.spec.base.spoolup;
    e.addShipAndMode(s, d.spec.base);
    e.addImplantsAndBoosters(s, d.spec.base, nullptr);
    e.addDrones(s, d.spec.base, nullptr);
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
    all.reserve(1024);
    Recorder rec{e, d.itemOption, all};
    rec.indexModules(s);
    e.traverse(s, rec);
    d.stats.instructions = all.size();

    // Dynamic: the source depends on the choice, or a value it reads (or an
    // assignment it makes) involves a field some dynamic instruction modifies.
    std::vector<FieldMask> dyn(static_cast<size_t>(s.count));
    auto touchesDyn = [&](const std::vector<std::pair<int, int>>& fields) {
        for (const auto& [item, f] : fields)
            if (dyn[static_cast<size_t>(item)].test(f))
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
                    if (!(dyn[static_cast<size_t>(item)].test(f)))
                    {
                        dyn[static_cast<size_t>(item)].set(f);
                        changed = true;
                    }
        }
    }

    // Order check: a dynamic instruction reading a field that a baked instruction
    // modifies later would see that later modification; pyfa wouldn't. Backwards,
    // with the fields static instructions write later.
    {
        std::vector<FieldMask> laterStatic(static_cast<size_t>(s.count));
        for (size_t k = all.size(); k-- > 0;)
        {
            const Instr& in = all[k];
            if (!in.dynamic)
            {
                for (const auto& [item, f] : in.writes)
                    laterStatic[static_cast<size_t>(item)].set(f);
                continue;
            }
            for (const auto& [item, f] : in.reads)
                if (laterStatic[static_cast<size_t>(item)].test(f))
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

    if (d.spec.keepInfluence)
        d.graph = all;
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
            if (dyn[static_cast<size_t>(item)].test(f))
                return true;
        return false;
    };
    std::vector<std::pair<int, int>> cycleDeps, fittingDeps;
    for (int i = 0; i < s.moduleCount; ++i)
    {
        int mod = s.modules[i];
        Item& it = s.items[mod];
        cycleDeps.clear();
        fittingDeps.clear();
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
        d.dynItem[static_cast<size_t>(item)] = dyn[static_cast<size_t>(item)].any();
        ++d.stats.items;
        if (dyn[static_cast<size_t>(item)].any())
        {
            ++d.stats.itemsWritten;
            continue;
        }
        const Item& it = s.items[item];
        for (int f = 0; f < it.stride; ++f)
            if (it.touched.test(f))
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

TestResult SearchPlan::evaluate(const int* choice, const Objective& objective, TestDetail* detail) const
{
    const Goal goal = objective.goal;
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
    s.factorReload = b.factorReload;
    s.spoolup = b.spoolup;
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
    for (int i = 0; i < b.droneCount; ++i)
    {
        useItem(b.drones[i]);
        s.drones[s.droneCount++] = b.drones[i];
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

    Impl::Totals t;
    const bool balanced = isBalancedGoal(goal);
    bool active = goal == Goal::ShieldActive || goal == Goal::ArmorActive || goal == Goal::ShieldRemote ||
                  goal == Goal::ArmorRemote || balanced;
    const bool damage = goal == Goal::AppliedDps;
    e.totals(s, t, active,
             balanced ? 1u << (2 * balancedLocalLayer(goal) + balancedRemoteLayer(goal)) : 0u, damage,
             damage ? &objective.target : nullptr);

    TestResult r;
    if (balanced)
    {
        const BalancedParts& parts = t.balanced[static_cast<size_t>(balancedLocalLayer(goal))]
                                               [static_cast<size_t>(balancedRemoteLayer(goal))];
        r.localRep = parts.local;
        r.remoteRep = parts.remote;
        r.goal = static_cast<float>(balancedScore(parts.local, parts.remote, objective.remoteWeight));
    }
    switch (goal)
    {
    case Goal::ShieldPassive: r.goal = t.ehp[Shield]; break;
    case Goal::ArmorPassive: r.goal = t.ehp[Armor]; break;
    case Goal::HullPassive: r.goal = t.ehp[Hull]; break;
    case Goal::ShieldActive: r.goal = t.effRepair[Shield]; break;
    case Goal::ArmorActive: r.goal = t.effRepair[Armor]; break;
    case Goal::ShieldResists: r.goal = 1 / t.divider[Shield]; break;
    case Goal::ArmorResists: r.goal = 1 / t.divider[Armor]; break;
    case Goal::ShieldRemote: r.goal = t.remoteRepair[Shield]; break;
    case Goal::ArmorRemote: r.goal = t.remoteRepair[Armor]; break;
    case Goal::AppliedDps: r.goal = t.appliedDps; break;
    default: break;
    }
    r.dps = t.dps;
    r.cpuUsed = t.cpu;
    r.powerUsed = t.power;
    r.calibrationUsed = t.calibration;
    r.cpuOutput = t.cpuOutput;
    r.powerOutput = t.powerOutput;
    r.calibrationOutput = t.calibrationOutput;
    r.capUsed = t.capUsed;
    r.capRecharge = t.capRecharge;

    if (detail)
    {
        if (s.moduleCount > TestDetail::kMaxModules)
            throw std::runtime_error("too many modules for TestDetail");
        for (int f = 0; f < e.shipLayout.count; ++f)
            detail->ship[static_cast<size_t>(f)] = e.value(s, s.ship, f, 0);
        detail->hp = t.hp;
        detail->divider = t.divider;
        detail->capPeak = t.capPeak;
        detail->cap = t.cap;
        detail->droneDps = t.droneDps;
        detail->droneAppliedDps = t.droneAppliedDps;
        detail->moduleCount = s.moduleCount;
        for (int i = 0; i < s.moduleCount; ++i)
        {
            int mod = s.modules[i];
            detail->modules[static_cast<size_t>(i)] = s.moduleStats[static_cast<size_t>(i)];
            int option = d.itemOption[static_cast<size_t>(mod)];
            detail->moduleOption[static_cast<size_t>(i)] = option >= 0 ? option : -1 - s.items[mod].input;
        }
        if (detail->wantResonanceParts)
        {
            static const Attr resAttr[LayerCount][4] = {
                {Attr::shieldEmDamageResonance, Attr::shieldThermalDamageResonance, Attr::shieldKineticDamageResonance,
                 Attr::shieldExplosiveDamageResonance},
                {Attr::armorEmDamageResonance, Attr::armorThermalDamageResonance, Attr::armorKineticDamageResonance,
                 Attr::armorExplosiveDamageResonance},
                {Attr::emDamageResonance, Attr::thermalDamageResonance, Attr::kineticDamageResonance,
                 Attr::explosiveDamageResonance}};
            for (int l = 0; l < LayerCount; ++l)
                for (int k = 0; k < 4; ++k)
                    e.attrParts(s, s.ship, e.shipLayout.fieldOf[static_cast<size_t>(resAttr[l][k])],
                                detail->resonanceParts[static_cast<size_t>(l)][static_cast<size_t>(k)]);
        }
    }
    return r;
}

int SearchPlan::optionIndex(int slot, int option) const
{
    return d_->slotFirstOption[static_cast<size_t>(slot)] + option;
}

int SearchPlan::optionCount() const
{
    return static_cast<int>(d_->optionModule.size());
}

Influence SearchPlan::influence(int slot, int option) const
{
    const Data& d = *d_;
    if (!d.spec.keepInfluence)
        throw std::logic_error("SearchPlan::influence needs SearchSpec::keepInfluence");
    const Scratch& s = *d.base;
    const int o = optionIndex(slot, option);

    // Same fixed point as the dynamic classification, seeded with this option only.
    std::vector<FieldMask> written(static_cast<size_t>(s.count));
    std::vector<char> hit(d.graph.size(), 0);
    for (bool changed = true; changed;)
    {
        changed = false;
        bool burstHit = false;
        for (size_t k = 0; k < d.graph.size(); ++k)
            burstHit |= d.graph[k].burst && hit[k];
        for (size_t k = 0; k < d.graph.size(); ++k)
        {
            const Instr& in = d.graph[k];
            if (!hit[k])
            {
                bool h = in.option == o || (in.kind == Instr::Boosts && burstHit);
                for (size_t r = 0; !h && r < in.reads.size(); ++r)
                    h = written[static_cast<size_t>(in.reads[r].first)].test(in.reads[r].second);
                if (!h)
                    continue;
                hit[k] = 1;
                changed = true;
            }
            for (const auto& [item, f] : in.writes)
                if (!(written[static_cast<size_t>(item)].test(f)))
                {
                    written[static_cast<size_t>(item)].set(f);
                    changed = true;
                }
        }
    }

    Influence out;
    for (int item = 0; item < s.count; ++item)
    {
        const FieldMask bits = written[static_cast<size_t>(item)];
        if (!bits.any())
            continue;
        const Item& it = s.items[static_cast<size_t>(item)];
        if (item == s.ship)
        {
            out.shipFields = bits.w[0];
            continue;
        }
        int itemOpt = d.itemOption[static_cast<size_t>(item)];
        int pinned = -1;
        if (itemOpt < 0)
        {
            int mod = it.kind == ItemKind::Charge ? it.owner : it.kind == ItemKind::Module ? item : -1;
            if (mod < 0)
                continue;  // implants, boosters, mode
            pinned = s.items[static_cast<size_t>(mod)].input;
        }
        else if (itemOpt == o)
            continue;
        for (int f = 0; f < it.stride; ++f)
            if (bits.test(f))
            {
                Attr a = it.layout->fields[f];
                if (itemOpt >= 0)
                    out.optionFields.emplace_back(itemOpt, a);
                else
                    out.pinnedFields.emplace_back(pinned, a);
            }
    }
    auto tidy = [](std::vector<std::pair<int, Attr>>& v) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    };
    tidy(out.optionFields);
    tidy(out.pinnedFields);
    return out;
}
}
