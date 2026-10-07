// PyfaReader - loads pyfa's game data (eve.db) into memory and exports saved
// fittings (saveddata.db) in EFT format.
//
// Usage:
//   PyfaReader [--game <eve.db>] [--save <saveddata.db>] [<fit name> | #<fitID>]
//   PyfaReader [--game ...] [--save ...] --engine-dump <out.tsv>
//   PyfaReader [--game ...] [--save ...] --search-check [maxFits]
//   PyfaReader [--game ...] [--save ...] --bench [fitID [seconds [goal]]]   (profiling case)
//   PyfaReader [--game ...] [--save ...] --optimize <fitID> <goal> [--racks mid,low,rig] [--price-cap ISK]
//              [--allow-officer] [--top K] [--results N] [--dump-kinds] [--slots m,l,r] [--threads N]
//   PyfaReader [--game ...] [--save ...] --optimize-check <fitID>[,<fitID>...] [goal|all] [--slots m,l,r]
//              [--verbose]   (stage 1 vs brute force; default 2 mid / 2 low / 1 rig open)
//   PyfaReader [--game ...] [--save ...] --optimize-request <in.json|-> [--out <out.json>] [--goal G] [--check]
//              [--threads N] [--slots m,l,r] [--verbose]   (pyfa integration, see cpp/OPTIMIZER_CLI.md)
//              [--members] [--no-refine] [--time-budget S] [--refine-rounds N] [--refine-patience N]
//              [--refine-pairs N] [--refine-chains N] [--refine-starts N] [--refine-exact N]   (refine tests; also for --optimize)
//   PyfaReader [--game ...] [--save ...] --make-request <fitID> [--open all|mid:0,1;low:2;rig:*] [--goal G]
//              [--out <req.json>]   (a request for a saved fit, for testing)
//
// Without a fit it lists the most recently modified fits. A fit name is matched
// exactly, then case-insensitively, then as a substring; if several fits match,
// the most recently modified one is exported and the others are listed on stderr.
//
// --engine-dump evaluates every saved fit with the fit engine (All-5 character,
// uniform damage, reload factored) and writes "fitID<TAB>key<TAB>value" lines,
// for comparison with pyfa (tools/pyfa_reference.py + tools/compare_engine.py).

#include "Eft.h"
#include "Fit.h"
#include "FitEngine.h"
#include "GameData.h"
#include "Optimizer.h"
#include "PyfaPaths.h"
#include "Request.h"
#include "Sqlite.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <chrono>
#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <numeric>
#include <random>
#include <iostream>
#include <map>
#include <unordered_map>
#include <string>

namespace fs = std::filesystem;

namespace
{
fs::path exeDir()
{
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(std::wstring(buf, n)).parent_path();
}

std::string toUtf8(const std::wstring& s)
{
    if (s.empty())
        return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len, nullptr, nullptr);
    return out;
}

std::string toUtf8(const fs::path& p)
{
    return toUtf8(p.wstring());
}

std::string describe(const FitSummary& f, const GameData& gd)
{
    const Type* ship = gd.type(f.shipID);
    return "#" + std::to_string(f.id) + "  [" + (ship ? ship->name : "?") + ", " + f.name + "]";
}
}

fitengine::FitInput toEngineInput(const Fit& fit, const GameData& gd, std::vector<int>& positions)
{
    fitengine::FitInput in;
    in.shipTypeID = fit.shipID;
    if (const Type* ship = gd.type(fit.shipID))
    {
        // pyfa: a T3 destroyer always has a mode; an invalid/missing one becomes the first.
        auto modes = gd.shipModes(*ship);
        if (!modes.empty())
            in.modeTypeID = fit.modeID && std::find(modes.begin(), modes.end(), *fit.modeID) != modes.end()
                                ? *fit.modeID
                                : modes.front();
    }
    for (const FitModule& m : fit.modules)
    {
        if (!m.itemID)
            continue;  // empty slot
        fitengine::ModuleInput mi;
        mi.typeID = *m.itemID;
        mi.chargeID = m.chargeID.value_or(0);
        mi.state = static_cast<fitengine::State>(static_cast<int>(m.state));
        if (m.isMutated())
            mi.baseTypeID = *m.baseItemID;
        for (const auto& [attr, v] : m.mutators)
            mi.mutators.emplace_back(attr, static_cast<float>(v));
        in.modules.push_back(std::move(mi));
        positions.push_back(m.position);
    }
    for (const FitItem& i : fit.appliedImplants())
        in.implants.push_back({i.itemID, i.active});
    for (const FitItem& b : fit.boosters)
        in.boosters.push_back({b.itemID, b.active, b.sideEffects});
    return in;
}

int engineDump(const GameData& gd, const Database& save, const fs::path& outPath)
{
    Character ch = gd.loadCharacter(5);
    auto start = std::chrono::steady_clock::now();
    fitengine::Engine engine(gd, ch);
    std::cerr << "engine: " << engine.compiledModifierCount() << " modifiers compiled in "
              << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()
              << " ms\n";

    FILE* out = nullptr;
    if (_wfopen_s(&out, outPath.c_str(), L"wb") != 0 || !out)
    {
        std::cerr << "cannot write " << toUtf8(outPath) << "\n";
        return 1;
    }
    int fits = 0, failed = 0;
    double evalSeconds = 0;
    for (const FitSummary& summary : recentFits(save, 1 << 30))
    {
        auto fit = loadFit(save, summary.id);
        if (!fit)
            continue;
        std::vector<int> positions;
        fitengine::FitInput in = toEngineInput(*fit, gd, positions);
        fitengine::FitResult r;
        try
        {
            auto t0 = std::chrono::steady_clock::now();
            r = engine.evaluate(in);
            evalSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        }
        catch (const std::exception& e)
        {
            std::cerr << "fit " << fit->id << ": " << e.what() << "\n";
            ++failed;
            continue;
        }
        long long id = fit->id;
        auto put = [&](const std::string& key, double v) { std::fprintf(out, "%lld\t%s\t%.17g\n", id, key.c_str(), v); };
        for (size_t i = 0; i < std::size(fitschema::ShipAttrs::kFields); ++i)
            put(std::string("ship.") + fitschema::kAttrName[static_cast<size_t>(fitschema::ShipAttrs::kFields[i])],
                r.ship[i]);
        put("used.cpu", r.cpuUsed);
        put("used.power", r.powerUsed);
        put("used.calibration", r.calibrationUsed);
        const char* layers[] = {"shield", "armor", "hull"};
        const char* reps[] = {"shieldRepair", "armorRepair", "hullRepair"};
        for (int l = 0; l < fitengine::LayerCount; ++l)
        {
            put(std::string("hp.") + layers[l], r.hp[l]);
            put(std::string("ehp.") + layers[l], r.ehp[l]);
            put(std::string("sustainable.") + reps[l], r.repair[l]);
            put(std::string("effectiveSustainable.") + reps[l], r.effectiveRepair[l]);
        }
        put("cap.peakRecharge", r.capPeakRecharge);
        put("cap.recharge", r.capRecharge);
        put("cap.used", r.capUsed);
        for (size_t i = 0; i < r.modules.size(); ++i)
        {
            const auto& m = r.modules[i];
            std::string p = "mod." + std::to_string(positions[i]) + ".";
            for (const auto& [a, v] : m.attrs)
                put(p + "attrs." + fitschema::kAttrName[static_cast<size_t>(a)], v);
            put(p + "rawCycleTime", m.rawCycleTime);
            put(p + "reactivationDelay", m.reactivationDelay);
            put(p + "moduleReloadTime", m.reloadTime);
            put(p + "averageCycleTime", m.averageCycleTime);
            put(p + "capUse", m.capUse);
            put(p + "numShots", m.numShots);
        }
        for (const auto& w : r.warnings)
            std::cerr << "fit " << id << ": " << w << "\n";
        ++fits;
    }
    std::fclose(out);
    std::cerr << fits << " fits evaluated (" << failed << " failed), " << evalSeconds * 1e6 / std::max(fits, 1)
              << " us per fit -> " << toUtf8(outPath) << "\n";
    return failed ? 1 : 0;
}

// Checks SearchPlan against the full engine: in every saved fit, up to 4 modules
// become open slots whose options are the original plus up to 3 other modules of
// the same group; random choices (incl. empty slots) are compared for all goals.
int searchCheck(const GameData& gd, const Database& save, int maxFits)
{
    using namespace fitengine;
    Character ch = gd.loadCharacter(5);
    Engine engine(gd, ch);
    std::mt19937 rng(12345);
    const Goal goals[] = {Goal::ShieldPassive, Goal::ArmorPassive, Goal::HullPassive, Goal::ShieldActive,
                          Goal::ArmorActive};
    const char* goalNames[] = {"shieldPassive", "armorPassive", "hullPassive", "shieldActive", "armorActive"};

    size_t fits = 0, tests = 0, mismatches = 0, detailMismatches = 0, instructions = 0, dynamic = 0, orderFits = 0;
    double planSeconds = 0, fullSeconds = 0, buildSeconds = 0;
    size_t planCalls = 0, fullCalls = 0;
    // Plan and engine sum the same float terms in a different order.
    auto close = [](double a, double b) { return std::abs(a - b) <= 1e-5 * std::abs(b) + 1e-6; };

    for (const FitSummary& summary : recentFits(save, 1 << 30))
    {
        if (maxFits && static_cast<int>(fits) >= maxFits)
            break;
        auto fit = loadFit(save, summary.id);
        if (!fit)
            continue;
        const Type* ship = gd.type(fit->shipID);
        if (!ship || !gd.categoryOf(*ship) || gd.categoryOf(*ship)->name != "Ship")
            continue;
        std::vector<int> positions;
        FitInput in = toEngineInput(*fit, gd, positions);
        if (in.modules.size() < 2)
            continue;

        // Pick slots and their options.
        std::vector<size_t> idx(in.modules.size());
        std::iota(idx.begin(), idx.end(), size_t{0});
        std::shuffle(idx.begin(), idx.end(), rng);
        idx.resize(std::min<size_t>(4, idx.size()));
        std::sort(idx.begin(), idx.end());
        SearchSpec spec;
        spec.base.shipTypeID = in.shipTypeID;
        spec.base.modeTypeID = in.modeTypeID;
        spec.base.implants = in.implants;
        spec.base.boosters = in.boosters;
        for (size_t i = 0; i < in.modules.size(); ++i)
            if (!std::binary_search(idx.begin(), idx.end(), i))
                spec.base.modules.push_back(in.modules[i]);
        for (size_t i : idx)
        {
            const ModuleInput& orig = in.modules[i];
            std::vector<ModuleInput> options{orig};
            if (const Type* t = gd.type(orig.typeID))
            {
                auto group = gd.typeIDsInGroup(t->groupID);
                std::shuffle(group.begin(), group.end(), rng);
                for (int id : group)
                {
                    if (options.size() >= 4)
                        break;
                    if (id == orig.typeID)
                        continue;
                    ModuleInput alt;
                    alt.typeID = id;
                    alt.chargeID = orig.chargeID;
                    alt.state = orig.state;
                    options.push_back(alt);
                }
            }
            spec.slots.push_back(std::move(options));
        }

        auto t0 = std::chrono::steady_clock::now();
        SearchPlan plan(engine, spec);
        buildSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        auto st = plan.stats();
        instructions += st.instructions;
        dynamic += st.dynamic;
        orderFits += st.orderSensitive > 0;
        ++fits;

        std::vector<int> choice(spec.slots.size());
        for (int r = 0; r < 20; ++r)
        {
            for (size_t s = 0; s < choice.size(); ++s)
                choice[s] = std::uniform_int_distribution<int>(-1, static_cast<int>(spec.slots[s].size()) - 1)(rng);
            auto t1 = std::chrono::steady_clock::now();
            FitResult full = engine.evaluate(plan.fitFor(choice.data()));
            fullSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
            ++fullCalls;
            for (size_t g = 0; g < std::size(goals); ++g)
            {
                auto t2 = std::chrono::steady_clock::now();
                TestResult got = plan.evaluate(choice.data(), goals[g]);
                planSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t2).count();
                ++planCalls;
                TestResult want = toTestResult(full, goals[g]);
                ++tests;
                // The detail must reproduce the test's own numbers exactly when
                // recombined with the shared totals code (as the optimizer does).
                {
                    static thread_local TestDetail detail;
                    TestResult again = plan.evaluate(choice.data(), goals[g], &detail);
                    CapSums cs = capSums(detail.modules.data(), detail.moduleCount);
                    float recharge = cs.added + detail.capPeak;
                    bool same = cs.used == again.capUsed && recharge == again.capRecharge;
                    int layer = goals[g] == Goal::ShieldActive ? Shield : goals[g] == Goal::ArmorActive ? Armor : -1;
                    if (layer >= 0)
                    {
                        RepProfile p = makeRepProfile(detail.modules.data(), detail.moduleCount, cs.used, recharge);
                        same &= p.repair(layer) / detail.divider[static_cast<size_t>(layer)] == again.goal;
                    }
                    if (!same && detailMismatches++ < 10)
                        std::cerr << "fit " << fit->id << " " << goalNames[g] << ": detail doesn't reproduce the test\n";
                }
                bool ok = close(got.goal, want.goal) && close(got.cpuUsed, want.cpuUsed) &&
                          close(got.powerUsed, want.powerUsed) && close(got.calibrationUsed, want.calibrationUsed) &&
                          close(got.cpuOutput, want.cpuOutput) && close(got.powerOutput, want.powerOutput) &&
                          close(got.capUsed, want.capUsed) && close(got.capRecharge, want.capRecharge);
                if (!ok && mismatches++ < 10)
                    std::cerr << "fit " << fit->id << " (" << ship->name << ") " << goalNames[g] << ": plan goal "
                              << got.goal << " cpu " << got.cpuUsed << " cap " << got.capUsed << "/" << got.capRecharge
                              << " vs engine goal " << want.goal << " cpu " << want.cpuUsed << " cap " << want.capUsed
                              << "/" << want.capRecharge << "\n";
            }
        }
    }
    std::cerr << fits << " fits, " << tests << " tests: " << mismatches << " mismatches, " << detailMismatches
              << " detail mismatches\n"
              << "instructions per plan: " << instructions / std::max<size_t>(fits, 1) << " recorded, "
              << dynamic / std::max<size_t>(fits, 1) << " replayed per test; plans with order-sensitive reads: "
              << orderFits << "\n"
              << "plan build " << buildSeconds * 1e3 / std::max<size_t>(fits, 1) << " ms, test "
              << planSeconds * 1e6 / std::max<size_t>(planCalls, 1) << " us vs full evaluation "
              << fullSeconds * 1e6 / std::max<size_t>(fullCalls, 1) << " us\n";
    return mismatches || detailMismatches ? 1 : 0;
}

// Profiling case: one SearchPlan over a fit's mid and low slots (each: the fitted
// module plus up to 7 others of its group), combinations enumerated in order on
// one thread for `seconds`. Run under the Visual Studio profiler.
int bench(const GameData& gd, const Database& save, int64_t fitID, double seconds, const std::string& goalName)
{
    using namespace fitengine;
    const std::pair<const char*, Goal> goals[] = {{"shieldPassive", Goal::ShieldPassive},
                                                  {"armorPassive", Goal::ArmorPassive},
                                                  {"hullPassive", Goal::HullPassive},
                                                  {"shieldActive", Goal::ShieldActive},
                                                  {"armorActive", Goal::ArmorActive}};
    Goal goal = Goal::ArmorActive;
    for (const auto& [name, g] : goals)
        if (goalName == name)
            goal = g;

    auto isMidOrLow = [&](int typeID) {
        const Type* t = gd.type(typeID);
        auto slot = t ? gd.moduleSlot(*t) : std::nullopt;
        return slot && (*slot == Slot::Med || *slot == Slot::Low);
    };
    if (!fitID)
    {
        // Default: the saved fit with the most mid and low slot modules.
        size_t best = 0;
        for (const FitSummary& f : recentFits(save, 1 << 30))
            if (auto fit = loadFit(save, f.id))
            {
                size_t n = std::count_if(fit->modules.begin(), fit->modules.end(),
                                         [&](const FitModule& m) { return m.itemID && isMidOrLow(*m.itemID); });
                if (n > best)
                    best = n, fitID = f.id;
            }
    }
    auto fit = loadFit(save, fitID);
    if (!fit)
    {
        std::cerr << "no fit #" << fitID << "\n";
        return 1;
    }
    std::vector<int> positions;
    FitInput in = toEngineInput(*fit, gd, positions);

    SearchSpec spec;
    spec.base = in;
    spec.base.modules.clear();
    for (const ModuleInput& m : in.modules)
    {
        if (!isMidOrLow(m.typeID))
        {
            spec.base.modules.push_back(m);
            continue;
        }
        std::vector<ModuleInput> options{m};
        for (int id : gd.typeIDsInGroup(gd.type(m.typeID)->groupID))
        {
            if (options.size() >= 8)
                break;
            if (id == m.typeID)
                continue;
            ModuleInput alt;
            alt.typeID = id;
            alt.chargeID = m.chargeID;
            alt.state = m.state;
            options.push_back(alt);
        }
        spec.slots.push_back(std::move(options));
    }

    Character ch = gd.loadCharacter(5);
    Engine engine(gd, ch);
    auto t0 = std::chrono::steady_clock::now();
    SearchPlan plan(engine, spec);
    double buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    auto st = plan.stats();
    double combinations = 1;
    for (const auto& slot : spec.slots)
        combinations *= static_cast<double>(slot.size());
    std::cerr << "fit #" << fit->id << " " << fit->name << " [" << gd.type(fit->shipID)->name << "]: "
              << spec.slots.size() << " open slots, " << combinations << " combinations, goal " << goalName << "\n"
              << "plan: built in " << buildMs << " ms, " << st.instructions << " instructions, " << st.dynamic
              << " replayed per test; " << st.itemsWritten << " of " << st.items << " items written per test; "
              << st.variableCycle << " of " << st.modules << " modules with per-test cycle values, "
              << st.variableFitting << " with per-test fitting values\n";

    // Odometer enumeration, like a brute-force search.
    std::vector<int> choice(spec.slots.size(), 0);
    float best = -1;
    size_t evaluations = 0;
    auto start = std::chrono::steady_clock::now();
    auto deadline = start + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline)
    {
        for (int batch = 0; batch < 1000; ++batch)
        {
            TestResult r = plan.evaluate(choice.data(), goal);
            if (r.cpuUsed <= r.cpuOutput && r.powerUsed <= r.powerOutput && r.goal > best)
                best = r.goal;
            ++evaluations;
            for (size_t s = 0; s < choice.size(); ++s)
            {
                if (++choice[s] < static_cast<int>(spec.slots[s].size()))
                    break;
                choice[s] = 0;
            }
        }
    }
    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cerr << evaluations << " evaluations in " << elapsed << " s: " << evaluations / elapsed << " per second ("
              << elapsed * 1e6 / static_cast<double>(evaluations) << " us each); best fitting goal value " << best
              << "\n";
    return 0;
}

std::optional<fitengine::Goal> parseGoal(const std::string& name)
{
    using fitengine::Goal;
    const std::pair<const char*, Goal> goals[] = {{"shieldPassive", Goal::ShieldPassive},
                                                  {"armorPassive", Goal::ArmorPassive},
                                                  {"hullPassive", Goal::HullPassive},
                                                  {"shieldActive", Goal::ShieldActive},
                                                  {"armorActive", Goal::ArmorActive}};
    for (const auto& [n, g] : goals)
        if (name == n)
            return g;
    return std::nullopt;
}

// The best modules for a saved fit's mid/low/rig slots (fitengine::opt).
int optimize(const GameData& gd, const Database& save, int64_t fitID, fitengine::opt::Options opt)
{
    using namespace fitengine;
    auto fit = loadFit(save, fitID);
    if (!fit)
    {
        std::cerr << "no fit #" << fitID << "\n";
        return 1;
    }
    std::vector<int> positions;
    FitInput in = toEngineInput(*fit, gd, positions);
    std::unordered_map<int, double> prices;
    try
    {
        auto st = save.prepare("SELECT typeID, price FROM prices");
        while (st.step())
            prices[static_cast<int>(st.getInt(0))] = st.getDouble(1);
    }
    catch (const std::exception&)
    {
    }
    opt.prices = prices.empty() ? nullptr : &prices;
    opt.log = &std::cerr;

    Character ch = gd.loadCharacter(5);
    Engine engine(gd, ch);
    std::cerr << "fit #" << fit->id << " " << fit->name << " [" << gd.type(fit->shipID)->name << "], "
              << prices.size() << " prices\n";
    opt::Result res = opt::optimize(gd, engine, in, opt);
    for (const auto& w : res.warnings)
        std::cerr << "warning: " << w << "\n";
    if (res.infeasible.infeasible)
        std::cerr << "infeasible (" << res.infeasible.reason << "): " << res.infeasible.message << "\n";

    auto line = [&](const TestResult& r) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "goal %.2f | cpu %.2f/%.2f pg %.2f/%.2f cal %.0f/%.0f | cap %.2f/%.2f",
                      r.goal, r.cpuUsed, r.cpuOutput, r.powerUsed, r.powerOutput, r.calibrationUsed,
                      r.calibrationOutput, r.capUsed, r.capRecharge);
        return std::string(buf);
    };
    std::cout << "start: " << line(res.start) << "\n\n";
    for (size_t i = 0; i < res.fits.size(); ++i)
    {
        const opt::FoundFit& f = res.fits[i];
        std::cout << "#" << i + 1 << (f.fits ? "" : " (does not fit)") << ": " << line(f.exact) << " | gain "
                  << f.exact.goal - res.start.goal;
        if (opt.prices)
            std::cout << " | " << f.price / 1e6 << "M ISK";
        std::cout << "\n";
        for (int r = 0; r < opt::RackCount; ++r)
        {
            if (f.typeIDs[r].empty())
                continue;
            std::cout << "    " << opt::rackName(r) << ":";
            std::map<std::string, int> names;
            for (int id : f.typeIDs[r])
                ++names[gd.type(id)->name];
            for (const auto& [n, c] : names)
                std::cout << " " << (c > 1 ? std::to_string(c) + "x " : "") << n << ";";
            std::cout << "\n";
        }
    }
    const auto& s = res.stats;
    std::cerr << "\ntime: candidates " << s.seconds[0] << " s, model " << s.seconds[1] << " s, stage 1 "
              << s.seconds[2] << " s, stage 2 " << s.seconds[3] << " s, total " << s.seconds[4] << " s; "
              << s.engineCalls << " engine calls\n";
    return 0;
}

// Stage 1 against brute force on small open-slot configurations (Options::maxSlots),
// for each fit and goal. Returns 1 if any case mismatched.
int optimizeCheck(const GameData& gd, const Database& save, const std::vector<int64_t>& fitIDs,
                  const std::vector<fitengine::Goal>& goals, fitengine::opt::Options opt, bool verbose)
{
    using namespace fitengine;
    std::unordered_map<int, double> prices;
    try
    {
        auto st = save.prepare("SELECT typeID, price FROM prices");
        while (st.step())
            prices[static_cast<int>(st.getInt(0))] = st.getDouble(1);
    }
    catch (const std::exception&)
    {
    }
    opt.prices = prices.empty() ? nullptr : &prices;
    opt.bruteForce = true;
    opt.log = verbose ? &std::cerr : nullptr;
    const char* goalNames[] = {"shieldPassive", "armorPassive", "hullPassive", "shieldActive", "armorActive"};
    Character ch = gd.loadCharacter(5);
    Engine engine(gd, ch);
    size_t cases = 0, bad = 0;
    for (int64_t id : fitIDs)
    {
        auto fit = loadFit(save, id);
        if (!fit)
        {
            std::cerr << "no fit #" << id << "\n";
            continue;
        }
        std::vector<int> positions;
        FitInput in = toEngineInput(*fit, gd, positions);
        for (Goal g : goals)
        {
            opt.goal = g;
            auto t0 = std::chrono::steady_clock::now();
            opt::Result res = opt::optimize(gd, engine, in, opt);
            double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            const opt::BruteForceCheck& c = res.check;
            ++cases;
            bool ok = c.ran && c.mismatches == 0 && c.fittingGoalChanges == 0;
            bad += ok ? 0 : 1;
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                          "#%-5lld %-14s %-13s %10.0f combos %8zu fit | best %12.4f vs stage 1 %12.4f | %3zu ranks, "
                          "max err %.2g | %s (%.1f s)",
                          static_cast<long long>(id), gd.type(fit->shipID)->name.c_str(),
                          goalNames[static_cast<int>(g)], c.combinations, c.feasible, c.bruteBest, c.stage1Best,
                          c.compared, c.maxError,
                          ok ? "ok" : ("MISMATCH " + std::to_string(c.mismatches) +
                                       (c.fittingGoalChanges ? " (fitting changed goal)" : ""))
                                          .c_str(),
                          sec);
            std::cout << buf << std::endl;
        }
    }
    std::cout << cases << " cases, " << bad << " with mismatches\n";
    return bad ? 1 : 0;
}

std::unordered_map<int, double> loadPrices(const Database& save)
{
    std::unordered_map<int, double> prices;
    try
    {
        auto st = save.prepare("SELECT typeID, price FROM prices");
        while (st.step())
            prices[static_cast<int>(st.getInt(0))] = st.getDouble(1);
    }
    catch (const std::exception&)
    {
    }
    return prices;
}

std::string dumpJson(const json::Value& v)
{
    return v.dump(2, ' ', false, json::Value::error_handler_t::replace) + "\n";
}

bool writeText(const std::optional<fs::path>& path, const std::string& text)
{
    if (!path || path->wstring() == L"-")
    {
        std::cout << text << std::flush;
        return true;
    }
    std::ofstream f(*path, std::ios::binary);
    f << text;
    return static_cast<bool>(f);
}

// --optimize-request: JSON request in, JSON response out. Exit code 0 ok, 1 check
// mismatch (--check), 2 error (the response then has status "error").
int optimizeRequest(const GameData& gd, const Database* save, const fs::path& in, const std::optional<fs::path>& out,
                    const request::RunOptions& ro, int threads)
{
    json::Value response;
    int code = 0;
    try
    {
        std::string text;
        if (in.wstring() == L"-")
            text.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
        else
        {
            std::ifstream f(in, std::ios::binary);
            if (!f)
                throw std::runtime_error("cannot read " + toUtf8(in));
            text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }
        if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0)
            text.erase(0, 3);  // UTF-8 BOM
        request::Request req = request::parse(json::Value::parse(text), gd);
        if (threads)
            req.threads = threads;
        std::unordered_map<int, double> prices;
        if (!req.prices && save)
            prices = loadPrices(*save);
        bool ok = true;
        response = request::run(gd, req, prices.empty() ? nullptr : &prices, ro, ok);
        code = ok ? 0 : 1;
        if (response.contains("check"))
            std::cerr << "check: " << response["check"].dump() << "\n";
    }
    catch (const std::exception& e)
    {
        response = request::errorResponse(e.what());
        std::cerr << "error: " << e.what() << "\n";
        code = 2;
    }
    if (!writeText(out, dumpJson(response)))
    {
        std::cerr << "cannot write " << toUtf8(*out) << "\n";
        return 2;
    }
    return code;
}

int makeRequest(const GameData& gd, const Database& save, int64_t fitID, const std::string& open, fitengine::Goal goal,
                const std::optional<fs::path>& out)
{
    auto fit = loadFit(save, fitID);
    if (!fit)
    {
        std::cerr << "no fit #" << fitID << "\n";
        return 1;
    }
    Character ch = gd.loadCharacter(5);
    fitengine::Engine engine(gd, ch);
    json::Value doc = request::makeRequest(gd, engine, *fit, open, goal);
    return writeText(out, dumpJson(doc)) ? 0 : 1;
}

int wmain(int argc, wchar_t* argv[])
{
    SetConsoleOutputCP(CP_UTF8);

    std::optional<fs::path> gameDb, saveDb, engineDumpPath;
    std::optional<int> searchCheckFits;
    std::optional<std::array<std::string, 3>> benchArgs;  // fitID, seconds, goal
    std::optional<int64_t> optimizeFit;
    fitengine::opt::Options optimizeOpt;
    std::vector<int64_t> checkFits;
    std::vector<fitengine::Goal> checkGoals;
    std::optional<std::array<int, fitengine::opt::RackCount>> slotsOverride;
    bool verbose = false;
    std::optional<fs::path> requestIn, outPath;
    std::optional<int64_t> makeRequestFit;
    std::string openSpec = "all";
    std::optional<fitengine::Goal> goalOverride;
    bool check = false;
    bool members = false, refineOff = false;
    std::optional<double> timeBudget, refineExact;
    std::optional<int> refineRounds, refinePatience, refinePairs, refineChains, refineStarts;
    std::optional<std::string> fitQuery;
    for (int i = 1; i < argc; ++i)
    {
        std::wstring arg = argv[i];
        if (arg == L"--game" && i + 1 < argc)
            gameDb = argv[++i];
        else if (arg == L"--save" && i + 1 < argc)
            saveDb = argv[++i];
        else if (arg == L"--engine-dump" && i + 1 < argc)
            engineDumpPath = argv[++i];
        else if (arg == L"--bench")
        {
            std::array<std::string, 3> a{"0", "10", "armorActive"};
            for (size_t k = 0; k < a.size() && i + 1 < argc && argv[i + 1][0] != L'-'; ++k)
                a[k] = toUtf8(std::wstring(argv[++i]));
            benchArgs = a;
        }
        else if (arg == L"--optimize" && i + 2 < argc)
        {
            std::string id = toUtf8(std::wstring(argv[++i]));
            optimizeFit = std::stoll(id[0] == '#' ? id.substr(1) : id);
            auto goal = parseGoal(toUtf8(std::wstring(argv[++i])));
            if (!goal)
            {
                std::cerr << "unknown goal (shieldPassive, armorPassive, hullPassive, shieldActive, armorActive)\n";
                return 1;
            }
            optimizeOpt.goal = *goal;
        }
        else if (arg == L"--racks" && i + 1 < argc)
        {
            std::string racks = toUtf8(std::wstring(argv[++i]));
            for (int r = 0; r < fitengine::opt::RackCount; ++r)
                optimizeOpt.search[static_cast<size_t>(r)] = racks.find(fitengine::opt::rackName(r)) != std::string::npos;
        }
        else if (arg == L"--price-cap" && i + 1 < argc)
            optimizeOpt.priceCap = std::stod(toUtf8(std::wstring(argv[++i])));
        else if (arg == L"--allow-officer")
            optimizeOpt.allowOfficer = true;
        else if (arg == L"--top" && i + 1 < argc)
            optimizeOpt.topK = std::stoi(toUtf8(std::wstring(argv[++i])));
        else if (arg == L"--results" && i + 1 < argc)
            optimizeOpt.results = std::stoi(toUtf8(std::wstring(argv[++i])));
        else if (arg == L"--dump-kinds")
            optimizeOpt.dumpKinds = true;
        else if (arg == L"--optimize-check" && i + 1 < argc)
        {
            std::string ids = toUtf8(std::wstring(argv[++i]));
            for (size_t at = 0; at < ids.size();)
            {
                size_t end = ids.find(',', at);
                if (end == std::string::npos)
                    end = ids.size();
                std::string id = ids.substr(at, end - at);
                if (!id.empty())
                    checkFits.push_back(std::stoll(id[0] == '#' ? id.substr(1) : id));
                at = end + 1;
            }
            std::string g = i + 1 < argc ? toUtf8(std::wstring(argv[i + 1])) : "";
            if (auto goal = parseGoal(g))
            {
                checkGoals = {*goal};
                ++i;
            }
            else
            {
                if (g == "all")
                    ++i;
                for (int k = 0; k < 5; ++k)
                    checkGoals.push_back(static_cast<fitengine::Goal>(k));
            }
            optimizeOpt.maxSlots = {2, 2, 1};
        }
        else if (arg == L"--slots" && i + 1 < argc)
        {
            std::string v = toUtf8(std::wstring(argv[++i]));
            std::array<int, fitengine::opt::RackCount> m{};
            if (sscanf_s(v.c_str(), "%d,%d,%d", &m[0], &m[1], &m[2]) == 3)
                slotsOverride = m;
        }
        else if (arg == L"--threads" && i + 1 < argc)
            optimizeOpt.threads = std::stoi(toUtf8(std::wstring(argv[++i])));
        else if (arg == L"--verbose")
            verbose = true;
        else if (arg == L"--optimize-request" && i + 1 < argc)
            requestIn = fs::path(argv[++i]);
        else if (arg == L"--out" && i + 1 < argc)
            outPath = fs::path(argv[++i]);
        else if (arg == L"--make-request" && i + 1 < argc)
        {
            std::string id = toUtf8(std::wstring(argv[++i]));
            makeRequestFit = std::stoll(id[0] == '#' ? id.substr(1) : id);
        }
        else if (arg == L"--open" && i + 1 < argc)
            openSpec = toUtf8(std::wstring(argv[++i]));
        else if (arg == L"--goal" && i + 1 < argc)
        {
            goalOverride = request::parseGoal(toUtf8(std::wstring(argv[++i])));
            if (!goalOverride)
            {
                std::cerr << "unknown goal (shieldPassive, armorPassive, hullPassive, shieldActive, armorActive)\n";
                return 2;
            }
        }
        else if (arg == L"--check")
            check = true;
        else if (arg == L"--members")
            members = true;
        else if (arg == L"--no-refine")
            refineOff = true;
        else if (arg == L"--time-budget" && i + 1 < argc)
            timeBudget = std::stod(toUtf8(std::wstring(argv[++i])));
        else if (arg == L"--refine-rounds" && i + 1 < argc)
            refineRounds = std::stoi(toUtf8(std::wstring(argv[++i])));
        else if (arg == L"--refine-patience" && i + 1 < argc)
            refinePatience = std::stoi(toUtf8(std::wstring(argv[++i])));
        else if (arg == L"--refine-pairs" && i + 1 < argc)
            refinePairs = std::stoi(toUtf8(std::wstring(argv[++i])));
        else if (arg == L"--refine-chains" && i + 1 < argc)
            refineChains = std::stoi(toUtf8(std::wstring(argv[++i])));
        else if (arg == L"--refine-exact" && i + 1 < argc)
            refineExact = std::stod(toUtf8(std::wstring(argv[++i])));
        else if (arg == L"--refine-starts" && i + 1 < argc)
            refineStarts = std::stoi(toUtf8(std::wstring(argv[++i])));
        else if (arg == L"--search-check")
            searchCheckFits = (i + 1 < argc && iswdigit(argv[i + 1][0])) ? std::stoi(argv[++i]) : 0;
        else if (!fitQuery)
            fitQuery = toUtf8(arg);
        else
            *fitQuery += " " + toUtf8(arg);  // allow unquoted names with spaces
    }

    if (!gameDb)
        gameDb = pyfa::findGameDb(exeDir());
    if (!gameDb)
        gameDb = pyfa::findGameDb(fs::current_path());
    if (!gameDb)
    {
        std::cerr << "Could not find eve.db; pass --game <path>\n";
        if (requestIn)
            writeText(outPath, dumpJson(request::errorResponse("could not find eve.db; pass --game <path>")));
        return requestIn ? 2 : 1;
    }
    if (!saveDb)
        saveDb = pyfa::findSaveDb(pyfa::findPyfaRoot(gameDb->parent_path()));
    if (requestIn)
    {
        // saveddata.db is optional here: only pyfa's prices table is read from it.
        try
        {
            fs::path modifiersTsv = exeDir() / "effect_modifiers.tsv";
            if (!fs::is_regular_file(modifiersTsv))
                throw std::runtime_error(toUtf8(modifiersTsv) + " not found");
            GameData gd = GameData::load(*gameDb, modifiersTsv);
            std::optional<Database> save;
            if (saveDb)
                save.emplace(*saveDb);
            request::RunOptions ro;
            ro.check = check;
            ro.goal = goalOverride;
            ro.log = verbose ? &std::cerr : nullptr;
            if (slotsOverride)
                ro.maxSlots = *slotsOverride;
            ro.members = members;
            if (refineOff)
                ro.refine = false;
            ro.timeBudget = timeBudget;
            ro.refineRounds = refineRounds;
            ro.refinePatience = refinePatience;
            ro.refinePairs = refinePairs;
            ro.refineChains = refineChains;
            ro.refineStarts = refineStarts;
            ro.refineExact = refineExact;
            ro.progress = [](double f, const std::string& phase) {
                std::fprintf(stderr, "progress %.3f %s\n", f, phase.c_str());
                std::fflush(stderr);
            };
            return optimizeRequest(gd, save ? &*save : nullptr, *requestIn, outPath, ro, optimizeOpt.threads);
        }
        catch (const std::exception& e)
        {
            std::cerr << "Error: " << e.what() << "\n";
            writeText(outPath, dumpJson(request::errorResponse(e.what())));
            return 2;
        }
    }
    if (!saveDb)
    {
        std::cerr << "Could not find saveddata.db (looked in " << toUtf8(pyfa::defaultSavePath())
                  << "); pass --save <path>\n";
        return 1;
    }

    try
    {
        auto start = std::chrono::steady_clock::now();
        // Copied next to the exe by the build; ships load without bonuses if it's missing.
        fs::path modifiersTsv = exeDir() / "effect_modifiers.tsv";
        if (!fs::is_regular_file(modifiersTsv))
        {
            std::cerr << "warning: " << toUtf8(modifiersTsv) << " not found; ship bonuses unavailable\n";
            modifiersTsv.clear();
        }
        GameData gd = GameData::load(*gameDb, modifiersTsv);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        std::cerr << "Loaded " << gd.typeCount() << " types (build " << gd.clientBuild << ") from "
                  << toUtf8(*gameDb) << " in " << ms.count() << " ms\n"
                  << "Fits from " << toUtf8(*saveDb) << "\n\n";

        Database save(*saveDb);

        if (engineDumpPath)
            return engineDump(gd, save, *engineDumpPath);
        if (searchCheckFits)
            return searchCheck(gd, save, *searchCheckFits);
        if (slotsOverride)
            optimizeOpt.maxSlots = *slotsOverride;
        optimizeOpt.refine = !refineOff;
        optimizeOpt.memberBruteForce = members;
        if (timeBudget)
            optimizeOpt.timeBudget = *timeBudget;
        if (refineRounds)
            optimizeOpt.refineRounds = *refineRounds;
        if (refinePatience)
            optimizeOpt.refinePatience = *refinePatience;
        if (refinePairs)
            optimizeOpt.refinePairs = *refinePairs;
        if (refineChains)
            optimizeOpt.refineChains = *refineChains;
        if (refineStarts)
            optimizeOpt.refineStarts = *refineStarts;
        if (refineExact)
            optimizeOpt.refineExact = *refineExact;
        if (makeRequestFit)
            return makeRequest(gd, save, *makeRequestFit, openSpec, goalOverride.value_or(fitengine::Goal::ArmorActive),
                               outPath);
        if (!checkFits.empty())
            return optimizeCheck(gd, save, checkFits, checkGoals, optimizeOpt, verbose);
        if (optimizeFit)
            return optimize(gd, save, *optimizeFit, optimizeOpt);
        if (benchArgs)
            return bench(gd, save, std::stoll((*benchArgs)[0]), std::stod((*benchArgs)[1]), (*benchArgs)[2]);

        if (!fitQuery)
        {
            std::cout << "Most recently modified fits:\n";
            for (const FitSummary& f : recentFits(save, 20))
                std::cout << "  " << describe(f, gd) << "\n";
            std::cout << "\nUsage: PyfaReader [--game <eve.db>] [--save <saveddata.db>] [<fit name> | #<fitID>]\n";
            return 0;
        }

        int64_t fitID = 0;
        if (fitQuery->size() > 1 && (*fitQuery)[0] == '#')
            fitID = std::stoll(fitQuery->substr(1));
        else
        {
            auto matches = findFitsByName(save, *fitQuery);
            if (matches.empty())
            {
                std::cerr << "No fit named \"" << *fitQuery << "\"\n";
                return 1;
            }
            fitID = matches.front().id;
            if (matches.size() > 1)
            {
                std::cerr << matches.size() << " fits match; exporting the most recently modified. "
                          << "Use #<fitID> to pick another:\n";
                for (const FitSummary& f : matches)
                    std::cerr << "  " << describe(f, gd) << "\n";
                std::cerr << "\n";
            }
        }

        auto fit = loadFit(save, fitID);
        if (!fit)
        {
            std::cerr << "No fit with ID " << fitID << "\n";
            return 1;
        }

        std::vector<std::string> warnings;
        std::cout << exportEft(*fit, gd, &warnings) << "\n";
        for (const auto& w : warnings)
            std::cerr << "warning: " << w << "\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
