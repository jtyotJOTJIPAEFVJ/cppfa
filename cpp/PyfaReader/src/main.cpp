// PyfaReader - loads pyfa's game data (eve.db) into memory and exports saved
// fittings (saveddata.db) in EFT format.
//
// Usage:
//   PyfaReader [--game <eve.db>] [--save <saveddata.db>] [<fit name> | #<fitID>]
//   PyfaReader [--game ...] [--save ...] --engine-dump <out.tsv>
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
#include "PyfaPaths.h"
#include "Sqlite.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <chrono>
#include <cstdio>
#include <iostream>
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
        mi.mutators.assign(m.mutators.begin(), m.mutators.end());
        in.modules.push_back(std::move(mi));
        positions.push_back(m.position);
    }
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

int wmain(int argc, wchar_t* argv[])
{
    SetConsoleOutputCP(CP_UTF8);

    std::optional<fs::path> gameDb, saveDb, engineDumpPath;
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
        return 1;
    }
    if (!saveDb)
        saveDb = pyfa::findSaveDb(pyfa::findPyfaRoot(gameDb->parent_path()));
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
