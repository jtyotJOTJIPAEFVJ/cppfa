#include "Fit.h"

#include "Sqlite.h"

namespace
{
std::optional<int> optInt(const Statement& st, int col)
{
    if (auto v = st.getOptInt(col))
        return static_cast<int>(*v);
    return std::nullopt;
}

const char* kSummaryColumns = "SELECT ID, name, shipID, IFNULL(modified, IFNULL(created, '')) FROM fits ";
const char* kRecentOrder = " ORDER BY IFNULL(modified, created) DESC, ID DESC";

std::vector<FitSummary> readSummaries(Statement& st)
{
    std::vector<FitSummary> out;
    while (st.step())
        out.push_back({st.getInt(0), st.getText(1), static_cast<int>(st.getInt(2)), st.getText(3)});
    return out;
}
}

std::vector<FitSummary> findFitsByName(const Database& saveDb, const std::string& name)
{
    const char* conditions[] = {
        "WHERE name = ?1",
        "WHERE name = ?1 COLLATE NOCASE",
        "WHERE instr(lower(name), lower(?1)) > 0",
    };
    for (const char* cond : conditions)
    {
        auto st = saveDb.prepare(std::string(kSummaryColumns) + cond + kRecentOrder);
        st.bind(1, name);
        auto result = readSummaries(st);
        if (!result.empty())
            return result;
    }
    return {};
}

std::vector<FitSummary> recentFits(const Database& saveDb, int limit)
{
    auto st = saveDb.prepare(std::string(kSummaryColumns) + kRecentOrder + " LIMIT ?1");
    st.bind(1, static_cast<int64_t>(limit));
    return readSummaries(st);
}

std::optional<Fit> loadFit(const Database& saveDb, int64_t fitID)
{
    Fit fit;
    {
        auto st = saveDb.prepare("SELECT ID, name, shipID, IFNULL(notes, ''), modeID FROM fits WHERE ID = ?1");
        st.bind(1, fitID);
        if (!st.step())
            return std::nullopt;
        fit.id = st.getInt(0);
        fit.name = st.getText(1);
        fit.shipID = static_cast<int>(st.getInt(2));
        fit.notes = st.getText(3);
        fit.modeID = optInt(st, 4);
    }
    {
        auto mutators = saveDb.prepare("SELECT attrID, value FROM mutators WHERE moduleID = ?1");
        auto st = saveDb.prepare(R"(
            SELECT ID, itemID, dummySlot, baseItemID, mutaplasmidID, chargeID, IFNULL(state, 0), IFNULL(position, 0)
            FROM modules WHERE fitID = ?1 AND projected = 0
            ORDER BY position, ID)");
        st.bind(1, fitID);
        while (st.step())
        {
            FitModule m;
            m.itemID = optInt(st, 1);
            m.dummySlot = optInt(st, 2);
            m.baseItemID = optInt(st, 3);
            m.mutaplasmidID = optInt(st, 4);
            m.chargeID = optInt(st, 5);
            m.state = static_cast<ModuleState>(st.getInt(6));
            m.position = static_cast<int>(st.getInt(7));
            if (m.isMutated())
            {
                mutators.reset();
                mutators.bind(1, st.getInt(0));
                while (mutators.step())
                    m.mutators[static_cast<int>(mutators.getInt(0))] = mutators.getDouble(1);
            }
            fit.modules.push_back(std::move(m));
        }
    }
    {
        auto mutators = saveDb.prepare("SELECT attrID, value FROM mutatorsDrones WHERE groupID = ?1");
        auto st = saveDb.prepare(R"(
            SELECT groupID, itemID, baseItemID, mutaplasmidID, amount, amountActive
            FROM drones WHERE fitID = ?1 AND IFNULL(projected, 0) = 0
            ORDER BY groupID)");
        st.bind(1, fitID);
        while (st.step())
        {
            FitDrone d;
            d.itemID = static_cast<int>(st.getInt(1));
            d.baseItemID = optInt(st, 2);
            d.mutaplasmidID = optInt(st, 3);
            d.amount = static_cast<int>(st.getInt(4));
            d.amountActive = static_cast<int>(st.getInt(5));
            if (d.isMutated())
            {
                mutators.reset();
                mutators.bind(1, st.getInt(0));
                while (mutators.step())
                    d.mutators[static_cast<int>(mutators.getInt(0))] = mutators.getDouble(1);
            }
            fit.drones.push_back(std::move(d));
        }
    }
    {
        auto st = saveDb.prepare(R"(
            SELECT itemID, amount, IFNULL(active, 0)
            FROM fighters WHERE fitID = ?1 AND IFNULL(projected, 0) = 0
            ORDER BY groupID)");
        st.bind(1, fitID);
        while (st.step())
            fit.fighters.push_back({static_cast<int>(st.getInt(0)), static_cast<int>(st.getInt(1)), st.getInt(2) != 0});
    }
    {
        auto st = saveDb.prepare(R"(
            SELECT i.itemID FROM fitImplants fi JOIN implants i ON i.ID = fi.implantID
            WHERE fi.fitID = ?1 AND i.itemID IS NOT NULL ORDER BY i.ID)");
        st.bind(1, fitID);
        while (st.step())
            fit.implants.push_back({static_cast<int>(st.getInt(0)), 1});
    }
    {
        auto st = saveDb.prepare("SELECT itemID FROM boosters WHERE fitID = ?1 AND itemID IS NOT NULL ORDER BY ID");
        st.bind(1, fitID);
        while (st.step())
            fit.boosters.push_back({static_cast<int>(st.getInt(0)), 1});
    }
    {
        auto st = saveDb.prepare("SELECT itemID, amount FROM cargo WHERE fitID = ?1 ORDER BY ID");
        st.bind(1, fitID);
        while (st.step())
            fit.cargo.push_back({static_cast<int>(st.getInt(0)), static_cast<int>(st.getInt(1))});
    }
    return fit;
}
