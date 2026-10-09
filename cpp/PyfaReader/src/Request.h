#pragma once

// JSON requests and responses for the module optimizer (pyfa integration, see
// cpp/OPTIMIZER_CLI.md): a fit with module states and the slots to optimize in,
// optimization options out.

#include "Fit.h"
#include "FitEngine.h"
#include "GameData.h"
#include "Json.h"
#include "Optimizer.h"

#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace request
{
constexpr int kVersion = 1;

struct ModuleEntry
{
    int position = 0;            // pyfa Module.position: index in fit.modules (all racks)
    Slot slot = Slot::Low;       // the rack (the type's slot for modules)
    fitengine::ModuleInput input;  // typeID 0: an empty slot
    int mutaplasmidID = 0;       // informational (the engine uses baseTypeID + mutators)
    bool optimize = false;
};

struct Request
{
    int version = kVersion;
    fitengine::FitInput fit;           // modules: the non-empty entries in position order
    std::vector<ModuleEntry> entries;  // every entry (also empty slots), position order
    std::vector<size_t> fitIndex;      // per entry: index into fit.modules, or SIZE_MAX if empty
    int defaultSkillLevel = 5;         // skills not listed in skillLevels
    std::vector<std::pair<int, int>> skillLevels;
    fitengine::Goal goal = fitengine::Goal::ArmorActive;
    double priceCap = 0;
    bool allowOfficer = false;
    bool allowAncillary = true;     // Options::allowAncillary
    int maxReps = 0;                // Options::maxReps (0 = no limit)
    bool requireCapStable = false;  // Options::requireCapStable
    double minCapRecharge = 0;      // Options::minCapRecharge (GJ/s)
    int results = 10;
    int threads = 0;
    double timeBudget = 60;  // seconds (Options::timeBudget); 0 = no limit
    std::optional<std::unordered_map<int, double>> prices;
};

// Throws std::runtime_error with a message naming the offending field.
Request parse(const json::Value& doc, const GameData& gd);

struct RunOptions
{
    bool check = false;               // Options::bruteForce on the request's open slots
    std::ostream* log = nullptr;      // optimizer log
    std::function<void(double, const std::string&)> progress;
    std::optional<fitengine::Goal> goal;  // overrides the request's goal
    std::optional<std::array<int, fitengine::opt::RackCount>> maxSlots;
    bool members = false;  // Options::memberBruteForce (response "members")
    // Test overrides of the refine options (Options::refine...); unset = request / defaults.
    std::optional<bool> refine;
    std::optional<double> timeBudget, refineExact;
    std::optional<int> refineRounds, refinePatience, refinePairs, refineChains, refineStarts;
};

// Runs the optimizer; the response document. `fallbackPrices` (e.g. pyfa's prices
// table) is used when the request has no prices. Sets `ok` false on an optimizer
// check mismatch (with RunOptions::check).
json::Value run(const GameData& gd, const Request& req, const std::unordered_map<int, double>* fallbackPrices,
                const RunOptions& ro, bool& ok);

json::Value errorResponse(const std::string& message);

// A request for a saved fit (testing): every module with its state; `open` picks the
// slots to optimize as "all" (every mid, low and rig slot, padded with empty slots up
// to the rack size) or "mid:0,1;low:2;rig:*;high:*" (indices within the rack, in
// position order, empty slots included; '*' = the whole rack).
json::Value makeRequest(const GameData& gd, const fitengine::Engine& engine, const Fit& fit, const std::string& open, fitengine::Goal goal);

const char* goalName(fitengine::Goal g);
std::optional<fitengine::Goal> parseGoal(const std::string& name);
}
