# Local vendor source notices

Original notices from Engine-local code and the retained Dear ImGui fork are
provided in texts/. PROVENANCE.json maps each text to a local source file and
byte interval. Repeated identical notices are stored once. Sources include
ImGuizmo, Recast/Detour, FastLZ, xatlas (including its embedded notices), dr_wav,
dr_mp3/minimp3, dirent and Intel Masked Occlusion Culling. The complete Apache-2.0
text accompanies the Intel references; it does not relicense other components.
The retained Dear ImGui and meshoptimizer license texts are also included.

These are local source hashes, not invented upstream release/commit identities.
ImGui now has per-file baselines: thirteen files match official docking commit
1780579403f07158adef7ac2f1e15d7990e732df and eleven additions match bgfx commit
75fe88fe3afb048ac6f775d9991f67a406e1f4a5. See imgui-upstream-baselines.json.
Three further widget implementations have exact historical bgfx matches in that
record; the three stb_*.h files are single-include wrappers. SDL backend changes
are recorded against the official ImGui baseline. Per-widget license attribution
is retained as historical evidence. These optional widgets, imgui_user files and
three wrappers are absent from compiler read logs for the accepted modern
Engine/Editor/Player. They are excluded from clean-source preparation and active
notice source requirements; archived copies and their original notices remain
untouched. Their historical presence is not a blanket license grant for reuse.
Dirent's full MIT license is included from upstream commit
9e22c75b65c0f804eb58ffacc78442a89e68aff2, whose header matches the local code
apart from line endings and the recorded local changes; this does not prove the
original import commit. The original 2006-2012 header attribution and upstream
license's 2015 attribution are both retained. See dirent-provenance.json.
The modern bgfx renderer now uses a documented adaptation of pinned upstream
examples/common/imgui/imgui.cpp at 65551a7db19240b4d105f09e665190d196243d92,
retaining Daniel Collin's attribution and the supplied bgfx BSD-2-Clause terms.
The older local renderer remains a legacy-only source and is excluded from the
clean publication copy. Its uncertain provenance is not reassigned to Falco.

License alternatives are retained as received, including dr_libs' MIT No
Attribution alternative and minimp3's separate public-domain reference. Local
changes and the final publication composition require their own review.
