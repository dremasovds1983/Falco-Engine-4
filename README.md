# Falco

<p align="center"><img src="Res/Brand/Falco.png" width="160" alt="Falco logo"></p>

**A C++ 3D game engine and visual editor with C# scripting, built on bgfx.**

[Русская версия](README.ru.md)

Falco continues the development of Falco Engine with a modern Windows build workflow and the original Falco branding. The current development version is **4.0.0.0**.

The supported target is **Windows x64 with Direct3D 11**. The project includes an editor, a standalone player, scene and material tools, terrain editing, CSG geometry, physics, audio, and a Mono-based C# scripting API.

## Development status

This is the public source repository for **Falco Engine 4 development preview**: https://github.com/dremasovds1983/Falco-Engine-4. Existing license files, third-party notices, and original attribution are retained.

Windows x64 Engine, Editor, Player and shader compilation have passed. This is not a claim of full runtime or visual acceptance. See [RELEASE_NOTES.md](RELEASE_NOTES.md) for changes, known limitations and the manual verification checklist.

## Requirements

- Windows 10/11 x64.
- Visual Studio 2022 with **Desktop development with C++**, MSVC v143, and a Windows SDK.
- Git for Windows.
- Windows PowerShell 5.1 or PowerShell 7.
- Internet access for the first dependency restore.
- At least 10 GB of free disk space is recommended; actual usage may be higher.

## Build from source

Clone this repository into a short path, such as `D:\FalcoDev`, and run from the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Build.ps1
```

The script downloads pinned CMake and upstream dependency sources, verifies their SHA-256 hashes, and builds the dependencies, Engine, Editor, and Player. Mono is restored from a pinned upstream package. A large shared dependency archive is not required.

The first build takes longer than subsequent builds. Local dependency output hashes are recorded in `out/local-locks`; toolchain changes can trigger dependency rebuilds. Source versions and download hashes remain tracked in Git.

Expected output:

- Editor: `out/falco/bin/Editor/Editor.exe`
- Player: `out/falco/bin/Player/Player.exe`

For a full rebuild, add `-Rebuild`. Changes to the C# API are picked up by the W8b dependency step.

## Installer

Run only when an installer is explicitly requested:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Build.ps1 -Mode Installer
```

If the applications have already been built:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Build-Installer.ps1 -SkipBuild
```

Output: `out/installer/FalcoEngineSetup.exe`.

The installer uses portable [Inno Setup](https://jrsoftware.org/isinfo.php). Tool downloads are pinned in [InstallerTools.lock.json](InstallerTools.lock.json). The package includes Editor, Player, shared resources, Mono, and the Visual C++ x64 Runtime. The runtime is installed only when the required version is missing; Windows may request elevation for that prerequisite.

A new installation uses the current user's `LocalAppData/Programs/Falco Engine` directory. The modern AppId is retained. The directory selection page is always shown, including on upgrades; choose the existing directory explicitly if needed. Manual installation and visual checks remain with the owner.

## Compatibility and migration

- The managed API assembly is now **FalcoEngine.dll**, with assembly name `FalcoEngine`. When an old C# project is regenerated, the editor updates its reference and recompiles its scripts.
- Precompiled plugins referencing the previous assembly must be rebuilt against FalcoEngine.dll. Export standalone games again after migration.
- The C# namespace `FalcoEngine`, serialized file headers, and the existing project list directory are retained for compatibility.
- Build script filenames retain their existing names. New application outputs go to `out/falco`.
- An existing `out/falco-modern` directory contains earlier build artifacts. Do not copy its CMake/MSBuild cache into the new output directory.
- Historical capture paths in `warning-baseline.json` describe earlier evidence, not a new build.
- `Build.ps1` records locally rebuilt API hashes in `out/local-locks`; use those locks with the optional BuildSupport snapshot.

## Development workflow

After completing changes, perform the appropriate checks and commit/push through:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Push-Changes.ps1 -AllowPublic -Message "Describe the change"
```

The script verifies the configured repository and origin, creates a commit, and performs a normal push. Public upload requires the explicit flag above. It does not force-push. If a push fails, the local commit is retained.

Do not commit credentials, dependency caches, build outputs, or local game projects. See [AGENTS.md](AGENTS.md) for the owner-directed workflow. `.gitattributes` preserves exact file bytes without automatic LF/CRLF conversion.

## License and attribution

See [LICENSE](LICENSE) and the notices under [ThirdParty](ThirdParty). Falco is derived from Falco Engine; original copyright and dependency notices remain part of the repository.

The original Falco artwork and provenance are documented in [Res/Brand/README.md](Res/Brand/README.md).

## Baked lighting geometry

Use opaque walls with sufficient thickness. Thin or one-sided intersecting surfaces
are not reliably supported by the current lightmapper. No universal minimum thickness
is specified; verify both lit and shadowed sides at the intended scale and lightmap
density. Experimental intersection splitting and colour diagnostic views were removed.
See the [Russian authoring guide](BAKED_LIGHTING_GUIDE.ru.md).
