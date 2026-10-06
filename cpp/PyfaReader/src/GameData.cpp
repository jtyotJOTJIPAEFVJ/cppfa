#include "GameData.h"

#include "Sqlite.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace
{
template <typename Map>
auto findPtr(const Map& map, int id) -> const typename Map::mapped_type*
{
    auto it = map.find(id);
    return it == map.end() ? nullptr : &it->second;
}

std::optional<int> optInt(const Statement& st, int col)
{
    if (auto v = st.getOptInt(col))
        return static_cast<int>(*v);
    return std::nullopt;
}
}

std::optional<double> Type::attr(int attributeID) const
{
    auto it = std::lower_bound(attributes.begin(), attributes.end(), attributeID,
                               [](const auto& a, int id) { return a.first < id; });
    if (it != attributes.end() && it->first == attributeID)
        return it->second;
    return std::nullopt;
}

bool Type::hasEffect(int effectID) const
{
    return std::binary_search(effects.begin(), effects.end(), effectID);
}

GameData GameData::load(const std::filesystem::path& eveDb, const std::filesystem::path& effectModifiersTsv)
{
    Database db(eveDb);
    GameData gd;

    {
        auto st = db.prepare("SELECT field_value FROM metadata WHERE field_name = 'client_build'");
        if (st.step())
            gd.clientBuild = st.getText(0);
    }
    {
        auto st = db.prepare("SELECT categoryID, name FROM invcategories");
        while (st.step())
        {
            Category c{static_cast<int>(st.getInt(0)), st.getText(1)};
            gd.categories_.emplace(c.id, std::move(c));
        }
    }
    {
        auto st = db.prepare("SELECT groupID, name, categoryID FROM invgroups");
        while (st.step())
        {
            Group g{static_cast<int>(st.getInt(0)), st.getText(1), static_cast<int>(st.getInt(2))};
            gd.groups_.emplace(g.id, std::move(g));
        }
    }
    {
        auto st = db.prepare("SELECT marketGroupID, marketGroupName, parentGroupID FROM invmarketgroups");
        while (st.step())
        {
            MarketGroup m{static_cast<int>(st.getInt(0)), st.getText(1), optInt(st, 2)};
            gd.marketGroups_.emplace(m.id, std::move(m));
        }
    }
    {
        auto st = db.prepare(
            "SELECT attributeID, attributeName, IFNULL(defaultValue, 0), minAttributeID, maxAttributeID FROM dgmattribs");
        while (st.step())
        {
            Attribute a{static_cast<int>(st.getInt(0)), st.getText(1), st.getDouble(2), optInt(st, 3), optInt(st, 4)};
            gd.attributeIDs_.emplace(a.name, a.id);
            gd.attributes_.emplace(a.id, std::move(a));
        }
    }
    {
        auto st = db.prepare("SELECT effectID, effectName FROM dgmeffects");
        while (st.step())
        {
            Effect e{static_cast<int>(st.getInt(0)), st.getText(1)};
            gd.effectIDs_.emplace(e.name, e.id);
            gd.effects_.emplace(e.id, std::move(e));
        }
    }
    {
        auto st = db.prepare("SELECT typeID, typeName, groupID, marketGroupID, IFNULL(published, 0) FROM invtypes");
        gd.types_.reserve(32768);
        while (st.step())
        {
            Type t;
            t.id = static_cast<int>(st.getInt(0));
            t.name = st.getText(1);
            t.groupID = static_cast<int>(st.getInt(2));
            t.marketGroupID = optInt(st, 3);
            t.published = st.getInt(4) != 0;
            gd.types_.emplace(t.id, std::move(t));
        }
    }
    {
        auto st = db.prepare("SELECT typeID, attributeID, value FROM dgmtypeattribs ORDER BY typeID, attributeID");
        while (st.step())
        {
            auto it = gd.types_.find(static_cast<int>(st.getInt(0)));
            if (it != gd.types_.end())
                it->second.attributes.emplace_back(static_cast<int>(st.getInt(1)), st.getDouble(2));
        }
    }
    {
        auto st = db.prepare("SELECT typeID, effectID FROM dgmtypeeffects ORDER BY typeID, effectID");
        while (st.step())
        {
            auto it = gd.types_.find(static_cast<int>(st.getInt(0)));
            if (it != gd.types_.end())
                it->second.effects.push_back(static_cast<int>(st.getInt(1)));
        }
    }

    for (const auto& [id, t] : gd.types_)
    {
        if (gd.moduleSlot(t))
            gd.moduleIDsByName_.emplace(t.name, id);

        const Category* cat = gd.categoryOf(t);
        if (!cat)
            continue;
        if (cat->name == "Ship")
        {
            // A few ship names exist twice; prefer the published one.
            auto [it, inserted] = gd.shipIDsByName_.emplace(t.name, id);
            if (!inserted && t.published && !gd.types_.at(it->second).published)
                it->second = id;
        }
        else if (cat->name == "Skill")
            gd.skillIDsByName_.emplace(t.name, id);
    }

    if (!effectModifiersTsv.empty())
    {
        gd.modifierCount_ = loadModifierTable(effectModifiersTsv, gd.effectModifiers_);
        auto buffs = effectModifiersTsv.parent_path() / "warfare_buffs.tsv";
        if (std::filesystem::is_regular_file(buffs))
            loadModifierTable(buffs, gd.warfareBuffModifiers_);
    }

    return gd;
}

std::optional<Slot> GameData::moduleSlot(const Type& type) const
{
    // Same order as effectSlotMap in Module.calculateSlot(): first match wins.
    static const std::pair<const char*, Slot> effectSlotMap[] = {
        {"rigSlot", Slot::Rig},       {"loPower", Slot::Low},         {"medPower", Slot::Med},
        {"hiPower", Slot::High},      {"subSystem", Slot::Subsystem}, {"serviceSlot", Slot::Service},
    };
    for (const auto& [effectName, slot] : effectSlotMap)
    {
        int id = effectID(effectName);
        if (id && type.hasEffect(id))
            return slot;
    }
    return std::nullopt;
}

std::optional<Module> GameData::loadModule(const std::string& name, const std::vector<std::string>& attributeNames) const
{
    auto it = moduleIDsByName_.find(name);
    if (it == moduleIDsByName_.end())
        return std::nullopt;

    Module m;
    m.type = type(it->second);
    m.slot = *moduleSlot(*m.type);
    m.attributes = resolveAttributes(*m.type, attributeNames);
    return m;
}

std::unordered_map<std::string, double> GameData::resolveAttributes(const Type& t,
                                                                    const std::vector<std::string>& attributeNames) const
{
    std::unordered_map<std::string, double> out;
    for (const std::string& attrName : attributeNames)
    {
        const Attribute* a = attribute(attributeID(attrName));
        if (!a)
            throw std::invalid_argument("unknown attribute \"" + attrName + "\"");
        out[attrName] = t.attr(a->id).value_or(a->defaultValue);
    }
    return out;
}

std::optional<Ship> GameData::loadShip(const std::string& name, const std::vector<std::string>& attributeNames) const
{
    auto it = shipIDsByName_.find(name);
    if (it == shipIDsByName_.end())
        return std::nullopt;

    Ship s;
    s.type = type(it->second);
    s.attributes = resolveAttributes(*s.type, attributeNames);

    auto slots = [&](const char* attrName) { return static_cast<int>(typeAttr(*s.type, attrName).value_or(0.0)); };
    s.slots.high = slots("hiSlots");
    s.slots.med = slots("medSlots");
    s.slots.low = slots("lowSlots");
    s.slots.rig = slots("rigSlots");
    s.slots.subsystem = slots("maxSubSystems");
    s.slots.service = slots("serviceSlots");
    s.slots.turretHardpoints = slots("turretSlotsLeft");
    s.slots.launcherHardpoints = slots("launcherSlotsLeft");

    for (int effect : s.type->effects)
    {
        for (const EffectModifier& m : effectModifiers(effect))
        {
            if (!m.appliesInContext("ship"))
                continue;
            ShipBonus b;
            b.modifier = &m;
            if (!m.skill.empty())
            {
                auto skill = skillIDsByName_.find(m.skill);
                b.skillID = skill == skillIDsByName_.end() ? 0 : skill->second;
            }
            if (!m.sourceAttr.empty())
                if (const Attribute* a = attribute(attributeID(m.sourceAttr)))
                    b.value = s.type->attr(a->id).value_or(a->defaultValue);
            s.bonuses.push_back(b);
        }
    }
    // Skill bonuses grouped by skill, role bonuses last; stable keeps effect order.
    std::stable_sort(s.bonuses.begin(), s.bonuses.end(), [](const ShipBonus& a, const ShipBonus& b) {
        if (a.isRoleBonus() != b.isRoleBonus())
            return b.isRoleBonus();
        return a.modifier->skill < b.modifier->skill;
    });
    return s;
}

std::vector<const ShipBonus*> Ship::bonusesForSkill(const std::string& skillName) const
{
    std::vector<const ShipBonus*> out;
    for (const ShipBonus& b : bonuses)
        if (b.modifier->skill == skillName)
            out.push_back(&b);
    return out;
}

Character GameData::loadCharacter(int level) const
{
    Character c;
    c.defaultLevel = level;
    for (const auto& [name, id] : skillIDsByName_)
        c.skills.push_back(type(id));
    std::sort(c.skills.begin(), c.skills.end(), [](const Type* a, const Type* b) { return a->name < b->name; });

    for (const Type* skill : c.skills)
    {
        for (int effect : skill->effects)
        {
            for (const EffectModifier& m : effectModifiers(effect))
            {
                if (!m.appliesInContext("skill"))
                    continue;
                CharacterBonus b;
                b.skill = skill;
                b.modifier = &m;
                if (!m.sourceAttr.empty())
                    if (const Attribute* a = attribute(attributeID(m.sourceAttr)))
                        b.value = skill->attr(a->id).value_or(a->defaultValue);
                c.bonuses.push_back(b);
            }
        }
    }
    return c;
}

int Character::skillLevel(int skillID) const
{
    auto it = levels.find(skillID);
    return it == levels.end() ? defaultLevel : it->second;
}

std::vector<int> GameData::shipModes(const Type& ship) const
{
    std::vector<int> modes;
    const Group* g = groupOf(ship);
    if (!(g && g->name == "Tactical Destroyer") && ship.name != "Anhinga")
        return modes;
    auto lower = [](std::string s) {
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string prefix = lower(ship.name);
    for (const auto& [id, t] : types_)
    {
        const Group* tg = groupOf(t);
        if (tg && tg->name == "Ship Modifiers" && lower(t.name).rfind(prefix, 0) == 0)
            modes.push_back(id);
    }
    std::sort(modes.begin(), modes.end());
    return modes;
}

int GameData::skillID(const std::string& name) const
{
    auto it = skillIDsByName_.find(name);
    return it == skillIDsByName_.end() ? 0 : it->second;
}

std::vector<int> GameData::requiredSkills(const Type& t) const
{
    static const char* attrs[] = {"requiredSkill1", "requiredSkill2", "requiredSkill3",
                                  "requiredSkill4", "requiredSkill5", "requiredSkill6"};
    std::vector<int> out;
    for (const char* name : attrs)
        if (auto v = typeAttr(t, name); v && *v > 0)
            out.push_back(static_cast<int>(*v));
    return out;
}

bool GameData::requiresSkill(const Type& t, int skill) const
{
    auto req = requiredSkills(t);
    return std::find(req.begin(), req.end(), skill) != req.end();
}

bool EffectModifier::appliesInContext(const std::string& context) const
{
    return contexts.empty() || std::find(contexts.begin(), contexts.end(), context) != contexts.end();
}

const std::vector<EffectModifier>& GameData::effectModifiers(int effectID) const
{
    static const std::vector<EffectModifier> none;
    auto it = effectModifiers_.find(effectID);
    return it == effectModifiers_.end() ? none : it->second;
}

size_t GameData::loadModifierTable(const std::filesystem::path& tsv,
                                   std::unordered_map<int, std::vector<EffectModifier>>& into)
{
    size_t count = 0;
    std::ifstream in(tsv, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot open " + tsv.string());

    auto split = [](const std::string& line, char sep) {
        std::vector<std::string> out;
        size_t start = 0;
        while (true)
        {
            size_t end = line.find(sep, start);
            out.push_back(line.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos)
                return out;
            start = end + 1;
        }
    };

    std::string line;
    std::getline(in, line);
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    std::unordered_map<std::string, size_t> col;
    auto header = split(line, '\t');
    for (size_t i = 0; i < header.size(); ++i)
        col[header[i]] = i;
    for (const char* required : {"effectID", "effectName", "effectType", "runTime", "contexts", "operation", "domain",
                                 "target", "targetAttr", "sourceAttr", "sourceOf", "factor", "invert", "perLevel", "skill", "filter", "condition",
                                 "stackingPenalized", "penaltyGroup", "position", "valueExpr", "note"})
        if (!col.count(required))
            throw std::runtime_error(tsv.string() + ": missing column " + required +
                                     " (regenerate it with tools/gen_effect_modifiers.py)");

    // "[charge.]kind[:value]" terms, '|' within a clause, '&' between clauses.
    auto parseFilter = [&](const std::string& text, EffectModifier& m) {
        if (text.empty())
            return;
        if (text.rfind("unparsed:", 0) == 0)
        {
            m.unparsedFilter = text.substr(9);
            return;
        }
        for (const std::string& clauseText : split(text, '&'))
        {
            std::vector<FilterTerm> clause;
            for (std::string termText : split(clauseText, '|'))
            {
                FilterTerm term;
                if (termText.rfind("charge.", 0) == 0)
                {
                    term.onCharge = true;
                    termText.erase(0, 7);
                }
                size_t colon = termText.find(':');
                std::string kind = termText.substr(0, colon);
                term.value = colon == std::string::npos ? "" : termText.substr(colon + 1);
                if (kind == "skill")
                    term.kind = FilterTerm::Kind::Skill;
                else if (kind == "ownerSkill")
                    term.kind = FilterTerm::Kind::OwnerSkill;
                else if (kind == "group")
                    term.kind = FilterTerm::Kind::Group;
                else if (kind == "hasAttr")
                    term.kind = FilterTerm::Kind::HasAttr;
                else
                {
                    m.unparsedFilter = text;  // newer generator term we don't know yet
                    m.filter.clear();
                    return;
                }
                clause.push_back(std::move(term));
            }
            m.filter.push_back(std::move(clause));
        }
    };

    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        auto f = split(line, '\t');
        f.resize(header.size());
        EffectModifier m;
        m.effectID = std::stoi(f[col["effectID"]]);
        m.effectName = f[col["effectName"]];
        m.effectType = f[col["effectType"]];
        m.runTime = f[col["runTime"]];
        if (const std::string& ctx = f[col["contexts"]]; !ctx.empty() && ctx != "*")
            m.contexts = split(ctx, '|');
        m.operation = f[col["operation"]];
        m.domain = f[col["domain"]];
        m.target = f[col["target"]];
        m.targetAttr = f[col["targetAttr"]];
        m.sourceAttr = f[col["sourceAttr"]];
        m.sourceOf = f[col["sourceOf"]];
        m.factor = f[col["factor"]].empty() ? 1.0 : std::stod(f[col["factor"]]);
        m.invert = f[col["invert"]] == "1";
        m.perLevel = f[col["perLevel"]] == "1";
        m.skill = f[col["skill"]];
        parseFilter(f[col["filter"]], m);
        if (const std::string& cond = f[col["condition"]]; !cond.empty())
        {
            for (std::string term : split(cond, '&'))
            {
                ShipCondition c;
                if (!term.empty() && term[0] == '!')
                {
                    c.negate = true;
                    term.erase(0, 1);
                }
                if (term.rfind("ship.skill:", 0) == 0)
                    c.kind = ShipCondition::Kind::Skill, c.value = term.substr(11);
                else if (term.rfind("ship.hasAttr:", 0) == 0)
                    c.kind = ShipCondition::Kind::HasAttr, c.value = term.substr(13);
                else if (term.rfind("self.baseNonNeg:", 0) == 0)
                    c.kind = ShipCondition::Kind::SelfBaseNonNeg, c.value = term.substr(16);
                else
                {
                    m.unparsedCondition = cond;  // runtime state, or a newer generator term
                    m.conditions.clear();
                    break;
                }
                m.conditions.push_back(std::move(c));
            }
        }
        m.stackingPenalized = f[col["stackingPenalized"]] == "1";
        m.penaltyGroup = f[col["penaltyGroup"]];
        m.position = f[col["position"]];
        m.valueExpr = f[col["valueExpr"]];
        m.note = f[col["note"]];
        into[m.effectID].push_back(std::move(m));
        ++count;
    }
    return count;
}

const Type* GameData::type(int typeID) const { return findPtr(types_, typeID); }
const Group* GameData::group(int groupID) const { return findPtr(groups_, groupID); }
const Category* GameData::category(int categoryID) const { return findPtr(categories_, categoryID); }
const MarketGroup* GameData::marketGroup(int marketGroupID) const { return findPtr(marketGroups_, marketGroupID); }
const Attribute* GameData::attribute(int attributeID) const { return findPtr(attributes_, attributeID); }

int GameData::attributeID(const std::string& name) const
{
    auto it = attributeIDs_.find(name);
    return it == attributeIDs_.end() ? 0 : it->second;
}

int GameData::effectID(const std::string& name) const
{
    auto it = effectIDs_.find(name);
    return it == effectIDs_.end() ? 0 : it->second;
}

std::vector<std::string> GameData::moduleNamesInGroup(int groupID, bool publishedOnly) const
{
    std::vector<std::string> names;
    for (const auto& [name, id] : moduleIDsByName_)
    {
        const Type& t = types_.at(id);
        if (t.groupID == groupID && (t.published || !publishedOnly))
            names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> GameData::moduleNamesInGroup(const std::string& groupName, bool publishedOnly) const
{
    std::vector<std::string> names;
    for (const auto& [id, g] : groups_)
    {
        if (g.name != groupName)
            continue;
        auto inGroup = moduleNamesInGroup(id, publishedOnly);
        names.insert(names.end(), inGroup.begin(), inGroup.end());
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::optional<double> GameData::typeAttr(const Type& type, const std::string& attrName) const
{
    int id = attributeID(attrName);
    return id ? type.attr(id) : std::nullopt;
}

const Category* GameData::categoryOf(const Type& type) const
{
    const Group* g = groupOf(type);
    return g ? category(g->categoryID) : nullptr;
}
