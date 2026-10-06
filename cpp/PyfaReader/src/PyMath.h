#pragma once

// Python / pyfa numeric helpers, reproduced so results match pyfa.

#include <charconv>
#include <cmath>
#include <type_traits>

namespace pymath
{
// Python's round(x, ndigits) for ndigits >= 0: correctly rounded on the exact
// binary value (round-half-even on exact ties), like CPython.
template <typename T>
T round(T value, int ndigits)
{
    static_assert(std::is_floating_point_v<T>);
    if (!std::isfinite(value))
        return value;
    if constexpr (std::is_same_v<T, float>)
    {
        // Float inputs are approximations anyway; skip the exact decimal round trip
        // (it differs only on exact binary ties).
        static constexpr double scale[] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000};
        if (ndigits >= 0 && ndigits < 8)
            return static_cast<float>(std::nearbyint(static_cast<double>(value) * scale[ndigits]) / scale[ndigits]);
    }
    char buf[512];
    auto res = std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::fixed, ndigits);
    T out = value;
    std::from_chars(buf, res.ptr, out);
    return out;
}

// eos.utils.float.floatUnerr: round away float noise, keeping 7 significant digits.
template <typename T>
T floatUnerr(T value)
{
    static_assert(std::is_floating_point_v<T>);
    if (value == 0 || std::isinf(value))
        return value;
    constexpr int keepDigits = 7;  // int(sys.float_info.dig / 2)
    int roundFactor = keepDigits - static_cast<int>(std::ceil(std::log10(std::abs(value))));
    if (roundFactor < 0)
    {
        T scale = static_cast<T>(std::pow(10.0, -roundFactor));
        return std::nearbyint(value / scale) * scale;
    }
    return round(value, roundFactor);
}
}
