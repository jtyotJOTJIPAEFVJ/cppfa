#pragma once

// nlohmann/json 3.11.3 (single header, MIT license: third_party/nlohmann/json.hpp).
// Included through this wrapper so the header's own warnings don't fail the /W4 /WX build.
// ordered_json keeps object members in insertion order (readable responses).

#pragma warning(push, 0)
#include <nlohmann/json.hpp>
#pragma warning(pop)

namespace json
{
using Value = nlohmann::ordered_json;
}
