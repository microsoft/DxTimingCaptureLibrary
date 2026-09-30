// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

// A tiny squarified treemap (Bruls, Huizing, van Wijk). Given a set of weights
// and a rectangle, it hands back one rectangle per weight whose area is
// proportional to that weight, laid out to stay as square as possible. That's
// the WinDirStat look.
namespace treemap
{
    struct Rect
    {
        float X = 0;
        float Y = 0;
        float Width = 0;
        float Height = 0;

        float Area() const { return Width * Height; }
    };

    // Fills 'out' with one rect per weight, in the same order as 'weights'.
    // Zero-or-negative weights get an empty rect.
    inline void Squarify(const std::vector<double>& weights, const Rect& bounds, std::vector<Rect>& out)
    {
        out.assign(weights.size(), Rect{});

        double total = 0;
        for (double w : weights)
        {
            if (w > 0)
            {
                total += w;
            }
        }

        if (total <= 0 || bounds.Area() <= 0)
        {
            return;
        }

        // Sort indices by weight, largest first, so we can write results back in order.
        std::vector<size_t> order;
        order.reserve(weights.size());
        for (size_t i = 0; i < weights.size(); ++i)
        {
            if (weights[i] > 0)
            {
                order.push_back(i);
            }
        }
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return weights[a] > weights[b]; });

        // Scale weights to areas that sum to the bounds' area.
        const double areaScale = static_cast<double>(bounds.Area()) / total;

        Rect free = bounds;

        auto shortSide = [](const Rect& r) { return std::min(r.Width, r.Height); };

        // "Worst" aspect ratio if we lay these areas along a strip of length 'side'.
        auto worst = [](const std::vector<double>& rowAreas, double rowSum, double side) -> double
        {
            if (rowSum <= 0 || side <= 0)
            {
                return 1e300;
            }
            double rmax = 0;
            double rmin = 1e300;
            for (double a : rowAreas)
            {
                rmax = std::max(rmax, a);
                rmin = std::min(rmin, a);
            }
            const double s2 = rowSum * rowSum;
            const double side2 = side * side;
            return std::max((side2 * rmax) / s2, s2 / (side2 * rmin));
        };

        size_t i = 0;
        std::vector<double> rowAreas;   // areas in the current strip
        std::vector<size_t> rowIndices; // matching original indices

        while (i < order.size())
        {
            rowAreas.clear();
            rowIndices.clear();

            const double side = shortSide(free);
            double rowSum = 0;

            // Grow the strip while doing so keeps the aspect ratio improving.
            while (i < order.size())
            {
                const double area = weights[order[i]] * areaScale;

                const double currentWorst = rowAreas.empty() ? 1e300 : worst(rowAreas, rowSum, side);

                rowAreas.push_back(area);
                const double candidateWorst = worst(rowAreas, rowSum + area, side);

                if (rowAreas.size() > 1 && candidateWorst > currentWorst)
                {
                    // Adding this one made things worse - back it out and lay the strip.
                    rowAreas.pop_back();
                    break;
                }

                rowSum += area;
                rowIndices.push_back(order[i]);
                ++i;
            }

            // Lay the strip along the short side, consuming from the long side.
            const bool horizontalStrip = free.Width >= free.Height;
            const double stripThickness = (side > 0) ? rowSum / side : 0;

            double offset = 0;
            for (size_t k = 0; k < rowIndices.size(); ++k)
            {
                const double cellLength = (rowSum > 0) ? (rowAreas[k] / rowSum) * side : 0;

                Rect cell;
                if (horizontalStrip)
                {
                    cell.X = free.X;
                    cell.Y = free.Y + static_cast<float>(offset);
                    cell.Width = static_cast<float>(stripThickness);
                    cell.Height = static_cast<float>(cellLength);
                }
                else
                {
                    cell.X = free.X + static_cast<float>(offset);
                    cell.Y = free.Y;
                    cell.Width = static_cast<float>(cellLength);
                    cell.Height = static_cast<float>(stripThickness);
                }

                out[rowIndices[k]] = cell;
                offset += cellLength;
            }

            // Shrink the free rect by the strip we just placed.
            if (horizontalStrip)
            {
                free.X += static_cast<float>(stripThickness);
                free.Width -= static_cast<float>(stripThickness);
            }
            else
            {
                free.Y += static_cast<float>(stripThickness);
                free.Height -= static_cast<float>(stripThickness);
            }
        }
    }
}
