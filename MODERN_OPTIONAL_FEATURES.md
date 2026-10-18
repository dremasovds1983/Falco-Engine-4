# Optional integrations in the modern Windows build

The base build uses pinned OpenAL Soft for audio. It does not require the
Creative OpenAL SDK, ALu or ALUT. Headers come from out/deps/w9/include/AL,
the import library and runtime DLL from the same pinned W9 package.

Steam integration is disabled in the base build (FALCO_WITH_STEAM is absent).
Steamworks SDK files and DLLs are not part of the base source/runtime package.
Existing guarded Steam API code is retained for private integrations. There is
no supported automatic Steam bootstrap or turnkey modern Steam build switch.

To maintain a private Steam-enabled variant, obtain the SDK through your own
Steamworks account and keep it outside the public candidate. The existing
code expects the private checkout's steam/ layout used by the legacy build.
Use a private property sheet to define FALCO_WITH_STEAM consistently for
Engine and the apps, provide SDK include paths and x64 import libraries
(steam_api64 and sdkencryptedappticket64 where used), and rebuild all targets.
Package the matching authorized runtime separately and test initialization,
callbacks and failure when Steam is unavailable. A preprocessor define alone
does not configure a working integration. This optional variant has not been
validated by the base-build cleanup.

Video playback is disabled in the modern base through the existing VideoClip
and VideoPlayer stubs. Removing unused FFmpeg linker entries does not restore
playback. Restoring it requires a separately scoped codec/backend and packaging
decision (PUB-120). Do not copy old FFmpeg libraries into the base package.

Legacy configurations retain their original SDK paths and library lists in the
private source archive. The final PUBLIC manifest must exclude those SDKs;
successful modern compilation alone is not publication approval.
