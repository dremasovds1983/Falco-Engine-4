#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace GX
{
    // Same 2-D Gaussian, clamped edges, accumulation order and quantization as
    // the original baker. Cache invariant weights instead of exp per neighbour.
    template<class Pixel, class Checkpoint>
    void bakeBlur(Pixel* lightmap, int size, float radius, float pi, Checkpoint rowDone)
    {
        std::vector<Pixel> data(size_t(size) * size);
        for (int row = 0; row < size; ++row)
        {
            for (int col = 0; col < size; ++col) data[row * size + col] = lightmap[row * size + col] * 255.0f;
            rowDone();
        }
        int rs = (int)std::ceil(radius * 2.57f);
        int width = rs * 2 + 1;
        std::vector<float> weights(width * width);
        Pixel wsum;
        for (int y = -rs; y <= rs; ++y)
            for (int x = -rs; x <= rs; ++x)
            {
                float dsq = float(x * x + y * y);
                float weight = std::exp(-dsq / (2 * radius * radius)) / (pi * 2 * radius * radius);
                weights[(y + rs) * width + x + rs] = weight;
                wsum += weight;
            }
        for (int row = 0; row < size; ++row)
        {
            for (int col = 0; col < size; ++col)
            {
                Pixel val;
                for (int y = -rs; y <= rs; ++y)
                    for (int x = -rs; x <= rs; ++x)
                    {
                        int ix = std::min(size - 1, std::max(0, col + x));
                        int iy = std::min(size - 1, std::max(0, row + y));
                        val += data[iy * size + ix] * weights[(y + rs) * width + x + rs];
                    }
                lightmap[row * size + col] = Pixel(std::round(val.r / wsum.r) / 255.0f,
                    std::round(val.g / wsum.g) / 255.0f, std::round(val.b / wsum.b) / 255.0f, 1.0f);
            }
            rowDone();
        }
    }
}
