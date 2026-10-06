#pragma once

// In-memory copy of the parts of eve.db needed to interpret fits.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Values match eos.const.FittingSlot.
enum class Slot
{
    Low = 1,
    Med = 2,
    High = 3,
    Rig = 4,
    Subsystem = 5,
    Service = 8,
};

struct Category
{
    int id = 0;
    std::string name;
};

struct Group
{
    int id = 0;
    std::string name;
    int categoryID = 0;
};

struct MarketGroup
{
    int id = 0;
    std::string name;
    std::optional<int> parentID;
};

struct Attribute
{
    int id = 0;
    std::string name;
    float defaultValue = 0.0;
    // The modified value is clamped to these attributes' values on the same item.
    std::optional<int> minAttributeID;
    std::optional<int> maxAttributeID;
};

struct Effect
{
    int id = 0;
    std::string name;
};

struct Type
{
    int id = 0;
    std::string name;
    int groupID = 0;
    std::optional<int> marketGroupID;
    bool published = false;

    // Sorted by attribute ID / effect ID.
    std::vector<std::pair<int, float>> attributes;
    std::vector<int> effects;

    std::optional<float> attr(int attributeID) const;
    bool hasEffect(int effectID) const;
};

// A fittable module with a caller-chosen subset of its attributes resolved by name.
struct Module
{
    const Type* type = nullptr;
    Slot slot = Slot::Low;
    // Requested attribute name -> value. Attributes the type doesn't define get
    // the attribute's default value from dgmattribs (as the game does).
    std::unordered_map<std::string, float> attributes;

    int id() const { return type->id; }
    const std::string& name() const { return type->name; }
    float attr(const std::string& name) const { return attributes.at(name); }
};

// One attribute modification performed by an effect, as extracted from pyfa's
// eos/effects.py by tools/gen_effect_modifiers.py (eve.db itself has no such data).
struct FilterTerm
{
    enum class Kind
    {
        Skill,       // item requires the skill named `value`
        OwnerSkill,  // item requires the skill that owns the effect
        Group,       // item is in the group named `value`
        HasAttr,     // item has the attribute named `value`
    };
    Kind kind = Kind::Skill;
    bool onCharge = false;  // test the module's loaded charge instead of the module
    std::string value;
};

// A requirement for a modifier to apply (from an `if` in pyfa's handler), on
// the fit's ship or on the effect's owner.
struct ShipCondition
{
    enum class Kind
    {
        Skill,        // the ship requires the skill named `value`
        HasAttr,      // the ship type has the attribute named `value`
        SelfBaseNonNeg,  // the effect owner's unmodified `value` attribute exists and is >= 0
    };
    Kind kind = Kind::Skill;
    bool negate = false;
    std::string value;
};

struct EffectModifier
{
    int effectID = 0;
    std::string effectName;
    std::string effectType;  // passive | active | projected | ... ('|'-joined)
    std::string runTime;     // early | normal | late: the pass pyfa runs the effect in
    // Owner kinds (pyfa handler contexts) this row applies to: ship, skill, implant,
    // booster, module, drone, fighter, projected+module, ... Empty = all.
    std::vector<std::string> contexts;
    std::string operation;   // boost (percent) | multiply | increase (add) | force | preAssign
    // What gets modified: ship | modules | drones | fighters | extraAttributes |
    // self (the effect's own item; for charge effects that's the module) | ...
    std::string domain;
    std::string target;      // item | charge (the charge loaded in matching modules / in self)
    std::string targetAttr;  // attribute that gets modified
    std::string sourceAttr;  // attribute holding the bonus amount...
    std::string sourceOf;    // ...on: owner (the effect's item) | ship | skill:<name> | charge
    float factor = 1.0;     // constant multiplier on the amount (e.g. -1)
    bool invert = false;     // amount = factor / value instead of factor * value
    bool perLevel = false;   // amount is multiplied by the owning skill's level
    std::string skill;       // amount is multiplied by this skill's level; empty = flat (role) bonus
    // Which items are affected: every clause must match; within a clause any term may.
    // Empty = all items in the domain.
    std::vector<std::vector<FilterTerm>> filter;
    std::string unparsedFilter;  // Python filter the generator couldn't translate
    std::vector<ShipCondition> conditions;  // all must hold
    std::string unparsedCondition;          // runtime-state conditions the generator couldn't translate
    bool stackingPenalized = false;
    std::string penaltyGroup;    // stacking penalty group when penalized
    std::string position;        // increase only: pre | post (relative to multipliers)
    std::string valueExpr;   // set when the amount isn't just sourceAttr (Python expression)
    std::string note;        // set when the generator couldn't resolve something

    bool appliesInContext(const std::string& context) const;
    // False when the amount or filter couldn't be resolved from pyfa's code.
    bool isResolved() const
    {
        return valueExpr.empty() && note.empty() && unparsedFilter.empty() && unparsedCondition.empty();
    }
};

struct SlotLayout
{
    int high = 0;
    int med = 0;
    int low = 0;
    int rig = 0;
    int subsystem = 0;
    int service = 0;
    int turretHardpoints = 0;
    int launcherHardpoints = 0;
};

// A ship bonus: one of the ship's effect modifiers plus the ship's bonus amount.
struct ShipBonus
{
    const EffectModifier* modifier = nullptr;
    int skillID = 0;              // 0 for role bonuses (modifier->skill empty) or unknown skills
    std::optional<float> value;  // ship's sourceAttr value; per skill level when skill is set

    bool isRoleBonus() const { return modifier->skill.empty(); }
};

struct Ship
{
    const Type* type = nullptr;
    SlotLayout slots;
    // Requested attribute name -> value, resolved like Module::attributes.
    std::unordered_map<std::string, float> attributes;
    // Skill bonuses first (by skill name), role bonuses last; effect order within each.
    std::vector<ShipBonus> bonuses;

    int id() const { return type->id; }
    const std::string& name() const { return type->name; }
    float attr(const std::string& name) const { return attributes.at(name); }
    std::vector<const ShipBonus*> bonusesForSkill(const std::string& skillName) const;
};

// A bonus a skill gives: one of the skill's effect modifiers plus its amount.
struct CharacterBonus
{
    const Type* skill = nullptr;
    const EffectModifier* modifier = nullptr;
    std::optional<float> value;  // skill's sourceAttr value; per level when modifier->perLevel
};

struct Character
{
    int defaultLevel = 5;                  // level of every skill not in `levels`
    std::unordered_map<int, int> levels;   // skill typeID -> level overrides (none for now)
    std::vector<const Type*> skills;       // every skill in the game data, by name
    std::vector<CharacterBonus> bonuses;   // grouped by skill, in skill name order

    int skillLevel(int skillID) const;
};

class GameData
{
public:
    // Reads everything into memory; the database is closed afterwards.
    // effectModifiersTsv is the table written by tools/gen_effect_modifiers.py;
    // without it ships load with no bonuses.
    static GameData load(const std::filesystem::path& eveDb, const std::filesystem::path& effectModifiersTsv = {});

    const Type* type(int typeID) const;
    const Group* group(int groupID) const;
    const Category* category(int categoryID) const;
    const MarketGroup* marketGroup(int marketGroupID) const;
    const Attribute* attribute(int attributeID) const;

    // Lookups by name; return 0 if not found.
    int attributeID(const std::string& name) const;
    int effectID(const std::string& name) const;

    // Convenience: attribute value on a type by attribute name.
    std::optional<float> typeAttr(const Type& type, const std::string& attrName) const;

    // Slot the type fits into (mirrors Module.calculateSlot in eos), or nullopt
    // if it isn't a fittable module.
    std::optional<Slot> moduleSlot(const Type& type) const;

    // Looks up a fittable module by exact type name and resolves the given
    // attributes. Returns nullopt if no module has that name; throws
    // std::invalid_argument if an attribute name doesn't exist in dgmattribs.
    std::optional<Module> loadModule(const std::string& name, const std::vector<std::string>& attributeNames) const;

    // Looks up a ship by exact type name (published ships win over unpublished
    // duplicates) with its slot layout, the given attributes and its bonuses.
    // Returns nullopt if no ship has that name; throws std::invalid_argument
    // for unknown attribute names.
    std::optional<Ship> loadShip(const std::string& name, const std::vector<std::string>& attributeNames) const;

    // A character that has every skill at `level`, with all skills' bonuses.
    Character loadCharacter(int level = 5) const;

    // T3 destroyer modes the ship can use (empty for other ships), as pyfa finds
    // them: "Ship Modifiers" items whose name starts with the ship's name.
    std::vector<int> shipModes(const Type& ship) const;

    // Skill typeID by exact name, 0 if unknown.
    int skillID(const std::string& name) const;
    // Skills the type directly requires (requiredSkill1..6), as pyfa's requiresSkill sees them.
    std::vector<int> requiredSkills(const Type& type) const;
    bool requiresSkill(const Type& type, int skillID) const;

    // Modifiers an effect applies (empty if unknown or no table was loaded).
    const std::vector<EffectModifier>& effectModifiers(int effectID) const;
    const std::unordered_map<int, std::vector<EffectModifier>>& allEffectModifiers() const { return effectModifiers_; }
    // Command burst buffs (pyfa Fit.__runCommandBoosts), keyed by warfare buff ID;
    // rows have sourceOf "buff": the amount is the buff's value. Loaded from
    // warfare_buffs.tsv next to the effect table, when present.
    const std::unordered_map<int, std::vector<EffectModifier>>& allWarfareBuffModifiers() const { return warfareBuffModifiers_; }
    size_t effectModifierCount() const { return modifierCount_; }

    // Type IDs in a group, ascending (unpublished ones only if publishedOnly is false).
    std::vector<int> typeIDsInGroup(int groupID, bool publishedOnly = true) const;

    // Names of the fittable modules in a group, sorted alphabetically. The
    // name overload matches every group with that exact name. Unpublished
    // (not obtainable in game) items are skipped unless publishedOnly is false.
    std::vector<std::string> moduleNamesInGroup(int groupID, bool publishedOnly = true) const;
    std::vector<std::string> moduleNamesInGroup(const std::string& groupName, bool publishedOnly = true) const;

    const Group* groupOf(const Type& type) const { return group(type.groupID); }
    const Category* categoryOf(const Type& type) const;

    std::string clientBuild;
    size_t typeCount() const { return types_.size(); }

private:
    static size_t loadModifierTable(const std::filesystem::path& tsv,
                                    std::unordered_map<int, std::vector<EffectModifier>>& into);
    // Requested attributes by name; type's value or the attribute's default.
    std::unordered_map<std::string, float> resolveAttributes(const Type& type,
                                                              const std::vector<std::string>& attributeNames) const;

    std::unordered_map<int, Type> types_;
    std::unordered_map<int, Group> groups_;
    std::unordered_map<int, Category> categories_;
    std::unordered_map<int, MarketGroup> marketGroups_;
    std::unordered_map<int, Attribute> attributes_;
    std::unordered_map<int, Effect> effects_;
    std::unordered_map<std::string, int> attributeIDs_;
    std::unordered_map<std::string, int> effectIDs_;
    std::unordered_map<std::string, int> moduleIDsByName_;
    std::unordered_map<std::string, int> shipIDsByName_;
    std::unordered_map<std::string, int> skillIDsByName_;
    std::unordered_map<int, std::vector<EffectModifier>> effectModifiers_;
    std::unordered_map<int, std::vector<EffectModifier>> warfareBuffModifiers_;
    size_t modifierCount_ = 0;
};
