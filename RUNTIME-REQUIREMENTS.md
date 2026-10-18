# Windows runtime requirements

This x64 Windows build requires Microsoft Visual C++ v14 Redistributable (x64).
For the verified MSVC 14.44.35207 build, the required runtime version is
14.44.35207.0 or newer. Mono and OpenAL use the external C++ runtime even though
much of the engine is linked statically.

Use the [official Microsoft download page](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist)
or its [x64 installer link](https://aka.ms/vc14/vc_redist.x64.exe).
Microsoft requires the Redistributable to be at least as recent as the build
tools used to compile the application. The modern setup includes the Microsoft
x64 runtime installer and installs it when needed. Raw source-build directories
rely on the runtime already installed on the development PC.

To check the installed prerequisites without starting Editor or Player, open
PowerShell in the application's directory and run:

    powershell -NoProfile -ExecutionPolicy Bypass -File .\TestRuntimePrerequisites.ps1

The execution-policy option applies to that process only. The check supports
Windows PowerShell 5.1 and PowerShell 7. It checks the pinned
Mono/OpenAL payload identities and the architecture/version of vcruntime140.dll,
vcruntime140_1.dll, msvcp140.dll and msvcp140_atomic_wait.dll in the application,
Mono/bin and system locations. A stale local copy is reported even if a newer
system runtime exists. Failure returns a nonzero process exit code. The script
does not install software or modify Windows. PASS is a prerequisite check, not
a GPU, scene or visual acceptance test.

If the payload hash check fails, restore the matching complete application
package. If runtime files are missing, old or of the wrong architecture, use
the supported Microsoft x64 Redistributable and check again. For source builds,
Build.ps1 records its Mono/OpenAL output hashes in the application
RuntimePrerequisites.json automatically. Rebuild with Build.ps1 after changing
dependencies; unknown or changed payload hashes are rejected.
