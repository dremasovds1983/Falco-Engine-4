# Bake Lighting diagnostics

The first diagnostic pass adds a dedicated Windows progress window and a per-bake
text report. A full frame/timeline profiler remains a separate planned task.

## Use

Save the scene, then choose **Lighting > Bake lighting**. The progress window
appears before static geometry/UV preparation. It displays the current phase and
map, triangle or row counts, shaded texels, completed ray queries, elapsed time,
lightmap-buffer size, process private memory and time since the last work checkpoint.

The bar covers the **current operation**, not an estimated percentage of the
entire bake. Geometry preparation, allocation, image writing and publication have
unknown work totals and use an activity indicator. Its animation only means the
window is alive. If no work checkpoint arrives for five seconds, the window says
the operation may be busy or stalled; it does not diagnose a deadlock.

The Console prints the result and a report path under `BakeReports` next to
`Editor.exe`. Reports contain scene, default lightmap size, GI parameters,
per-light shadow settings, actual allocated map sizes, periodically flushed
counters and aggregate wall-clock seconds per phase. Detailed progress stays out
of the Console. Report creation failure is explicitly reported; baking can still
proceed with live diagnostics.

**Cancel** (or closing the progress window) requests cancellation at the next safe
checkpoint. There are checkpoints between maps, triangles, raster rows, ray
queries and filter rows. The button stays responsive on its own UI thread.
An individual ray query, native image operation, allocation or UV-generation call
cannot be interrupted inside that call. Cancellation therefore need not be immediate.

The engine calculation remains on its existing thread; editing is disabled while
baking. Native SDL messages are pumped at throttled checkpoints without running
editor callbacks. An opaque long-running engine operation can still stop the
main window from repainting; the separate progress window continues to respond.
This is not a background scene-editing implementation.

## Output and failure handling

New lossless PNGs are written into a unique `Lightmaps.baking-*` sibling directory.
Cancellation and pre-publication errors remove only this temporary output and
preserve the preceding lightmap files. Publication starts only after all writes
succeed, and cancellation is then disabled. The old directory is renamed to a
unique backup before switching in the complete new directory. A failed switch
attempts to restore the backup. A restore failure reports its retained location.
A locked backup may remain for manual cleanup after successful publication.
This is exception-safe replacement, not a crash-atomic filesystem transaction:
a process/power failure between renames can require restoring `Lightmaps.previous-*`.
Geometry preparation itself is not rolled back by cancellation.

Temporary pixel buffers, cached images and maps are released on success,
cancellation and exceptions. Save failures and allocation failures reach the
Console and report rather than silently producing an apparently successful bake.

## Changes affecting cost and correctness

- The Gaussian filter caches its invariant weights and normalization. It retains
  the original two-dimensional kernel, edge clamping, summation order and RGB
  quantization. With baked GI disabled, its all-zero indirect buffer needs no blur.
- Position/normal interpolation happens after rejecting already shaded or
  out-of-triangle texels.
- GI hits without an allocated lightmap are checked before dereferencing the map.
  Hit color/emission now come from the hit material instead of a possibly null
  source material; this correction can affect bounced lighting.
- Native window handle lookup initializes the SDL WM-info version and checks its
  return value, so the bake can safely disable/re-enable its owner window.

The existing filter still runs `resolution / 64` passes. Thus its work grows
roughly cubically with map resolution; caching weights does not eliminate that
scaling. Two primary float buffers cost 32 bytes per texel per map (8 MiB at 512,
128 MiB at 2048), plus temporary filtering/encoding buffers and scene resources.
All map buffers still coexist during GI. The report helps decide whether the next
optimization should target rays, filtering, geometry generation or memory.

## Automated checks (2026-09-21)

`Tests/BakeDiagnostics.cpp` checks exact RGBA agreement with the previous Gaussian
implementation for constant, random and edge-impulse inputs at several sizes,
repeated passes, row checkpoints, exception propagation, cancellation cleanup,
failed-publication rollback, first publication and replacement of old maps.

Run from an x64 Visual Studio developer command prompt, at repository root:

```bat
mkdir out\tests\bake
cl /nologo /std:c++17 /EHsc /O2 /MT /UNDEBUG /Foout/tests/bake/test.obj /Feout/tests/bake/test.exe Tests/BakeDiagnostics.cpp
out\tests\bake\test.exe
```

CPU checks passed. One local 128x128 single-pass microbenchmark measured about
15.9 ms for the original filter and 3.1 ms with cached weights. This is not an
end-to-end bake speedup or a timing guarantee.

The modern Release x64 Editor build passed; build log directory:
`out/dreamowl/logs/20260921-030014-735-44340`. No installer was built.

## Owner acceptance still required

1. Small saved scene: complete a bake; check progress, final Console result,
   generated report and rendered lighting, including a GI bounce between different materials.
2. Cancel once during lighting and once during filtering, with existing maps:
   verify their files remain unchanged, then successfully bake again.
3. On the reported scene, compare one setting at a time: lightmap resolution
   512/1024/2048, then shadow resolution and directional cascade count. Keep GI,
   geometry and other parameters fixed; retain each report. The owner's original
   report of “resolution 512, two cascades” did not establish which resolution
   control was changed or prove that cascades caused the slowdown.
4. Check progress-window text, counts, unknown-total animation and cancellation on
   the owner's display scale; check the Editor is enabled again after each result.

No full bake of the owner's project or visual acceptance was performed here.

## Static-batch UV correction (2026-09-21)

The owner's 4096-resolution report recorded 131 maps but only 131 shaded texels
and 116 ray queries. Lighting took 0.179 seconds; reload took 175.75 seconds out
of 214.84 seconds total. This was not evidence of a fast, correct bake.

The batch generator previously required the imported `useLightmapUVs` flag before
creating its atlas. The baker still allocated and shaded maps for batches without
that flag. Collapsed UV triangles were then treated as a valid vertex by the
barycentric fallback, allowing one texel per map to masquerade as a completed bake.

Static lighting batches now generate their own atlas when `buildUvs` is requested,
independently of imported secondary UV availability. Atlas input arrays use vertex
count (previously index count), generation errors are checked, and the generated
UV flag is retained. Before allocating maps the baker checks triangle indices,
UV ranges and usable UV area. A model without usable lightmap UVs now produces an
explicit error with its object/submesh name; non-batched models still require
Generate lightmap UVs and reimport when their secondary channel is absent.

The rasterizer skips collapsed UV triangles and evaluates barycentric coordinates
in double precision, replacing an absolute area cutoff that also rejected small
valid atlas triangles. A separate direct-light bug was removed: an occluded light
now contributes zero instead of subtracting the contribution of another light.
No shader lighting model changes are included in this correction. UV overlap
validation, bake/realtime shading parity, filtering seams and large-map memory
optimization remain follow-up checks after a valid rebake.

CPU regression tests now include collapsed UVs, small valid UV triangles, winding,
outside points and nonfinite/out-of-range coordinates. The owner's visual
confirmation of the new progress display is recorded; correct lighting output is
still awaiting a rebake on this correction.

Suggested controlled comparison on a saved test copy:

- Ambient RGB 32/32/32, alpha 255; both realtime GI and baked GI off.
- One enabled Directional Light, white RGB 255/255/255, intensity 1,
  Cast Shadows on; keep its rotation and the camera fixed. Disable other lights.
- Default lightmap size 512 initially (and check per-object overrides).
- Realtime directional shadows 2048, two cascades.
- Capture Realtime after Clear baked data; then switch the same light to Baked,
  bake again and capture the same view without changing ambient.

A valid bake will process far more texels than the defective one and may take
longer. The reported 131 maps at 4096 require about 65.5 GiB for the two primary
float buffers alone; begin with 512 to assess correctness before increasing size.

UV correction validation: CPU tests PASS; modern Engine/Editor build PASS, logs
`out/dreamowl/logs/20260921-033649-186-35040`. The first link attempt was blocked
by the running Editor; it succeeded after the owner closed the application.
The corrected scene bake and visual comparison remain owner checks.

## Direct-shadow acceleration and thin surfaces (2026-09-21)

The next owner run completed in 15934.78 seconds (4 h 25 min): Lighting took
15899.58 seconds, with 22061215 shaded texels and 11657588 shadow ray queries.
The maps are now populated, but the owner still sees large differences between
realtime and baked shadow coverage. The thin converted BSP/FBX wall is a relevant
case, not a confirmed explanation for every difference.

The generic raycaster used by direct baking collected/sorted scene hits and
linearly tested batch triangles for each ray. It also rejected back faces.
Direct bake visibility now builds an immutable world-space triangle BVH once and
stops at the first blocker. Both triangle sides block direct baked light,
including a single-sided wall. Rays start at the receiving surface towards the
light, skipping the first 0.001 world units for self-intersection. Point/spot
queries stop before the light; directional queries no longer have the arbitrary
1000-world-unit reach of the former synthetic light origin.

The snapshot includes active, lighting-static, shadow-casting non-batched meshes
and terrain, shadow-casting lighting-static batches and CSG shadow geometry.
Transient renderables remain excluded. Triangle geometry is opaque: alpha-cutout
foliage and transmissive shadows are not implemented by this change. Runtime
picking and GI bounce queries retain their previous code and semantics; the new
acceleration applies to direct baked shadows only. No shader parity change is
included. The report identifies the query mode and blocker triangle count.

CPU checks pass for both sides of a thin triangle, reversed winding, finite light
range, self/parallel/missed rays, a blocker beyond 1000 units, an empty scene,
1000 randomized rays against an independent plane/edge-test reference, and BVH
pruning (one triangle tested out of 8192 in the grid fixture). These are not an
end-to-end speed measurement on the owner's level. Editor build passed:
`out/dreamowl/logs/20260921-112935-502-37752`. No installer was built.

Owner check: rebake at the same 512 / GI-off / one-white-directional-light settings
and compare the same camera view and ambient. Retain the new report to compare
Lighting time and `shadow_triangles`. Thin-wall shadow coverage and overall
bake/realtime agreement remain pending visual acceptance.

## Lightmap storage and residual leaks (2026-09-21)

The owner confirmed a faster rebake: 42.884 s total / 11.352 s Lighting,
versus 15934.784 s total / 15899.579 s Lighting before the BVH, with the
same 22061215 texels, 11657588 rays and 131 maps. Large shadow coverage is
closer, but residual door/floor and thin-wall leaks remain under investigation.

New maps use GUID[_submesh].lightmap.png instead of JPEG. The dedicated suffix
selects uncompressed texture import, clamp addressing, no ordinary atlas mipmaps
and the full baked resolution, including when the Assets importer loads first.
This removes lossy storage and mip mixing as possible contributors, not every
source of leaks. The derived cache is invalidated after publishing a rebake.
Batch, CSG, mesh and export readers prefer the new maps with legacy JPG fallback.
Packaged players can resolve the new maps from their texture cache. Export and
visual acceptance still need an owner runtime check.

The current uncompressed texture path uses RGBA16 (8 bytes/texel), so GPU memory
grows relative to BC7. No mipmaps can also increase distant aliasing; chart-aware
padding/mips remain future work. Bilinear filtering, finite atlas resolution,
UV overlaps and ray self-intersection tolerance can still affect shadow edges.

Rebake at 512 with the same light, ambient, GI and camera before comparing. Old
JPEG maps do not benefit until regenerated. Inspector Shadow bias controls
realtime shadow maps, not the baker's 0.001-world-unit ray near distance.
Realtime CSM bias/filtering has not been changed by this storage correction.
DIRECTIONAL-CSM-ACNE-001 is reopened, not resolved.

Validation: Tests/LightmapStorage.cpp roundtrips a high-contrast 24-bit image
through FreeImage PNG and verifies every RGB texel; PASS. Compile with MSVC
/std:c++17 /EHsc /O2 /MT /UNDEBUG /Iout/deps/w3/include and link
out/deps/w3/lib/FreeImageLib.lib. Existing BakeDiagnostics CPU checks also PASS.

Engine/Editor build PASS (exit 0): out/dreamowl/logs/20260921-161306-808-24384.
No installer built. Visual and exported-player checks remain pending.

## Intersecting thin-wall contact experiment (2026-09-21)

The owner confirmed that the gate extends below the floor, so a visible gap
between the two surfaces is not the working explanation. The PNG rebake still
showed leaks. This change remains pending visual acceptance.

Directional shadow passes now rasterize both triangle sides. Mesh, static batch,
CSG and terrain render paths preserve that explicit two-sided shadow policy
after material render states are applied. Point/spot and ordinary surface
culling retain their previous behavior. More visible self-shadow acne or extra
shadow rasterization cost are possible; PCF and Inspector bias are unchanged.

Direct baked rays now use a double-precision affine point on the source
triangle, exclude that triangle from occlusion, and use only scale-dependent
double roundoff tolerance instead of a fixed 0.001 world-unit near gap. Nearby
triangles in the same mesh still block light. Point/spot rays calculate their
direction/range from the same precise point and stop at the light. GI rays,
lightmap sampling density and bilinear filtering are unchanged; remaining atlas
edge bleeding is not ruled out. Rebake is required to exercise this change.

Correction to initial investigation: the active Res/BuiltinResources shadow
helper already had symmetric PCF. The obsolete helper in BuiltinResources was
not evidence of asymmetric filtering in the owner's current Editor.

Validation: Tests/ShadowContact.cpp PASS (24 wall/floor cases, both windings,
translations, close blockers, finite light range, source self exclusion and
BVH reordering). Existing Tests/BakeDiagnostics.cpp PASS, including 1000 rays
against the independent reference. Both compiled with MSVC /std:c++17 /EHsc
/O2 /MT /UNDEBUG. Full Engine/Editor build PASS, exit 0:
out/dreamowl/logs/20260921-164754-495-34756. An earlier build exposed Win32
near/far macro collisions, fixed by naming the BVH bounds tMin/tMax. ShaderSmoke
PASS: out/dreamowl/logs/20260921-164506-199-39156. No installer built.

Owner check: launch out/dreamowl/bin/Editor/Editor.exe; compare Realtime with
the same gate camera, light, bias and shadow settings, then rebake at 512 with
GI off. Compare both the contact light strip and any new dark speckles/acne.
Do not close DIRECTIONAL-CSM-ACNE-001 until this comparison is accepted.

## Thin-wall experiment retired (2026-09-21)

Owner tests reproduced contact leaks on thin intersecting surfaces. Experimental
intersection splitting removed the bright seam in one small scene but introduced
dark points on the lit side and exceeded cut limits on a complex level. The owner
chose sufficiently thick geometry and deferred general thin-surface remediation.

Removed the experimental splitter, associated padding, xatlas cancellation patch,
colour diagnostics, footprint probes, shaders/UI and experiment-specific tests.
Retained 7375a2f lightmap storage/cache fixes and 6d27f41/04eeff8 shadow precision
and directional caster changes. Ordinary bake monitoring remains.

Rebake scenes previously using splitting after updating. Existing project assets
and baked data were not modified by this rollback. See
[geometry guidance](BAKED_LIGHTING_GUIDE.ru.md). The thin-wall limitation is not fixed.

Validation of rollback: ShadowContact, BakeDiagnostics and LightmapStorage PASS;
shader smoke PASS (20260921-202113-725-17688); Engine/Editor build and resource
installation PASS (20260921-201908-143-28216), with compiler warnings and no errors.
Source/shader/test/vendor files match 04eeff8 exactly; only documentation differs.
No installer built. Owner visual verification remains outstanding.
