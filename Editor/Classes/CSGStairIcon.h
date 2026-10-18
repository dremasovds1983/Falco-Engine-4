#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace GX
{
    struct CSGIconStep
    {
        std::array<float, 3> position;
        std::array<float, 3> scale;
    };

    // Older scenes have no stair component. The builder's indexed child names
    // survive save/load and individual step edits. Require the complete, unique
    // sequence rather than treating any object named "Stair" as a staircase.
    inline bool hasCSGStairStepNames(const std::vector<std::string>& names)
    {
        if (names.size() < 2) return false;
        std::vector<bool> seen(names.size(), false);
        for (const auto& name : names)
        {
            if (name.compare(0, 5, "Step ") != 0 || name.size() == 5) return false;
            size_t index = 0;
            for (size_t i = 5; i < name.size(); ++i)
            {
                if (name[i] < '0' || name[i] > '9') return false;
                const size_t digit = static_cast<size_t>(name[i] - '0');
                if (index > (names.size() - 1) / 10) return false;
                index = index * 10 + digit;
                if (index >= names.size()) return false;
            }
            if (name != "Step " + std::to_string(index) || seen[index]) return false;
            seen[index] = true;
        }
        return true;
    }

    // The stair builder stores an ordinary group of cube brushes, not a stair
    // component. Recognize its open/closed layout without relying on names or
    // adding editor-only metadata to the scene format. Renaming/reordering and
    // transforming the parent do not affect this local-space classification.
    inline bool isCSGStairLayout(std::vector<CSGIconStep> steps)
    {
        if (steps.size() < 2) return false;
        for (const auto& step : steps)
            for (size_t axis = 0; axis < 3; ++axis)
                if (!std::isfinite(step.position[axis]) ||
                    !std::isfinite(step.scale[axis]) || step.scale[axis] <= 0)
                    return false;
        std::sort(steps.begin(), steps.end(), [](const CSGIconStep& a, const CSGIconStep& b) {
            return a.position[1] < b.position[1];
        });
        const auto first = steps.front();
        const float depth = steps.back().scale[2];
        auto approximatelyEqual = [](float a, float b) {
            return std::abs(a - b) <= 0.0001f * std::max(1.0f, std::max(std::abs(a), std::abs(b)));
        };
        bool open = true, closed = true;
        for (size_t i = 0; i < steps.size(); ++i)
        {
            const auto& step = steps[i];
            const float index = static_cast<float>(i);
            if (!approximatelyEqual(step.scale[0], first.scale[0]) || !approximatelyEqual(step.scale[1], first.scale[1]) ||
                !approximatelyEqual(step.position[0], first.position[0]) ||
                !approximatelyEqual(step.position[1], first.position[1] + index * 2 * first.scale[1]))
                return false;
            open = open && approximatelyEqual(step.scale[2], depth) &&
                approximatelyEqual(step.position[2], first.position[2] - index * 2 * depth);
            closed = closed && approximatelyEqual(step.scale[2], static_cast<float>(steps.size() - i) * depth) &&
                approximatelyEqual(step.position[2], first.position[2] - index * depth);
        }
        return open || closed;
    }
}
