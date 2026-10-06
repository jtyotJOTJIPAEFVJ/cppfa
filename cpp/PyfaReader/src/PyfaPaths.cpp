#include "PyfaPaths.h"

#include <cstdlib>

namespace fs = std::filesystem;

namespace pyfa
{
std::optional<fs::path> findPyfaRoot(const fs::path& start)
{
    std::error_code ec;
    fs::path dir = fs::absolute(start, ec);
    while (!dir.empty())
    {
        if (fs::is_regular_file(dir / "eve.db", ec))
            return dir;
        fs::path parent = dir.parent_path();
        if (parent == dir)
            break;
        dir = parent;
    }
    return std::nullopt;
}

fs::path defaultSavePath()
{
    // Mirrors os.path.expanduser("~/.pyfa") in config.py.
    wchar_t* home = nullptr;
    size_t len = 0;
    if (_wdupenv_s(&home, &len, L"USERPROFILE") == 0 && home)
    {
        fs::path p = fs::path(home) / ".pyfa";
        free(home);
        return p;
    }
    return fs::path(".pyfa");
}

std::optional<fs::path> findGameDb(const fs::path& start)
{
    if (auto root = findPyfaRoot(start))
        return *root / "eve.db";
    return std::nullopt;
}

std::optional<fs::path> findSaveDb(const std::optional<fs::path>& pyfaRoot)
{
    std::error_code ec;
    fs::path home = defaultSavePath() / "saveddata.db";
    if (fs::is_regular_file(home, ec))
        return home;
    if (pyfaRoot)
    {
        fs::path inRoot = *pyfaRoot / "saveddata" / "saveddata.db";
        if (fs::is_regular_file(inRoot, ec))
            return inRoot;
    }
    return std::nullopt;
}
}
