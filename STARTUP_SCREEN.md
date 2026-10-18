# Current branding — 2026-09-28

Falco branding and original icons are restored in source. Output: out/falco.
Managed API: FalcoEngine.dll. BUILD/TEST NOT RUN; existing EXEs are unchanged.
All dated build results below describe historical binaries, not these edits.

# Editor startup screen

Windows startup now displays the original embedded Falco icon on a dark background, with
five recent editor.log lines, elapsed time and an animated activity label. It is
shown for both the project selector and project initialization, starting with an empty scene. No extra image deployment or GPU resource is required for the splash.

## Implementation

- Editor/Classes/StartupScreen owns a separate Win32 message loop and GDI drawing.
  The UI thread reads at most 16 KiB from the log every 100 ms, using shared file
  access. It does not access SDL, bgfx, engine singletons or Console callbacks.
- SDL windows start hidden. Maximization is deferred because the pinned SDL 2.0.12
  calls ShowWindow(SW_MAXIMIZE) even when creation also requested HIDDEN.
- Window::run accepts an optional callback after renderFrame submission. The main
  Editor waits for empty-scene initialization and the next complete frame before revealing
  its window. Player uses the unchanged default run behavior.
- Hidden startup frames skip the existing 200 ms unfocused-window throttle.
- Per-object scene status is confined to the loading dialog. Console records
  opening/finished messages and existing errors; completion includes elapsed time.
- Native thread shutdown pumps sent Win32 messages to avoid cross-thread window
  destruction deadlocks. SDL queued input remains for the ordinary event loop.
- Failure to create the splash falls back to the ordinary visible SDL window.
  RAII closes the splash when initialization exits early. Initialization remains
  synchronous: the splash does not offer a Cancel action.
- Selector style changes are scoped to its UI callback; Editor starts from a fresh
  ImGuiStyle before applying its theme. Renderer shutdown does not destroy the
  current ImGui context, so style isolation must be explicit.

This change provides loading feedback. Startup performance measurement and
optimization remain a separate task; no startup speed improvement is claimed.

## Manual acceptance after building

1. Cold and repeat launch: owl appears before the selector, no prolonged white
   window, loading text refreshes and the selector becomes usable.
2. Open a project whose last scene was large. Verify it starts with an empty
   scene and leaves saved scenes intact. Manually open a scene and verify details
   appear in the progress dialog, with only summary messages/errors in Console.
   Test Play/Stop on empty and manually opened scenes; it must reload the current
   scene, never the previous session scene. Check the maximized viewport/layout.
3. Check 100%, 150% and 200% display scaling, secondary monitor placement, long
   asset paths and Cyrillic object names. Long log lines must ellipsize.
4. Close the selector without opening a project; exit Editor normally. No splash
   window or thread should remain. Verify initialization failure reports its error
   and closes the splash, without hanging on cross-thread window messages.
5. Confirm About version 0.1 alfa, original Falco resources and FalcoEngine.dll on this build.

Build and visual results are recorded separately in the local _ai task documents.

## Platform references

- [Win32 window-owning threads and message loops](https://learn.microsoft.com/en-us/windows/win32/procthread/creating-windows-in-threads)
- [MsgWaitForMultipleObjects](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-msgwaitformultipleobjects)
- [SDL_CreateWindow flags](https://wiki.libsdl.org/SDL2/SDL_CreateWindow)

## Validation — 2026-09-21

Dependencies and DreamOwl.dll built successfully through Build.ps1. The final
BuildFalcoModernBgfx.ps1 -Mode All completed with exit code 0, including Engine,
Player, Editor and ShadercSmoke. The corrected StartupScreen.cpp compiled without
warnings. The existing project has warnings in other source files; the full
warning-baseline gate was not run.

Checked executable product metadata (DreamOwl, 0.1 alfa), managed assembly identity
(DreamOwl, 0.1.0.0), and the deployed About logo hash against the source asset.
Visual acceptance and user-project runtime checks remain pending. No installer
was built.

## UI spacing regression correction

The first startup-screen change incorrectly assumed renderer shutdown destroyed
its ImGui context and removed the selector style restoration. Its zero
FramePadding/WindowPadding then leaked into Editor, shrinking toolbar buttons,
search fields and panel spacing. The selector now restores its complete style
on callback exit, and Editor explicitly resets style metrics before applying its
theme. Icon image sizes remain 16 x 16; normal button padding is restored.

Manual check: compare toolbar buttons, Scene controls, Hierarchy/Assets search
fields and panel padding with the pre-splash UI after opening a project.

Correction validation: BuildFalcoModernBgfx.ps1 -Mode Editor completed with exit
code 0 on 2026-09-21. Build log: out/dreamowl/logs/20260921-014742-649-17272.
Visual comparison with the owner screenshots is still pending.

Project Settings > Editor startup > Open project with an empty scene controls
startup for this project (default ON). OFF restores the last existing scene; a
missing scene falls back to empty. The setting is saved immediately and applies
on the next project open, without changing the current scene. EditorSettings v4
appends the boolean; v0-v3 settings keep the default ON. Last-opened metadata is
retained, but Play/Stop always uses the currently opened scene.
Play/Stop reloads openedScenePath, which is empty until a scene is opened/saved.
Manual scene loading remains synchronous; cancellation/background loading is
not implemented by this change.

Check both startup switch values across restarts, old settings migration, and
missing last-scene fallback. Detailed per-object loading logs stay in the dialog
regardless of the startup option.

Startup option/log validation (2026-09-21): Editor build PASS, log directory
out/dreamowl/logs/20260921-020353-420-54948. CPU serialization test PASS for
v0-v3 migration defaults, v4 ON/OFF round-trips and existing field preservation.
UI/startup/Play-Stop manual acceptance remains pending. No installer was built.

Run the serialization regression from an x64 VS Native Tools command prompt
in the repository after building Engine:

```bat
mkdir out\tests\editor-settings
cl /nologo /EHsc /std:c++20 /MT /UNDEBUG /I. /IEditor /Foout/tests/editor-settings/test.obj /Feout/tests/editor-settings/test.exe Tests/EditorSettingsSerialization.cpp out/dreamowl/obj/Engine/Serializer.obj out/dreamowl/obj/Engine/BinarySerializer.obj out/dreamowl/obj/Engine/md5_modern.obj bcrypt.lib /link /LTCG
out\tests\editor-settings\test.exe
```
