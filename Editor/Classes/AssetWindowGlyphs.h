#pragma once
#include <imgui.h>
#include <imgui_internal.h>

namespace GX
{
    inline void restoreAssetWindowLayout(const char* title, const char* previous)
    {
        if (ImGui::FindWindowByName(title) || ImGui::FindWindowSettings(ImHashStr(title))) return;
        if (const auto* old = ImGui::FindWindowSettings(ImHashStr(previous)))
        {
            if (old->DockId) ImGui::SetNextWindowDockID(old->DockId, ImGuiCond_Always);
            else
            {
                ImGui::SetNextWindowPos(ImVec2(old->Pos.x + old->ViewportPos.x, old->Pos.y + old->ViewportPos.y));
                ImGui::SetNextWindowSize(ImVec2(old->Size.x, old->Size.y));
            }
        }
    }

    inline void addAssetWindowGlyphs(ImFontAtlas* atlas, ImFont* font)
    {
        const int browser = atlas->AddCustomRectFontGlyph(font, 0xe000, 16, 16, 18.0f);
        const int tags = atlas->AddCustomRectFontGlyph(font, 0xe001, 16, 16, 18.0f);
        if (!atlas->Build()) return;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        atlas->GetTexDataAsAlpha8(&pixels, &width, &height);
        // Folder + magnifier, and two offset tags; native monochrome glyphs.
        const char* masks[2][16] = {
            {"................", ".#####..........", ".#...######.....", ".##########.....",
             ".#........#.....", ".#...####.#.....", ".#..#....##.....", ".#..#....##.....",
             ".#..#....##.....", ".#...####.......", ".#####...##.....", "..........##....",
             "...........##...", "............##..", "................", "................"},
            {"................", "....######......", "....#....##.....", "..######..##....",
             "..#....##..##...", "..#.##..##..##..", "..#.##...##..#..", "..#.......####..",
             "..##.......##...", "...##.....##....", "....##...##.....", ".....##.##......",
             "......###.......", ".......#........", "................", "................"}
        };
        const int ids[] = {browser, tags};
        for (int i = 0; i < 2; ++i)
        {
            const auto* rect = atlas->GetCustomRectByIndex(ids[i]);
            for (int y = 0; y < 16; ++y)
                for (int x = 0; x < 16; ++x)
                    pixels[(rect->Y + y) * width + rect->X + x] = masks[i][y][x] == '#' ? 255 : 0;
        }
    }
}
