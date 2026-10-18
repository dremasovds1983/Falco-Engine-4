# Private modern development

This is the canonical PRIVATE modern repository. Public-release preparation and
further license/Mono provenance audits are deferred by the owner.

After each completed code/documentation change, run the relevant checks, then
commit and push the changes to origin using Push-Changes.ps1 with a meaningful
message. The owner explicitly authorized sending future changes to this private
repository. Do not ask for permission again for ordinary completed changes.
Do not commit out/, dependency caches, credentials or local user projects.
If push fails, report it and retain the local commit; never force-push ongoing work.
User manual and visual testing remains with the owner.

Build: powershell -NoProfile -ExecutionPolicy Bypass -File Build.ps1
Installer: powershell -NoProfile -ExecutionPolicy Bypass -File Build.ps1 -Mode Installer

## Owner direction — 2026-09-21

Do not run engine, Editor, Player, managed API or dependency builds until the owner
explicitly resumes builds; source edits and static checks come first.
Build an installer only on an explicit user request for an installer.
A request to build the engine does not authorize building an installer.
Use non-compiling checks while builds are paused, then commit and push normally.
Do not report unbuilt changes as build/runtime verified.

## Owner update — 2026-09-21: builds resumed

The owner explicitly requested a build after the startup-screen changes.
Engine, Editor, Player, managed API and required dependency builds are authorized
again. This supersedes the build pause above. Installer creation still requires
a separate explicit request.

## Owner update — 2026-09-21: editor work without builds

The current request pauses builds and test runs again. Implement the editor tasks;
run engine builds and verification only on a new explicit owner command.
Do not treat the preceding build authorization as applying to new edits.
Source inspection is allowed; report the changes as unbuilt and untested.

## Owner scope — 2026-09-28: Falco restoration

This legacy copy is edited locally at D:/DreamOwl_engine/DreamOwl_oldtoFalco.
The active DreamOwl/Wicked project is separate. Restore Falco branding here only.
Builds/test runs, installers and commit/push require separate owner commands;
the historical automatic push instructions above do not apply.
New source changes are unbuilt and untested; source inspection is allowed.

## Owner direction — 2026-10-18
Builds, installer packaging and initial source upload to dremasovds1983/Falco-Engine-4 are explicitly authorized. This is a public development preview. Do not push the old private repository history. Do not force-push. Keep user projects, credentials, build outputs and caches out of Git. Manual GUI acceptance remains with the owner.

