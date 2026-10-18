#define FREEIMAGE_LIB
#include <FreeImage.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

// Verify the baker's 24-bit PNG path keeps dark texels next to bright edges exact.
int main()
{
    FreeImage_Initialise();
    const int size = 32, pitch = size * 3;
    std::vector<BYTE> pixels(size * pitch);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            for (int c = 0; c < 3; ++c)
                pixels[y * pitch + x * 3 + c] = x < y ? 0 : BYTE((x * 31 + y * 17 + c * 73) % 256);
    FIBITMAP* source = FreeImage_ConvertFromRawBits(pixels.data(), size, size, pitch, 24,
        0xFF0000, 0x00FF00, 0x0000FF, false);
    assert(source);
    FIMEMORY* stream = FreeImage_OpenMemory();
    assert(stream && FreeImage_SaveToMemory(FIF_PNG, source, stream, PNG_DEFAULT));
    assert(FreeImage_SeekMemory(stream, 0, SEEK_SET));
    FIBITMAP* decoded = FreeImage_LoadFromMemory(FIF_PNG, stream);
    assert(decoded && FreeImage_GetWidth(decoded) == size && FreeImage_GetHeight(decoded) == size);
    FIBITMAP* rgb = FreeImage_ConvertTo24Bits(decoded);
    assert(rgb);
    for (int y = 0; y < size; ++y)
        assert(std::memcmp(FreeImage_GetScanLine(source, y), FreeImage_GetScanLine(rgb, y), pitch) == 0);
    FreeImage_Unload(rgb);
    FreeImage_Unload(decoded);
    FreeImage_CloseMemory(stream);
    FreeImage_Unload(source);
    FreeImage_DeInitialise();
    std::cout << "PASS: lightmap PNG roundtrip preserves every RGB texel, including dark edges\n";
}
