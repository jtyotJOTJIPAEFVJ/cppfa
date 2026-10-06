#pragma once

// Python / pyfa numeric helpers, reproduced exactly so results match pyfa.

#include <charconv>
#include <cmath>

namespace pymath
{
// Python's round(x, ndigits) for ndigits >= 0: correctly rounded on the exact
// binary value (round-half-even on exact ties), like CPython.
inline double round(double value, int ndigits)
{
    if (!std::isfinite(value))
        return value;
    char buf[512];
    auto res = std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::fixed, ndigits);
    double out = value;
    std::from_chars(buf, res.ptr, out);
    return out;
}

// eos.utils.float.floatUnerr: round away float noise, keeping 7 significant digits.
inline double floatUnerr(double value)
{
    if (value == 0.0 || std::isinf(value))
        return value;
    constexpr int keepDigits = 7;  // int(sys.float_info.dig / 2)
    int roundFactor = keepDigits - static_cast<int>(std::ceil(std::log10(std::abs(value))));
    if (roundFactor < 0)
    {
        double scale = std::pow(10.0, -roundFactor);
        return std::nearbyint(value / scale) * scale;
    }
    return round(value, roundFactor);
}
}
