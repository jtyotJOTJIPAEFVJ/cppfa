#pragma once

// Locates pyfa's databases the same way config.py does:
//   game data : <pyfa root>\eve.db
//   saved fits: %USERPROFILE%\.pyfa\saveddata.db
//               (or <pyfa root>\saveddata\saveddata.db when pyfa runs with saveInRoot)

#include <filesystem>
#include <optional>

namespace pyfa
{
// Walks up from `start` looking for a directory containing eve.db.
std::optional<std::filesystem::path> findPyfaRoot(const std::filesystem::path& start);

std::filesystem::path defaultSavePath();  // %USERPROFILE%\.pyfa

std::optional<std::filesystem::path> findGameDb(const std::filesystem::path& start);
std::optional<std::filesystem::path> findSaveDb(const std::optional<std::filesystem::path>& pyfaRoot);
}
