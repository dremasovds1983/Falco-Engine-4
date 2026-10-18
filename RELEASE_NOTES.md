# Falco Engine 4 — development preview

## Included changes

- Falco Engine 4 branding, managed FalcoEngine.dll API and Windows installer.
- HDR/exposure/tone mapping, Bloom, FXAA, deferred SSAO and SSR, environment lighting and animated clouds.
- Game window and menu icon; Scene diagnostics for deferred materials.
- Editable prefabs, camera orbit about the root origin, preview lighting and transform gizmo corrections.
- Terrain prefab rotation/scale and near-tree transforms; billboard orientation and diffuse lighting corrections.
- Mesh decal edge softness using a boundary BVH texture for large static meshes.
- Guard against recursive rendering on entering Play.

## Verification and limitations

Engine, Editor, Player and shader compilation passed on Windows x64/D3D11.
Compiler warnings remain. Manual installation, GUI and visual acceptance remain pending.
The source-built dependency restore path and the optional prebuilt support archive
are different workflows; the archive is a convenience snapshot of the Windows toolchain outputs.
It is not a guarantee of byte-identical output on another toolchain.

The owner previously confirmed basic SSR and exposure. Recent fixes still require
the checks below. Tree billboards use approximate lighting (not full crown normals,
all lights or matching shadow reception). Decal edge softness targets open boundaries
of static LOD0 meshes; skinned, closed and batched meshes are not covered. Scene
diagnostics do not provide every DreamOwl display mode.

## Manual verification checklist

1. Install into a chosen directory; launch Editor, open a copy of an existing project,
   enter/exit Game repeatedly and export/run a standalone Player.
2. On a static mesh decal compare Edge softness width 0, 1, 5 and 20 at fixed camera.
   The contour should fade gradually, not merely change opacity everywhere. Check FPS.
3. Open a prefab: camera orbit must not rotate the model or dirty the asset. Check
   Frame prefab, zoom, pan, gizmo alignment, transform editing, save and reopen.
4. Paint trees near and far, approach existing trees, save/reopen terrain. Check
   orientation, size and lighting through the mesh/billboard transition.
5. Open Window > Game (icon present) and each available Scene diagnostic mode.
6. Recheck Scene gizmos, shadow stripes, skybox reflections with SSR off/on,
   water reflections, SSAO, clouds, Bloom and FXAA.

## Distribution

Source is Apache-2.0; third-party components retain their own terms. See LICENSE,
LICENSE_SCOPE.md and ThirdParty/. Runtime packages include dependency notices.
Installers are unsigned unless explicitly signed by the distributor; no signing
certificate is included in the repository. This preview is not a stable release.
