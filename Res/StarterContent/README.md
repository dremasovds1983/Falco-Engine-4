# Falco Starter Content

This is the canonical source tree for redistribution-approved optional starter
assets shown by the Editor. It is installed beside `BuiltinResources`; it is not
part of the hidden engine runtime-resource tree.

Selected assets copied into this directory must have a matching entry in
`Manifests/starter-content.lock.json` before release.

```text
StarterContent/
  Materials/
    Surfaces/
    Terrain/
  Textures/
    Surfaces/
      PolyHaven/
        brick_wall_003/
        wood_planks/
        concrete/
        metal_grate_rusty/
    Terrain/
      PolyHaven/
        leafy_grass/
        dirt/
        rock_ground/
  Models/
    Prototype/
      Kenney/prototype_kit/
    Characters/
      Kenney/blocky_characters/
    FirstPerson/
      Wriks/wrad_arms/
    Weapons/
      Kenney/blaster_kit/
  Prefabs/
    Characters/
    FirstPerson/
    Weapons/
  Licenses/
  Manifests/
```

Original downloaded archives do not belong here. Keep them in
`deps-inbox/starter-content/<provider>/`; only the audited dependency-closed
runtime subset is copied into `Res/StarterContent`.
