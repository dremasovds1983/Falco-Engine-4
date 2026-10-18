#pragma once
#include <array>
#include <cmath>

namespace GX
{
    // Double precision avoids rejecting small but valid atlas triangles with
    // the old absolute 1e-6 area threshold. Collapsed UVs never shade a texel.
    template<class UV> double bakeUvArea(const UV& a, const UV& b, const UV& c)
    {
        return (double(b.x)-a.x)*(double(c.y)-a.y) - (double(b.y)-a.y)*(double(c.x)-a.x);
    }
    template<class UV> bool bakeUvInRange(const UV& uv)
    {
        return std::isfinite(uv.x) && std::isfinite(uv.y) &&
            uv.x >= -1e-6 && uv.x <= 1.000001 && uv.y >= -1e-6 && uv.y <= 1.000001;
    }
    template<class UV> std::array<double, 3> bakeBarycentric(const UV& a, const UV& b, const UV& c, const UV& p)
    {
        const double area = bakeUvArea(a, b, c);
        if (!std::isfinite(area) || area == 0) return {-1, -1, -1};
        const double x = bakeUvArea(p, b, c) / area;
        const double y = bakeUvArea(a, p, c) / area;
        return {x, y, 1.0-x-y};
    }
}
