#pragma once

// A fit as stored in pyfa's saveddata.db (raw IDs, no game data resolved).

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "GameData.h"  // Slot

class Database;

// Values match eos.const.FittingModuleState.
enum class ModuleState
{
    Offline = -1,
    Online = 0,
    Active = 1,
    Overheated = 2,
};

struct FitModule
{
    std::optional<int> itemID;       // empty for placeholder ("dummy") slots
    std::optional<int> dummySlot;    // slot of a placeholder
    std::optional<int> baseItemID;   // mutated modules: the original item
    std::optional<int> mutaplasmidID;
    std::optional<int> chargeID;
    ModuleState state = ModuleState::Online;
    int position = 0;
    std::map<int, double> mutators;  // attributeID -> mutated value

    bool isMutated() const { return baseItemID && mutaplasmidID; }
};

struct FitDrone
{
    int itemID = 0;
    std::optional<int> baseItemID;
    std::optional<int> mutaplasmidID;
    int amount = 0;
    int amountActive = 0;
    std::map<int, double> mutators;

    bool isMutated() const { return baseItemID && mutaplasmidID; }
};

struct FitFighter
{
    int itemID = 0;
    int amount = 0;  // -1 means "full squadron"
    bool active = false;
};

struct FitItem
{
    int itemID = 0;
    int amount = 1;
    bool active = true;               // implants / boosters
    std::vector<int> sideEffects;     // boosters: effectIDs of side effects switched on
};

struct Fit
{
    int64_t id = 0;
    std::string name;
    int shipID = 0;
    std::string notes;
    std::optional<int> modeID;  // T3 destroyer mode as saved (may be missing/stale)

    std::vector<FitModule> modules;  // non-projected, ordered by position
    std::vector<FitDrone> drones;
    std::vector<FitFighter> fighters;
    std::vector<FitItem> implants;           // the fit's own implants
    std::vector<FitItem> characterImplants;  // the character's implants
    bool implantsFromCharacter = false;      // pyfa implantLocation CHARACTER: the character's ones apply
    const std::vector<FitItem>& appliedImplants() const { return implantsFromCharacter ? characterImplants : implants; }
    std::vector<FitItem> boosters;
    std::vector<FitItem> cargo;
};

struct FitSummary
{
    int64_t id = 0;
    std::string name;
    int shipID = 0;
    std::string modified;
};

// Fits whose name matches: exact first, then case-insensitive, then substring.
// Most recently modified first.
std::vector<FitSummary> findFitsByName(const Database& saveDb, const std::string& name);
std::vector<FitSummary> recentFits(const Database& saveDb, int limit);
std::optional<Fit> loadFit(const Database& saveDb, int64_t fitID);
