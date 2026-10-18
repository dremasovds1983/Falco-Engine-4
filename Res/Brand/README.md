# Original Falco brand assets

Restored 2026-09-28 from the owner's legacy Falco source tree.
No new artwork was generated.

Source: D:/FalcoSource/FalcoEngine/_FalcoEngine_/FalcoEngine_bgfx_master_ChatGPT

- Falco.png: exact Res/Editor/Logo.png, used by About.
- Falco.ico: exact Res/Build/icon.ico, used by Editor/Player EXE resources,
  the Editor startup screen and exported game default file icon.
- PlayerLogo.png: original PNG bytes extracted from Player/LogoText.c.
- InstallerLogo.bmp and InstallerBanner.bmp: exact Logo.bmp and back_picture.bmp
  from Milestones/M1-20260910/Source/x64/Logo within the same legacy tree.

Update-BrandAssets.ps1 copies the originals without resizing/re-encoding and
regenerates the embedded Player PNG array. It does not build an application or
installer. Resource changes require a separately authorized build to appear in EXEs.
