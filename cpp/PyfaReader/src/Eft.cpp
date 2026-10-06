#include "Eft.h"

#include "GameData.h"
#include "PyMath.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <optional>
#include <tuple>

namespace
{
constexpr Slot kSlotOrder[] = {Slot::Low, Slot::Med, Slot::High, Slot::Rig, Slot::Subsystem, Slot::Service};

const char* slotLabel(Slot s)
{
    switch (s)
    {
    case Slot::Low: return "Low";
    case Slot::Med: return "Med";
    case Slot::High: return "High";
    case Slot::Rig: return "Rig";
    case Slot::Subsystem: return "Subsystem";
    case Slot::Service: return "Service";
    }
    return "";
}

// Ship attribute holding the number of slots of each kind (Fit.slots in eos).
const char* slotCountAttr(Slot s)
{
    switch (s)
    {
    case Slot::Low: return "lowSlots";
    case Slot::Med: return "medSlots";
    case Slot::High: return "hiSlots";
    case Slot::Rig: return "rigSlots";
    case Slot::Subsystem: return "maxSubSystems";
    case Slot::Service: return "serviceSlots";
    }
    return "";
}

// Python's repr() of a float: shortest round-trip digits, fixed notation for
// decimal exponents in [-4, 16), scientific otherwise.
std::string pyFloatRepr(double v)
{
    if (std::isinf(v))
        return v > 0 ? "inf" : "-inf";
    if (std::isnan(v))
        return "nan";

    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::scientific);
    std::string sci(buf, res.ptr);

    std::string sign;
    if (sci[0] == '-')
    {
        sign = "-";
        sci.erase(0, 1);
    }
    size_t ePos = sci.find('e');
    std::string digits = sci.substr(0, ePos);
    digits.erase(std::remove(digits.begin(), digits.end(), '.'), digits.end());
    int exp = std::stoi(sci.substr(ePos + 1));

    std::string out;
    if (exp >= -4 && exp < 16)
    {
        if (exp < 0)
            out = "0." + std::string(-exp - 1, '0') + digits;
        else if (static_cast<int>(digits.size()) <= exp + 1)
            out = digits + std::string(exp + 1 - digits.size(), '0') + ".0";
        else
            out = digits.substr(0, exp + 1) + "." + digits.substr(exp + 1);
    }
    else
    {
        out = digits.substr(0, 1);
        if (digits.size() > 1)
            out += "." + digits.substr(1);
        char e[8];
        snprintf(e, sizeof(e), "e%c%02d", exp < 0 ? '-' : '+', std::abs(exp));
        out += e;
    }
    return sign + out;
}

using pymath::floatUnerr;

std::string typeName(const GameData& gd, std::optional<int> typeID)
{
    if (!typeID)
        return {};
    const Type* t = gd.type(*typeID);
    return t ? t->name : "<unknown type " + std::to_string(*typeID) + ">";
}

struct Mutant
{
    int baseItemID;
    int mutaplasmidID;
    const std::map<int, double>* mutators;
};

std::string renderMutant(const Mutant& m, const GameData& gd, int reference)
{
    std::vector<std::pair<std::string, double>> attrs;
    for (const auto& [attrID, value] : *m.mutators)
    {
        const Attribute* a = gd.attribute(attrID);
        attrs.emplace_back(a ? a->name : std::to_string(attrID), value);
    }
    std::sort(attrs.begin(), attrs.end());

    std::string attrLine;
    for (const auto& [name, value] : attrs)
    {
        if (!attrLine.empty())
            attrLine += ", ";
        attrLine += name + " " + pyFloatRepr(floatUnerr(value));
    }
    return "[" + std::to_string(reference) + "] " + typeName(gd, m.baseItemID) + "\n  " +
           typeName(gd, m.mutaplasmidID) + "\n  " + attrLine;
}

std::string join(const std::vector<std::string>& parts, const std::string& sep)
{
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i)
    {
        if (i)
            out += sep;
        out += parts[i];
    }
    return out;
}

class Exporter
{
public:
    Exporter(const Fit& fit, const GameData& gd, std::vector<std::string>* warnings)
        : fit_(fit), gd_(gd), warnings_(warnings)
    {
    }

    std::string run()
    {
        std::vector<std::string> sections;
        if (auto s = modules(); !s.empty())
            sections.push_back(s);

        std::vector<std::string> minions;
        if (auto s = drones(); !s.empty())
            minions.push_back(s);
        if (auto s = fighters(); !s.empty())
            minions.push_back(s);
        if (!minions.empty())
            sections.push_back(join(minions, "\n\n"));

        std::vector<std::string> charSection;
        if (auto s = slotSortedItems(fit_.implants, "implantness"); !s.empty())
            charSection.push_back(s);
        if (auto s = slotSortedItems(fit_.boosters, "boosterness"); !s.empty())
            charSection.push_back(s);
        if (!charSection.empty())
            sections.push_back(join(charSection, "\n\n"));

        if (auto s = cargo(); !s.empty())
            sections.push_back(s);

        if (!mutants_.empty())
        {
            std::vector<std::string> lines;
            for (size_t i = 0; i < mutants_.size(); ++i)
                lines.push_back(renderMutant(mutants_[i], gd_, static_cast<int>(i + 1)));
            sections.push_back(join(lines, "\n"));
        }

        const Type* ship = gd_.type(fit_.shipID);
        std::string header = "[" + (ship ? ship->name : typeName(gd_, fit_.shipID)) + ", " + fit_.name + "]";
        return header + "\n\n" + join(sections, "\n\n\n");
    }

private:
    void warn(const std::string& msg)
    {
        if (warnings_)
            warnings_->push_back(msg);
    }

    std::string mutationSuffix(int baseItemID, int mutaplasmidID, const std::map<int, double>& mutators)
    {
        mutants_.push_back({baseItemID, mutaplasmidID, &mutators});
        return " [" + std::to_string(mutants_.size()) + "]";
    }

    // Slot count as the ship would have after subsystems apply their modifiers
    // (the only slot-count changes pyfa's fill() can see for saved fits).
    int totalSlots(Slot slot, const std::vector<const Type*>& subsystems) const
    {
        const Type* ship = gd_.type(fit_.shipID);
        if (!ship)
            return 0;
        double total = gd_.typeAttr(*ship, slotCountAttr(slot)).value_or(0.0);
        const char* modifierAttr = slot == Slot::High  ? "hiSlotModifier"
                                   : slot == Slot::Med ? "medSlotModifier"
                                   : slot == Slot::Low ? "lowSlotModifier"
                                                       : nullptr;
        if (modifierAttr)
            for (const Type* sub : subsystems)
                total += gd_.typeAttr(*sub, modifierAttr).value_or(0.0);
        return static_cast<int>(total);
    }

    std::string modules()
    {
        struct Entry
        {
            const FitModule* mod;  // nullptr for slots added by fill()
            const Type* item;
            Slot slot;
        };
        std::vector<Entry> entries;
        std::vector<const Type*> subsystems;

        for (const FitModule& m : fit_.modules)
        {
            if (!m.itemID)
            {
                if (m.dummySlot)
                    entries.push_back({&m, nullptr, static_cast<Slot>(*m.dummySlot)});
                continue;
            }
            const Type* item = gd_.type(*m.itemID);
            if (!item)
            {
                warn("module type " + std::to_string(*m.itemID) + " not in game data, skipped");
                continue;
            }
            auto slot = gd_.moduleSlot(*item);
            if (!slot)
                continue;  // not a fittable slot item; pyfa doesn't export these either
            if (*slot == Slot::Subsystem)
                subsystems.push_back(item);
            entries.push_back({&m, item, *slot});
        }

        // Fit.fill(): pad each rack with empty slots, or drop surplus placeholders.
        for (Slot slot : kSlotOrder)
        {
            int used = static_cast<int>(std::count_if(entries.begin(), entries.end(),
                                                      [&](const Entry& e) { return e.slot == slot; }));
            int freeSlots = totalSlots(slot, subsystems) - used;
            for (int i = 0; i < freeSlots; ++i)
                entries.push_back({nullptr, nullptr, slot});
            for (auto it = entries.begin(); freeSlots < 0 && it != entries.end();)
            {
                if (it->slot == slot && !it->item)
                {
                    it = entries.erase(it);
                    ++freeSlots;
                }
                else
                    ++it;
            }
        }

        std::vector<std::string> racks;
        for (Slot slot : kSlotOrder)
        {
            std::vector<std::string> lines;
            for (const Entry& e : entries)
            {
                if (e.slot != slot)
                    continue;
                if (!e.item)
                {
                    lines.push_back(std::string("[Empty ") + slotLabel(slot) + " slot]");
                    continue;
                }
                const FitModule& m = *e.mod;
                std::string line = m.isMutated() ? typeName(gd_, m.baseItemID) : e.item->name;
                if (m.chargeID && gd_.type(*m.chargeID))
                    line += ", " + typeName(gd_, m.chargeID);
                if (m.state == ModuleState::Offline)
                    line += " /offline";
                if (m.isMutated())
                    line += mutationSuffix(*m.baseItemID, *m.mutaplasmidID, m.mutators);
                lines.push_back(line);
            }
            if (!lines.empty())
                racks.push_back(join(lines, "\n"));
        }
        return join(racks, "\n\n");
    }

    std::string drones()
    {
        // Same as in pyfa's drone additions panel.
        static const char* droneOrder[] = {
            "Light Scout Drones",    "Light Hybrid Drones",       "Medium Scout Drones",
            "Medium Hybrid Drones",  "Heavy Attack Drones",       "Heavy Hybrid Drones",
            "Sentry Drones",         "Combat Utility Drones",     "Electronic Warfare Drones",
            "Logistic Drones",       "Mining Drones",             "Salvage Drones",
        };
        auto orderOf = [&](const FitDrone& d) {
            int id = d.isMutated() ? *d.baseItemID : d.itemID;
            const Type* t = gd_.type(id);
            const MarketGroup* mg = t && t->marketGroupID ? gd_.marketGroup(*t->marketGroupID) : nullptr;
            if (mg)
                for (size_t i = 0; i < std::size(droneOrder); ++i)
                    if (mg->name == droneOrder[i])
                        return static_cast<int>(i);
            return static_cast<int>(std::size(droneOrder));
        };
        auto nameOf = [&](const FitDrone& d) { return typeName(gd_, d.isMutated() ? d.baseItemID : d.itemID); };

        std::vector<const FitDrone*> sorted;
        for (const FitDrone& d : fit_.drones)
            if (gd_.type(d.itemID))
                sorted.push_back(&d);
            else
                warn("drone type " + std::to_string(d.itemID) + " not in game data, skipped");
        std::stable_sort(sorted.begin(), sorted.end(), [&](const FitDrone* a, const FitDrone* b) {
            return std::make_tuple(orderOf(*a), a->isMutated(), nameOf(*a)) <
                   std::make_tuple(orderOf(*b), b->isMutated(), nameOf(*b));
        });

        std::vector<std::string> lines;
        for (const FitDrone* d : sorted)
        {
            std::string line = nameOf(*d) + " x" + std::to_string(d->amount);
            if (d->isMutated())
                line += mutationSuffix(*d->baseItemID, *d->mutaplasmidID, d->mutators);
            lines.push_back(line);
        }
        return join(lines, "\n");
    }

    std::string fighters()
    {
        static const char* fighterOrder[] = {
            "Light Fighter",   "Structure Light Fighter",   "Heavy Fighter",
            "Structure Heavy Fighter", "Support Fighter",   "Structure Support Fighter",
        };
        struct Entry
        {
            int order;
            std::string name;
            int amount;
        };
        std::vector<Entry> entries;
        for (const FitFighter& f : fit_.fighters)
        {
            const Type* t = gd_.type(f.itemID);
            if (!t)
            {
                warn("fighter type " + std::to_string(f.itemID) + " not in game data, skipped");
                continue;
            }
            const Group* g = gd_.groupOf(*t);
            int order = static_cast<int>(std::size(fighterOrder));
            for (size_t i = 0; g && i < std::size(fighterOrder); ++i)
                if (g->name == fighterOrder[i])
                    order = static_cast<int>(i);
            // -1 means a full squadron. pyfa uses the skill-modified squadron size;
            // without a dogma engine we can only use the base value.
            int amount = f.amount;
            if (amount == -1)
                amount = static_cast<int>(gd_.typeAttr(*t, "fighterSquadronMaxSize").value_or(0.0));
            entries.push_back({order, t->name, amount});
        }
        std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            return std::tie(a.order, a.name) < std::tie(b.order, b.name);
        });
        std::vector<std::string> lines;
        for (const Entry& e : entries)
            lines.push_back(e.name + " x" + std::to_string(e.amount));
        return join(lines, "\n");
    }

    // Implants / boosters, sorted by their slot attribute.
    std::string slotSortedItems(const std::vector<FitItem>& items, const char* slotAttr)
    {
        std::vector<std::pair<int, const Type*>> sorted;
        for (const FitItem& i : items)
            if (const Type* t = gd_.type(i.itemID))
                sorted.emplace_back(static_cast<int>(gd_.typeAttr(*t, slotAttr).value_or(0.0)), t);
            else
                warn("type " + std::to_string(i.itemID) + " not in game data, skipped");
        std::stable_sort(sorted.begin(), sorted.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<std::string> lines;
        for (const auto& [slot, t] : sorted)
            lines.push_back(t->name);
        return join(lines, "\n");
    }

    std::string cargo()
    {
        struct Entry
        {
            std::string category, group, name;
            int amount;
        };
        std::vector<Entry> entries;
        for (const FitItem& c : fit_.cargo)
        {
            const Type* t = gd_.type(c.itemID);
            if (!t)
            {
                warn("cargo type " + std::to_string(c.itemID) + " not in game data, skipped");
                continue;
            }
            const Group* g = gd_.groupOf(*t);
            const Category* cat = gd_.categoryOf(*t);
            entries.push_back({cat ? cat->name : "", g ? g->name : "", t->name, c.amount});
        }
        std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            return std::tie(a.category, a.group, a.name) < std::tie(b.category, b.group, b.name);
        });
        std::vector<std::string> lines;
        for (const Entry& e : entries)
            lines.push_back(e.name + " x" + std::to_string(e.amount));
        return join(lines, "\n");
    }

    const Fit& fit_;
    const GameData& gd_;
    std::vector<std::string>* warnings_;
    std::vector<Mutant> mutants_;
};
}

std::string exportEft(const Fit& fit, const GameData& gd, std::vector<std::string>* warnings)
{
    return Exporter(fit, gd, warnings).run();
}
