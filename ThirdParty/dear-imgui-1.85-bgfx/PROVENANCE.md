# Dear ImGui compatibility island

Byte-identical copy of the project's Bgfx/bgfx/3rdparty/dear-imgui.
Version in source: 1.85 WIP / IMGUI_VERSION_NUM 18419. This is the existing
bgfx fork, not an upgrade to upstream ImGui. Original source comments and
widget notices (including markdown's zlib terms) are retained verbatim.
LICENSE.txt was retrieved from https://raw.githubusercontent.com/ocornut/imgui/v1.85/LICENSE.txt.
The original import is recorded in IMPORT-MANIFEST.json. On 2026-09-14 thirteen
files (core, imstb headers, LICENSE and stdlib header) matched official ImGui
docking commit 1780579403f07158adef7ac2f1e15d7990e732df byte-for-byte. The stdlib
implementation differs only in its relative include. Eleven additional files
match Git blobs in bgfx commit 75fe88fe3afb048ac6f775d9991f67a406e1f4a5.
UPSTREAM-BASELINES.json records the exact paths, hashes and unmatched files.
These are composite per-file baselines, not a single historical import commit.
The three previously unmatched widget implementations now have exact historical
bgfx matches recorded in reconciledAdditions. The three stb_*.h files contain
only single include directives, not STB implementations. SDL backend changes
against the identified official baseline are recorded separately. Per-widget
license attribution remains recorded as a historical matter. All 17 optional
widget/user/wrapper files are unused by the accepted modern build (verified in
Engine/Editor/Player compiler read logs) and excluded from clean-source creation.
The active core uses the already identified upstream ImGui and imstb code.
