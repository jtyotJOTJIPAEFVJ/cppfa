#include "Request.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>

namespace request
{
using fitengine::FitInput;
using fitengine::Goal;
using fitengine::ModuleInput;
using fitengine::State;
using fitengine::TestResult;
namespace opt = fitengine::opt;

namespace
{
// Typed access to request values; errors name the field.
const char* typeName(const json::Value& v)
{
    return v.is_null() ? "null" : v.is_boolean() ? "a boolean" : v.is_number() ? "a number" : v.is_string() ? "a string"
                                  : v.is_array() ? "an array" : "an object";
}
[[noreturn]] void typeError(const std::string& what, const char* want, const json::Value& v)
{
    throw std::runtime_error(what + ": expected " + want + ", got " + typeName(v));
}
const json::Value* get(const json::Value& obj, const std::string& key)
{
    if (!obj.is_object())
        return nullptr;
    auto it = obj.find(key);
    return it == obj.end() ? nullptr : &*it;
}
bool asBool(const json::Value& v, const std::string& what)
{
    if (!v.is_boolean())
        typeError(what, "a boolean", v);
    return v.get<bool>();
}
double asNumber(const json::Value& v, const std::string& what)
{
    if (!v.is_number())
        typeError(what, "a number", v);
    return v.get<double>();
}
int asInt(const json::Value& v, const std::string& what)
{
    double d = asNumber(v, what);
    if (d != std::floor(d) || d < -2147483648.0 || d > 2147483647.0)
        throw std::runtime_error(what + ": expected an integer");
    return static_cast<int>(d);
}
const std::string& asString(const json::Value& v, const std::string& what)
{
    if (!v.is_string())
        typeError(what, "a string", v);
    return v.get_ref<const std::string&>();
}
const json::Value& asArray(const json::Value& v, const std::string& what)
{
    if (!v.is_array())
        typeError(what, "an array", v);
    return v;
}
const json::Value& asObject(const json::Value& v, const std::string& what)
{
    if (!v.is_object())
        typeError(what, "an object", v);
    return v;
}
// Engine floats as the shortest decimal that reads back as the same float.
double num(float f)
{
    char buf[32];
    auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), f);
    double d = f;
    std::from_chars(buf, end, d);
    return d;
}
// Derived quantities (sums, bounds): 4 decimals.
double round4(double v) { return std::round(v * 1e4) / 1e4; }
const std::pair<const char*, Goal> kGoals[] = {{"shieldPassive", Goal::ShieldPassive},
                                               {"armorPassive", Goal::ArmorPassive},
                                               {"hullPassive", Goal::HullPassive},
                                               {"shieldActive", Goal::ShieldActive},
                                               {"armorActive", Goal::ArmorActive},
                                               {"shieldResists", Goal::ShieldResists},
                                               {"armorResists", Goal::ArmorResists},
                                               {"shieldRemote", Goal::ShieldRemote},
                                               {"armorRemote", Goal::ArmorRemote},
                                               {"localShieldRemoteShield", Goal::LocalShieldRemoteShield},
                                               {"localShieldRemoteArmor", Goal::LocalShieldRemoteArmor},
                                               {"localArmorRemoteShield", Goal::LocalArmorRemoteShield},
                                               {"localArmorRemoteArmor", Goal::LocalArmorRemoteArmor}};

const std::pair<const char*, Slot> kSlots[] = {{"low", Slot::Low},   {"med", Slot::Med},
                                               {"high", Slot::High}, {"rig", Slot::Rig},
                                               {"subsystem", Slot::Subsystem}, {"service", Slot::Service}};

const char* slotName(Slot s)
{
    for (const auto& [n, v] : kSlots)
        if (v == s)
            return n;
    return "?";
}

Slot parseSlot(const json::Value& v, const std::string& what)
{
    if (v.is_number())
    {
        int n = asInt(v, what);
        for (const auto& [name, s] : kSlots)
            if (static_cast<int>(s) == n)
                return s;
        throw std::runtime_error(what + ": unknown slot " + std::to_string(n));
    }
    const std::string& s = asString(v, what);
    if (s == "mid")
        return Slot::Med;
    for (const auto& [name, slot] : kSlots)
        if (s == name)
            return slot;
    throw std::runtime_error(what + ": unknown slot \"" + s + "\" (low, med, high, rig, subsystem, service)");
}

const char* stateName(State s)
{
    switch (s)
    {
    case State::Offline: return "offline";
    case State::Online: return "online";
    case State::Active: return "active";
    default: return "overheated";
    }
}

State parseState(const json::Value& v, const std::string& what)
{
    if (v.is_number())
    {
        int n = asInt(v, what);
        if (n < -1 || n > 2)
            throw std::runtime_error(what + ": state must be -1..2");
        return static_cast<State>(n);
    }
    const std::string& s = asString(v, what);
    for (State st : {State::Offline, State::Online, State::Active, State::Overheated})
        if (s == stateName(st))
            return st;
    throw std::runtime_error(what + ": unknown state \"" + s + "\" (offline, online, active, overheated)");
}

const Slot kRackSlots[opt::RackCount] = {Slot::Med, Slot::Low, Slot::Rig, Slot::High};

int rackOfSlot(Slot s)
{
    switch (s)
    {
    case Slot::Med: return opt::RackMid;
    case Slot::Low: return opt::RackLow;
    case Slot::Rig: return opt::RackRig;
    case Slot::High: return opt::RackHigh;
    default: return -1;
    }
}

int getInt(const json::Value& obj, const char* key, int def, const std::string& where)
{
    const json::Value* v = get(obj, key);
    return v && !v->is_null() ? asInt(*v, where + "." + key) : def;
}
double getNumber(const json::Value& obj, const char* key, double def, const std::string& where)
{
    const json::Value* v = get(obj, key);
    return v && !v->is_null() ? asNumber(*v, where + "." + key) : def;
}
bool getBool(const json::Value& obj, const char* key, bool def, const std::string& where)
{
    const json::Value* v = get(obj, key);
    return v && !v->is_null() ? asBool(*v, where + "." + key) : def;
}

// {"<id>": value, ...} (JSON keys are strings; Python's json.dumps writes int keys so).
std::vector<std::pair<int, double>> idMap(const json::Value& v, const std::string& what)
{
    std::vector<std::pair<int, double>> out;
    for (const auto& item : asObject(v, what).items())
    {
        const std::string& k = item.key();
        const json::Value& val = item.value();
        size_t used = 0;
        int id = 0;
        try
        {
            id = std::stoi(k, &used);
        }
        catch (const std::exception&)
        {
            used = 0;
        }
        if (used != k.size())
            throw std::runtime_error(what + ": key \"" + k + "\" is not an integer ID");
        out.emplace_back(id, asNumber(val, what + "." + k));
    }
    return out;
}

void requireType(const GameData& gd, int typeID, const std::string& what)
{
    if (!gd.type(typeID))
        throw std::runtime_error(what + ": unknown typeID " + std::to_string(typeID));
}

json::Value statsJson(const TestResult& r, Goal goal)
{
    json::Value s = json::Value::object();
    s["goal"] = num(r.goal);
    if (fitengine::isBalancedGoal(goal))
    {
        s["localRep"] = num(r.localRep);
        s["remoteRep"] = num(r.remoteRep);
    }
    s["cpuUsed"] = num(r.cpuUsed);
    s["cpuOutput"] = num(r.cpuOutput);
    s["powerUsed"] = num(r.powerUsed);
    s["powerOutput"] = num(r.powerOutput);
    s["calibrationUsed"] = num(r.calibrationUsed);
    s["calibrationOutput"] = num(r.calibrationOutput);
    s["capUsed"] = num(r.capUsed);
    s["capRecharge"] = num(r.capRecharge);
    s["capStable"] = r.capUsed <= r.capRecharge;
    s["fits"] = r.cpuUsed <= r.cpuOutput && r.powerUsed <= r.powerOutput && r.calibrationUsed <= r.calibrationOutput;
    return s;
}

bool sameModule(const ModuleInput& a, const ModuleInput& b)
{
    return a.typeID == b.typeID && a.chargeID == b.chargeID && a.state == b.state && a.baseTypeID == b.baseTypeID &&
           a.mutators == b.mutators;
}
}  // namespace

const char* goalName(Goal g)
{
    for (const auto& [n, v] : kGoals)
        if (v == g)
            return n;
    return "?";
}

std::optional<Goal> parseGoal(const std::string& name)
{
    for (const auto& [n, g] : kGoals)
        if (name == n)
            return g;
    return std::nullopt;
}

Request parse(const json::Value& doc, const GameData& gd)
{
    Request req;
    asObject(doc, "request");
    req.version = getInt(doc, "version", kVersion, "request");
    if (req.version != kVersion)
        throw std::runtime_error("request.version " + std::to_string(req.version) + " is not supported (expected " +
                                 std::to_string(kVersion) + ")");

    const json::Value* ship = get(doc, "ship");
    if (!ship)
        throw std::runtime_error("request.ship (ship typeID) is missing");
    req.fit.shipTypeID = asInt(*ship, "request.ship");
    requireType(gd, req.fit.shipTypeID, "request.ship");
    req.fit.modeTypeID = getInt(doc, "mode", 0, "request");
    if (const Type* t = gd.type(req.fit.shipTypeID))
    {
        // As pyfa: a T3 destroyer always has a mode; an invalid/missing one becomes the first.
        auto modes = gd.shipModes(*t);
        if (!modes.empty() && std::find(modes.begin(), modes.end(), req.fit.modeTypeID) == modes.end())
            req.fit.modeTypeID = modes.front();
        if (modes.empty())
            req.fit.modeTypeID = 0;
    }

    if (const json::Value* ch = get(doc, "character"); ch && !ch->is_null())
    {
        asObject(*ch, "request.character");
        req.defaultSkillLevel = getInt(*ch, "defaultLevel", 5, "request.character");
        if (req.defaultSkillLevel < 0 || req.defaultSkillLevel > 5)
            throw std::runtime_error("request.character.defaultLevel must be 0..5");
        if (const json::Value* sk = get(*ch, "skills"); sk && !sk->is_null())
        {
            const auto& list = asArray(*sk, "request.character.skills");
            for (size_t i = 0; i < list.size(); ++i)
            {
                std::string w = "request.character.skills[" + std::to_string(i) + "]";
                asObject(list[i], w);
                int id = getInt(list[i], "skillID", 0, w), level = getInt(list[i], "level", -1, w);
                if (level < 0 || level > 5)
                    throw std::runtime_error(w + ".level must be 0..5");
                req.skillLevels.emplace_back(id, level);
            }
        }
    }

    if (const json::Value* imps = get(doc, "implants"); imps && !imps->is_null())
    {
        const auto& list = asArray(*imps, "request.implants");
        for (size_t i = 0; i < list.size(); ++i)
        {
            std::string w = "request.implants[" + std::to_string(i) + "]";
            asObject(list[i], w);
            fitengine::ImplantInput in;
            in.typeID = getInt(list[i], "typeID", 0, w);
            requireType(gd, in.typeID, w + ".typeID");
            in.active = getBool(list[i], "active", true, w);
            req.fit.implants.push_back(in);
        }
    }
    if (const json::Value* boosters = get(doc, "boosters"); boosters && !boosters->is_null())
    {
        const auto& list = asArray(*boosters, "request.boosters");
        for (size_t i = 0; i < list.size(); ++i)
        {
            std::string w = "request.boosters[" + std::to_string(i) + "]";
            asObject(list[i], w);
            fitengine::BoosterInput in;
            in.typeID = getInt(list[i], "typeID", 0, w);
            requireType(gd, in.typeID, w + ".typeID");
            in.active = getBool(list[i], "active", true, w);
            if (const json::Value* se = get(list[i], "sideEffects"); se && !se->is_null())
                for (const json::Value& e : asArray(*se, w + ".sideEffects"))
                    in.sideEffects.push_back(asInt(e, w + ".sideEffects[]"));
            req.fit.boosters.push_back(in);
        }
    }

    const json::Value* mods = get(doc, "modules");
    if (mods && !mods->is_null())
    {
        const auto& list = asArray(*mods, "request.modules");
        std::set<int> positions;
        for (size_t i = 0; i < list.size(); ++i)
        {
            std::string w = "request.modules[" + std::to_string(i) + "]";
            const json::Value& m = list[i];
            asObject(m, w);
            ModuleEntry e;
            const json::Value* pos = get(m, "position");
            if (!pos)
                throw std::runtime_error(w + ".position is missing");
            e.position = asInt(*pos, w + ".position");
            if (!positions.insert(e.position).second)
                throw std::runtime_error(w + ".position " + std::to_string(e.position) + " is used twice");
            e.input.typeID = getInt(m, "typeID", 0, w);
            e.optimize = getBool(m, "optimize", false, w);
            const json::Value* slot = get(m, "slot");
            if (e.input.typeID)
            {
                const Type* t = gd.type(e.input.typeID);
                if (!t)
                    throw std::runtime_error(w + ".typeID: unknown typeID " + std::to_string(e.input.typeID));
                auto s = gd.moduleSlot(*t);
                if (!s)
                    throw std::runtime_error(w + ".typeID " + std::to_string(e.input.typeID) + " (" + t->name +
                                             ") is not a module");
                e.slot = *s;
                if (slot && !slot->is_null() && parseSlot(*slot, w + ".slot") != e.slot)
                    throw std::runtime_error(w + ".slot doesn't match the module's slot (" + slotName(e.slot) + ")");
                e.input.chargeID = getInt(m, "chargeID", 0, w);
                if (e.input.chargeID)
                    requireType(gd, e.input.chargeID, w + ".chargeID");
                const json::Value* st = get(m, "state");
                e.input.state = st && !st->is_null() ? parseState(*st, w + ".state") : State::Online;
                e.input.baseTypeID = getInt(m, "baseTypeID", 0, w);
                e.mutaplasmidID = getInt(m, "mutaplasmidID", 0, w);
                if (e.input.baseTypeID)
                    requireType(gd, e.input.baseTypeID, w + ".baseTypeID");
                if (const json::Value* mu = get(m, "mutators"); mu && !mu->is_null())
                {
                    for (const auto& [attr, v] : idMap(*mu, w + ".mutators"))
                        e.input.mutators.emplace_back(attr, static_cast<float>(v));
                    std::sort(e.input.mutators.begin(), e.input.mutators.end());
                }
            }
            else
            {
                if (!slot || slot->is_null())
                    throw std::runtime_error(w + ": an empty slot needs \"slot\"");
                e.slot = parseSlot(*slot, w + ".slot");
            }
            req.entries.push_back(std::move(e));
        }
    }
    std::stable_sort(req.entries.begin(), req.entries.end(),
                     [](const ModuleEntry& a, const ModuleEntry& b) { return a.position < b.position; });
    for (const ModuleEntry& e : req.entries)
    {
        if (e.input.typeID)
        {
            req.fitIndex.push_back(req.fit.modules.size());
            req.fit.modules.push_back(e.input);
        }
        else
            req.fitIndex.push_back(std::numeric_limits<size_t>::max());
    }

    const json::Value* goal = get(doc, "goal");
    if (!goal)
        throw std::runtime_error("request.goal is missing");
    auto g = parseGoal(asString(*goal, "request.goal"));
    if (!g)
        throw std::runtime_error("request.goal: unknown goal \"" + goal->get<std::string>() +
                                 "\" (shieldPassive, armorPassive, hullPassive, shieldActive, armorActive, "
                                 "shieldResists, armorResists, shieldRemote, armorRemote, localShieldRemoteShield, "
                                 "localShieldRemoteArmor, localArmorRemoteShield, localArmorRemoteArmor)");
    req.goal = *g;
    req.priceCap = getNumber(doc, "priceCap", 0, "request");
    req.allowOfficer = getBool(doc, "allowOfficer", false, "request");
    req.allowAncillary = getBool(doc, "allowAncillary", true, "request");
    req.maxReps = getInt(doc, "maxReps", 0, "request");
    if (req.maxReps < 0)
        throw std::runtime_error("request.maxReps must be >= 0 (0 = no limit)");
    req.requireCapStable = getBool(doc, "requireCapStable", false, "request");
    req.remoteWeight = getNumber(doc, "remoteWeight", 4, "request");
    if (!(req.remoteWeight > 0))
        throw std::runtime_error("request.remoteWeight must be > 0");
    req.minCapRecharge = getNumber(doc, "minCapRecharge", 0, "request");
    if (!(req.minCapRecharge >= 0))
        throw std::runtime_error("request.minCapRecharge must be >= 0 (GJ/s, 0 = none)");
    req.results = getInt(doc, "results", 10, "request");
    if (req.results < 1)
        throw std::runtime_error("request.results must be at least 1");
    req.threads = getInt(doc, "threads", 0, "request");
    req.timeBudget = getNumber(doc, "timeBudget", 60, "request");
    if (!(req.timeBudget >= 0))
        throw std::runtime_error("request.timeBudget must be >= 0 (seconds, 0 = no limit)");
    if (const json::Value* p = get(doc, "prices"); p && !p->is_null())
    {
        req.prices.emplace();
        for (const auto& [id, price] : idMap(*p, "request.prices"))
            (*req.prices)[id] = price;
    }
    return req;
}

json::Value errorResponse(const std::string& message)
{
    json::Value r = json::Value::object();
    r["version"] = kVersion;
    r["status"] = "error";
    r["message"] = message;
    return r;
}

json::Value run(const GameData& gd, const Request& req, const std::unordered_map<int, double>* fallbackPrices,
                const RunOptions& ro, bool& ok)
{
    ok = true;
    auto t0 = std::chrono::steady_clock::now();
    const std::unordered_map<int, double>* prices = req.prices ? &*req.prices : fallbackPrices;
    if (prices && prices->empty())
        prices = nullptr;
    auto priceOf = [&](int typeID) {
        if (!prices || !typeID)
            return 0.0;
        auto it = prices->find(typeID);
        return it == prices->end() ? 0.0 : std::max(0.0, it->second);
    };

    Character ch = gd.loadCharacter(req.defaultSkillLevel);
    for (const auto& [id, level] : req.skillLevels)
        ch.levels[id] = level;
    fitengine::Engine engine(gd, ch);

    json::Value warnings = json::Value::array();
    opt::Options o;
    o.goal = ro.goal ? *ro.goal : req.goal;
    o.priceCap = req.priceCap;
    o.allowOfficer = req.allowOfficer;
    o.allowAncillary = req.allowAncillary;
    o.maxReps = req.maxReps;
    o.requireCapStable = req.requireCapStable;
    o.remoteWeight = req.remoteWeight;
    o.minCapRecharge = req.minCapRecharge;
    o.results = req.results;
    o.threads = req.threads;
    o.prices = prices;
    o.log = ro.log;
    o.progress = ro.progress;
    o.bruteForce = ro.check;
    if (ro.maxSlots)
        o.maxSlots = *ro.maxSlots;
    o.timeBudget = ro.timeBudget.value_or(req.timeBudget);
    o.refine = ro.refine.value_or(o.refine);
    o.refineRounds = ro.refineRounds.value_or(o.refineRounds);
    o.refinePatience = ro.refinePatience.value_or(o.refinePatience);
    o.refinePairs = ro.refinePairs.value_or(o.refinePairs);
    o.refineChains = ro.refineChains.value_or(o.refineChains);
    o.refineStarts = ro.refineStarts.value_or(o.refineStarts);
    o.refineExact = ro.refineExact.value_or(o.refineExact);
    o.memberBruteForce = ro.members;
    o.partial = true;
    o.openModules.assign(req.fit.modules.size(), uint8_t{0});

    // The requested slots per rack, in position order (entry indices).
    std::array<std::vector<size_t>, opt::RackCount> requested;
    double startPrice = 0;
    for (size_t i = 0; i < req.entries.size(); ++i)
    {
        const ModuleEntry& e = req.entries[i];
        if (!e.optimize)
            continue;
        int rack = rackOfSlot(e.slot);
        if (rack < 0)
        {
            warnings.push_back(std::string(slotName(e.slot)) + " slot at position " + std::to_string(e.position) +
                          " can't be optimized (only high, med, low and rig slots); kept as is");
            continue;
        }
        requested[static_cast<size_t>(rack)].push_back(i);
        if (e.input.typeID)
        {
            o.openModules[req.fitIndex[i]] = 1;
            startPrice += priceOf(e.input.typeID) + priceOf(e.input.chargeID);
        }
        else
            ++o.openEmpty[static_cast<size_t>(rack)];
    }

    opt::Result res = opt::optimize(gd, engine, req.fit, o);
    for (const std::string& w : res.warnings)
        warnings.push_back(w);
    // Pinned modules evaluated with a lowered state (module rules, e.g. assault damage controls).
    json::Value clamped = json::Value::array();
    for (const auto& [index, state] : res.clamped)
        for (size_t i = 0; i < req.entries.size(); ++i)
            if (req.fitIndex[i] == static_cast<size_t>(index))
            {
                const ModuleEntry& e = req.entries[i];
                const Type* t = gd.type(e.input.typeID);
                json::Value c = json::Value::object();
                c["position"] = e.position;
                c["slot"] = slotName(e.slot);
                c["typeID"] = e.input.typeID;
                c["name"] = t ? t->name : std::string();
                c["state"] = stateName(e.input.state);
                c["evaluatedState"] = stateName(state);
                clamped.push_back(std::move(c));
                warnings.push_back((t ? t->name : std::string("module")) + " at position " + std::to_string(e.position) +
                                   " is evaluated " + stateName(state) + ", not " + stateName(e.input.state) +
                                   " (its active bonus doesn't count toward tank)");
            }
    for (int r = 0; r < opt::RackCount; ++r)
    {
        size_t want = requested[static_cast<size_t>(r)].size();
        if (static_cast<size_t>(res.openSlots[static_cast<size_t>(r)]) < want)
            warnings.push_back(std::string(slotName(kRackSlots[r])) +
                          ": " + std::to_string(want) + " slots requested, " +
                          std::to_string(res.openSlots[static_cast<size_t>(r)]) +
                          " free (the rest stay empty)");
    }

    json::Value out = json::Value::object();
    out["version"] = kVersion;
    out["status"] = res.infeasible.infeasible ? "infeasible" : "ok";
    out["message"] = res.infeasible.infeasible ? res.infeasible.message : std::string();
    out["goal"] = goalName(o.goal);
    if (res.infeasible.infeasible)
    {
        // Why no option can fit: the pinned part's resources (see OPTIMIZER_CLI.md).
        const opt::Infeasibility& inf = res.infeasible;
        json::Value ij = json::Value::object();
        ij["reason"] = inf.reason;
        ij["message"] = inf.message;
        json::Value rs = json::Value::array();
        for (const opt::Shortfall& sh : inf.resources)
        {
            json::Value r = json::Value::object();
            r["resource"] = opt::resourceName(sh.resource);
            r["pinnedUsed"] = round4(sh.pinnedUsed);
            r["output"] = round4(sh.output);
            r["bestPinnedUsed"] = round4(sh.bestPinnedUsed);
            r["bestOutput"] = round4(sh.bestOutput);
            r["shortfall"] = round4(sh.shortfall);
            r["message"] = sh.message;
            rs.push_back(std::move(r));
        }
        ij["resources"] = std::move(rs);
        json::Value mods = json::Value::array();
        for (size_t m = 0; m < inf.modules.size(); ++m)
            for (size_t i = 0; i < req.entries.size(); ++i)
                if (req.fitIndex[i] == static_cast<size_t>(inf.modules[m]))
                {
                    const ModuleEntry& e = req.entries[i];
                    const Type* t = gd.type(e.input.typeID);
                    json::Value mj = json::Value::object();
                    mj["position"] = e.position;
                    mj["slot"] = slotName(e.slot);
                    mj["typeID"] = e.input.typeID;
                    mj["name"] = t ? t->name : std::string();
                    mj["cost"] = round4(m < inf.moduleCost.size() ? inf.moduleCost[m] : 0.0);
                    mods.push_back(std::move(mj));
                }
        ij["modules"] = std::move(mods);
        out["infeasible"] = std::move(ij);
    }
    {
        json::Value open = json::Value::object();
        open["med"] = res.openSlots[opt::RackMid];
        open["low"] = res.openSlots[opt::RackLow];
        open["rig"] = res.openSlots[opt::RackRig];
        open["high"] = res.openSlots[opt::RackHigh];
        out["openSlots"] = std::move(open);
    }
    json::Value start = statsJson(res.start, o.goal);
    start["price"] = startPrice;
    out["start"] = std::move(start);
    out["clampedModules"] = std::move(clamped);

    // Options: the found modules mapped onto the requested positions. Per rack, a found
    // module identical in type and charge to a requested slot's current module stays in
    // that slot (then same type, other charge); the rest fill the remaining requested
    // positions in ascending order, sorted by (typeID, chargeID); leftover slots are empty.
    json::Value options = json::Value::array();
    for (size_t fi = 0; fi < res.fits.size(); ++fi)
    {
        const opt::FoundFit& f = res.fits[fi];
        std::map<size_t, const ModuleInput*> placed;  // entry index -> module (none = empty)
        for (int r = 0; r < opt::RackCount; ++r)
        {
            const auto& slotsIn = requested[static_cast<size_t>(r)];
            std::vector<const ModuleInput*> left;
            for (const ModuleInput& m : f.modules[r])
                left.push_back(&m);
            std::vector<bool> taken(slotsIn.size(), false);
            // A mutated module is the same item only with the same base type and mutations.
            auto sameItem = [](const ModuleInput& a, const ModuleInput& b) {
                return a.typeID == b.typeID && a.baseTypeID == b.baseTypeID && a.mutators == b.mutators;
            };
            for (int pass = 0; pass < 2; ++pass)
                for (size_t s = 0; s < slotsIn.size(); ++s)
                {
                    const ModuleInput& cur = req.entries[slotsIn[s]].input;
                    if (taken[s] || !cur.typeID)
                        continue;
                    for (size_t k = 0; k < left.size(); ++k)
                        if (sameItem(*left[k], cur) && (pass == 1 || left[k]->chargeID == cur.chargeID))
                        {
                            placed[slotsIn[s]] = left[k];
                            taken[s] = true;
                            left.erase(left.begin() + static_cast<ptrdiff_t>(k));
                            break;
                        }
                }
            std::stable_sort(left.begin(), left.end(), [](const ModuleInput* a, const ModuleInput* b) {
                return a->typeID != b->typeID ? a->typeID < b->typeID : a->chargeID < b->chargeID;
            });
            size_t k = 0;
            for (size_t s = 0; s < slotsIn.size() && k < left.size(); ++s)
                if (!taken[s])
                {
                    placed[slotsIn[s]] = left[k++];
                    taken[s] = true;
                }
            if (k < left.size())
                throw std::logic_error("optimizer returned more modules than requested slots");
        }

        // The whole fit in pyfa's order, evaluated again as a consistency check.
        FitInput full = req.fit;
        full.modules.clear();
        json::Value slots = json::Value::array();
        double price = 0;
        int changed = 0;
        for (size_t i = 0; i < req.entries.size(); ++i)
        {
            const ModuleEntry& e = req.entries[i];
            bool isRequested = false;
            for (const auto& list : requested)
                isRequested |= std::find(list.begin(), list.end(), i) != list.end();
            if (!isRequested)
            {
                if (e.input.typeID)
                {
                    ModuleInput m = e.input;
                    if (const Type* t = gd.type(m.typeID))
                        m.state = opt::clampPinnedState(gd, *t, m.state);
                    full.modules.push_back(m);
                }
                continue;
            }
            auto it = placed.find(i);
            const ModuleInput* m = it == placed.end() ? nullptr : it->second;
            json::Value s = json::Value::object();
            s["position"] = e.position;
            s["slot"] = slotName(e.slot);
            bool unchanged = m ? sameModule(*m, e.input) : !e.input.typeID;
            if (m)
            {
                full.modules.push_back(*m);
                s["typeID"] = m->typeID;
                s["chargeID"] = m->chargeID;
                s["state"] = stateName(m->state);
                const Type* t = gd.type(m->typeID);
                s["name"] = t ? t->name : std::string();
                const Type* c = m->chargeID ? gd.type(m->chargeID) : nullptr;
                s["chargeName"] = c ? json::Value(c->name) : json::Value();
                if (m->baseTypeID)
                {
                    // An owned mutated module (from an opened slot), as it was requested.
                    s["baseTypeID"] = m->baseTypeID;
                    json::Value mu = json::Value::object();
                    for (const auto& [attr, v] : m->mutators)
                        mu[std::to_string(attr)] = v;
                    s["mutators"] = std::move(mu);
                }
                price += priceOf(m->typeID) + priceOf(m->chargeID);
            }
            else
            {
                s["typeID"] = 0;
                s["chargeID"] = 0;
                s["state"] = json::Value();
                s["name"] = json::Value();
                s["chargeName"] = json::Value();
            }
            s["unchanged"] = unchanged;
            changed += unchanged ? 0 : 1;
            slots.push_back(std::move(s));
        }
        TestResult again = fitengine::toTestResult(engine.evaluate(full), {o.goal, static_cast<float>(o.remoteWeight)});
        if (std::abs(again.goal - f.exact.goal) > 1e-4 * std::max(1.0f, std::abs(f.exact.goal)))
            warnings.push_back("option " + std::to_string(fi + 1) + ": goal " + std::to_string(f.exact.goal) +
                          " but the assembled fit evaluates to " + std::to_string(again.goal));

        json::Value op = json::Value::object();
        op["rank"] = fi + 1;
        op["goal"] = num(f.exact.goal);
        op["gain"] = num(f.exact.goal - res.start.goal);
        op["fits"] = f.fits;
        op["price"] = price;
        op["priceDelta"] = price - startPrice;
        op["changedSlots"] = changed;
        op["stats"] = statsJson(f.exact, o.goal);
        op["slots"] = std::move(slots);
        options.push_back(std::move(op));
    }
    out["options"] = std::move(options);
    out["warnings"] = std::move(warnings);

    json::Value st = json::Value::object();
    st["seconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    st["candidates"] = res.stats.candidates;
    st["kinds"] = res.stats.kinds;
    st["engineCalls"] = res.stats.engineCalls;
    st["stage2Starts"] = res.stats.stage2Starts;
    st["secondsStage1"] = res.stats.seconds[2];
    st["secondsStage2"] = res.stats.seconds[3];
    st["secondsRefine"] = res.stats.seconds[5];
    st["stage2Best"] = res.stats.stage2Best;
    st["refineStarts"] = res.stats.refineStarts;
    st["refineRounds"] = res.stats.refineRounds;
    st["refineEvaluations"] = res.stats.refineEvaluations;
    st["refineImproved"] = res.stats.refineImproved;
    st["refineExact"] = res.stats.refineExact;
    st["refineStop"] = res.stats.refineStop;
    if (!res.stats.modelSkipped.empty())
        st["modelSkipped"] = res.stats.modelSkipped;
    out["stats"] = std::move(st);

    if (ro.members)
    {
        const opt::MemberCheck& m = res.members;
        json::Value mj = json::Value::object();
        mj["ran"] = m.ran;
        mj["skipped"] = m.skipped;
        mj["combinations"] = m.combinations;
        mj["fitting"] = m.fitting;
        mj["best"] = m.best;
        mj["bestPrice"] = m.bestPrice;
        mj["bestModules"] = m.bestModules;
        mj["seconds"] = m.seconds;
        mj["found"] = res.fits.empty() ? 0.0 : static_cast<double>(res.fits.front().exact.goal);
        mj["foundPrice"] = res.fits.empty() ? 0.0 : res.fits.front().price;
        out["members"] = std::move(mj);
    }

    if (ro.check)
    {
        const opt::BruteForceCheck& c = res.check;
        json::Value cj = json::Value::object();
        cj["ran"] = c.ran;
        cj["combinations"] = c.combinations;
        cj["feasible"] = c.feasible;
        cj["compared"] = c.compared;
        cj["mismatches"] = c.mismatches;
        cj["fittingGoalChanges"] = c.fittingGoalChanges;
        cj["bruteBest"] = c.bruteBest;
        cj["stage1Best"] = c.stage1Best;
        cj["maxError"] = c.maxError;
        out["check"] = std::move(cj);
        bool noSlots = std::accumulate(res.openSlots.begin(), res.openSlots.end(), 0) == 0;
        ok = noSlots || (c.ran && c.mismatches == 0 && c.fittingGoalChanges == 0);
    }
    return out;
}

json::Value makeRequest(const GameData& gd, const fitengine::Engine& engine, const Fit& fit, const std::string& open,
                        Goal goal)
{
    // Entries in position order; the rack of a dummy is its dummySlot.
    struct Entry
    {
        const FitModule* m;
        Slot slot;
        int position;
        bool optimize = false;
    };
    std::vector<Entry> entries;
    FitInput in;
    in.shipTypeID = fit.shipID;
    if (const Type* ship = gd.type(fit.shipID))
    {
        auto modes = gd.shipModes(*ship);
        if (!modes.empty())
            in.modeTypeID = fit.modeID && std::find(modes.begin(), modes.end(), *fit.modeID) != modes.end()
                                ? *fit.modeID
                                : modes.front();
    }
    int maxPos = -1;
    for (const FitModule& m : fit.modules)
    {
        maxPos = std::max(maxPos, m.position);
        if (m.itemID)
        {
            const Type* t = gd.type(*m.itemID);
            auto s = t ? gd.moduleSlot(*t) : std::nullopt;
            if (!s)
                continue;  // unknown item: the engine input skips it too
            entries.push_back({&m, *s, m.position});
            ModuleInput mi;
            mi.typeID = *m.itemID;
            mi.chargeID = m.chargeID.value_or(0);
            mi.state = static_cast<State>(static_cast<int>(m.state));
            if (m.isMutated())
                mi.baseTypeID = *m.baseItemID;
            for (const auto& [attr, v] : m.mutators)
                mi.mutators.emplace_back(attr, static_cast<float>(v));
            in.modules.push_back(mi);
        }
        else if (m.dummySlot)
            entries.push_back({&m, static_cast<Slot>(*m.dummySlot), m.position});
    }
    for (const FitItem& i : fit.appliedImplants())
        in.implants.push_back({i.itemID, i.active});
    for (const FitItem& b : fit.boosters)
        in.boosters.push_back({b.itemID, b.active, b.sideEffects});
    fitengine::FitResult fr = engine.evaluate(in);

    // Which rack indices are open.
    const Slot* rackSlot = kRackSlots;
    const fitschema::Attr sizeAttr[opt::RackCount] = {fitschema::Attr::medSlots, fitschema::Attr::lowSlots,
                                                      fitschema::Attr::rigSlots, fitschema::Attr::hiSlots};
    std::array<std::set<int>, opt::RackCount> openIdx;
    std::array<bool, opt::RackCount> whole{};
    if (open == "all")
        whole = {true, true, true, false};  // the high rack only when named (high:*)
    else
    {
        std::stringstream ss(open);
        std::string part;
        while (std::getline(ss, part, ';'))
        {
            if (part.empty())
                continue;
            size_t colon = part.find(':');
            std::string rack = part.substr(0, colon);
            int r = rack == "mid" || rack == "med" ? opt::RackMid
                    : rack == "low"                ? opt::RackLow
                    : rack == "rig"                ? opt::RackRig
                    : rack == "high"               ? opt::RackHigh
                                                   : -1;
            if (r < 0 || colon == std::string::npos)
                throw std::runtime_error("--open: expected rack:indices, e.g. mid:0,1;low:2;high:* or rig:*");
            std::string list = part.substr(colon + 1);
            if (list == "*")
            {
                whole[static_cast<size_t>(r)] = true;
                continue;
            }
            std::stringstream ls(list);
            std::string n;
            while (std::getline(ls, n, ','))
                if (!n.empty())
                    openIdx[static_cast<size_t>(r)].insert(std::stoi(n));
        }
    }
    // Pad racks with empty slots up to the rack size (pyfa keeps dummies for empty slots).
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.position < b.position; });
    for (int r = 0; r < opt::RackCount; ++r)
    {
        int size = static_cast<int>(std::lround(fr.shipAttr(sizeAttr[r])));
        int have = static_cast<int>(std::count_if(entries.begin(), entries.end(),
                                                  [&](const Entry& e) { return e.slot == rackSlot[r]; }));
        for (; have < size; ++have)
            entries.push_back({nullptr, rackSlot[r], ++maxPos});
    }
    std::array<int, opt::RackCount> seen{};
    for (Entry& e : entries)
    {
        int r = rackOfSlot(e.slot);
        if (r < 0)
            continue;
        int idx = seen[static_cast<size_t>(r)]++;
        e.optimize = whole[static_cast<size_t>(r)] || openIdx[static_cast<size_t>(r)].count(idx);
    }

    json::Value doc = json::Value::object();
    doc["version"] = kVersion;
    doc["ship"] = fit.shipID;
    if (in.modeTypeID)
        doc["mode"] = in.modeTypeID;
    json::Value chj = json::Value::object();
    chj["defaultLevel"] = 5;
    chj["skills"] = json::Value::array();
    doc["character"] = std::move(chj);
    json::Value imps = json::Value::array();
    for (const auto& i : in.implants)
    {
        json::Value v = json::Value::object();
        v["typeID"] = i.typeID;
        v["active"] = i.active;
        imps.push_back(std::move(v));
    }
    doc["implants"] = std::move(imps);
    json::Value boosters = json::Value::array();
    for (const auto& b : in.boosters)
    {
        json::Value v = json::Value::object();
        v["typeID"] = b.typeID;
        v["active"] = b.active;
        json::Value se = json::Value::array();
        for (int id : b.sideEffects)
            se.push_back(id);
        v["sideEffects"] = std::move(se);
        boosters.push_back(std::move(v));
    }
    doc["boosters"] = std::move(boosters);
    json::Value mods = json::Value::array();
    for (const Entry& e : entries)
    {
        json::Value v = json::Value::object();
        v["position"] = e.position;
        v["slot"] = slotName(e.slot);
        if (e.m && e.m->itemID)
        {
            const FitModule& m = *e.m;
            v["typeID"] = *m.itemID;
            v["chargeID"] = m.chargeID.value_or(0);
            v["state"] = stateName(static_cast<State>(static_cast<int>(m.state)));
            if (m.isMutated())
            {
                v["baseTypeID"] = *m.baseItemID;
                v["mutaplasmidID"] = *m.mutaplasmidID;
            }
            if (!m.mutators.empty())
            {
                json::Value mu = json::Value::object();
                for (const auto& [attr, val] : m.mutators)
                    mu[std::to_string(attr)] = val;
                v["mutators"] = std::move(mu);
            }
        }
        else
            v["typeID"] = 0;
        v["optimize"] = e.optimize;
        mods.push_back(std::move(v));
    }
    doc["modules"] = std::move(mods);
    doc["goal"] = goalName(goal);
    doc["priceCap"] = 0;
    doc["allowOfficer"] = false;
    doc["results"] = 10;
    doc["threads"] = 0;
    return doc;
}
}  // namespace request
