# Pivot wind adaptation — 2026-09-10

Status: wind prototype installed in the portfolio on 2026-09-10 at the owner's
explicit request for a scene test. Shared parent material instances and
live-scene Copy All verification are still outstanding. This is not an
end-to-end acceptance receipt.

## Scope

Reference and format evidence:
[`dagor_pivot_wind_20260910.md`](../reference_notes/dagor_pivot_wind_20260910.md).
This slice reuses the artist's seven beech atlas pairs. It does not regenerate
the atlases or rewrite their authored DDS, FBX or material source files.

- Mesh import preserves authored UV sets and vertex colors. Color attributes
  use native UE FBX byte quantization and sRGB-to-linear MeshDescription storage,
  so the final render buffer retains the original mask bytes. Importer version 6
  admits rebuilding an equal-source mesh produced by the old importer.
- Pivot texture import enforces and validates uncompressed linear data,
  dimensions, format, channel identity, clamp, nearest filtering, one mip and
  no resizing or streaming. Setup shares this policy with the importer.
- `MF_MHPivotWind_v1` decodes four hierarchy levels, applies per-instance wind
  in world space and rotates the existing surface normal. Global settings are
  separate from the plant's response parameters.
- `AMHPivotWindController` uses one MPC instance per world. No actor Tick is
  required. Normal map reopen, component registration and world reinitialization
  republish settings; teardown avoids writing into a destroyed scene.
- `MHPivotWindSetup` creates five wind resources and can explicitly attach the
  function to the two existing vegetation masters. It checks all assets before
  use, rejects unrelated WPO graphs, waits for usable shader maps and saves only
  changed packages. Existing incompatible generated resources are preserved.

## Verification environment

- Engine: UE 5.7.4, `D:/PersonalProjects/UE5/UE_5.7`; Engine source unchanged.
- Host: `E:/temp/MH_CEI1_package_20260907/HostProject`.
- Evidence: `E:/temp/MH_PivotWind_20260910`.
- Portfolio masters and their dependencies were copied to the isolated host.
  No original portfolio master was edited during these checks.
- Strict compilation uses `-DisableUnity -NoPCH -NoSharedPCH
  -NoEngineChanges -MaxParallelActions=4 -NoUBA`.

`BUILD_FRESH_ALL.log` passed: 175 actions, all plugin sources freshly compiled,
650.46 seconds. `BUILD_FINAL.log` passed: nine actions including the final saved
graph validators and negative tests, 29.68 seconds. Rendering checks caught invalid synthetic FBX axis/color
layer metadata, overly strict float trigonometry assertions, and an immediate
material compilation before its new texture-reference cache was populated.
Corrections preserve the existing mesh admission contract.

- Synthetic D3D12/SM6 render: `MaterialFullMapReport/index.json`, 1/1 success.
  SMC and ISM silhouette displacement both `(-4.693, 0.656)` pixels; normal-color
  change both `(-4.193, 5.315, -12.453)` bytes. Disabled wind restores the exact
  original silhouette and encoded normal image.
- Real `bush_beech_small_b.mesh.fbx` and its original H:/CDK atlas pair:
  `FieldRhiReport/index.json`, 7/7 success, zero failed/not-run. The render test
  uses the MH mesh builder, a nonzero ISM component origin, rotated instances
  and nonuniform scale. SMC/ISM displacement both `(-5.632, -0.955)` pixels,
  coverage difference zero. Normal-color changes `(-7.138, -0.010, 2.628)` and
  `(-7.136, -0.008, 2.626)` bytes. This diagnostic material checks deformation
  and normals; it is not an artist acceptance of the final bark/leaf appearance.
- Copied portfolio masters: `SETUP.log`, seven changed packages saved, two
  masters compiled; `SETUP_REPEAT.log`, zero changed packages saved. Both exited
  cleanly. Missing optional ACL/Oodle assets in the isolated host produced
  startup warnings, not material compile failures.
- Final D3D12/SM6 `FinalRhiReport/index.json`: **9/9** success, zero failed/not-run.
  This includes the real small_b render, atlas platform bytes and new negative
  checks for disconnected function inputs, a wrong WPO output and disconnected
  normal rotation. Its displacement/normal metrics match FieldRhiReport above.
- Initial full NullRHI: 306/309 success. Two new tests incorrectly demanded
  initialized GPU buffers/platform texture builds under NullRHI; their source
  data checks remain active and GPU assertions remain in D3D12. A legacy receipt
  version assertion was stale in the compiled object despite corrected source.
  The local staging helper now updates timestamps after copying. The fresh full
  compilation and final incremental compilation above reject stale object reuse.
- Final full NullRHI `FinalFullReport/index.json`: **311/311**, 187 success +
  124 success with warnings, zero failed/not-run at the automation framework
  level; 68.20 seconds, graceful exit. Rendering-only bodies explicitly report
  NOT RUN under NullRHI (including atlas platform bytes and wind attachment/
  rendering checks), and existing external parity/cottage gates remain opt-in.
  The wind rendering/platform checks were executed separately in FinalRhiReport.

At this checkpoint the input-readiness questions were answered. The owner's
subsequent field test identified overly smooth leaf motion and authorized the
source-based wind revision described in
[`dagor_wind_motion_parity_20260911.md`](../reference_notes/dagor_wind_motion_parity_20260911.md).
The prototype verification above is historical and does not validate that revision.

The synchronous full-shader-map render gate uses
`-ini:Engine:[SystemSettings]:r.ShaderCompiler.JobCacheDDC=0`. UE 5.7's normal
per-shader DDC/ODSC mode deliberately does not produce that complete map at this
point. Early runs with that unsuitable test configuration reported missing maps
and crashed during shutdown; the render test now rejects it before creating the
fixture. Final reported rendered checks use the full-map mode and shut down
normally. This is an isolated process setting, not a portfolio or Engine change.
Default ODSC editor behavior remains part of the artist field check.

## Setup and field migration

Installation was initially deferred for the input-readiness review and then
explicitly requested by the owner for a scene test. The plugin and master setup
below are now installed; the targeted mesh/material reimports and placement of
the controller are the artist's next field steps. Input-source findings and the
missing parent layer are recorded in the reference note's follow-up.

### Portfolio installation evidence

- Installed plugin: `D:/PersonalProjects/UE5/MimirHead_portfolio 5.7/Plugins/MimirComposite`.
  All 261 files were SHA-256 verified against the tested staging inputs.
- Backup and manifest:
  `D:/PersonalProjects/UE5/MimirHead_portfolio 5.7/Saved/MimirPluginBackups/PivotWind_20260910_201225`.
  Includes the previous plugin and both original master packages.
- `E:/temp/MH_PivotWind_20260910/PORTFOLIO_SETUP.log`: setup validated,
  seven packages saved, two masters checked; exit code 0 and graceful shutdown.
  Project startup activated the existing MultiLobeSpec shader overlay and
  triggered shader recompilation. The log contains 601 warnings and zero
  errors; these are not claimed to be a warning-free project startup.
- Five generated wind assets exist in `/Game/MH/Wind`. Both masters changed;
  all 608 generated material-instance files retain their pre-install hashes.
- No source-file rewrite, mesh/material reimport, controller placement, map
  save, or manual `rendinst_simple` reparenting was performed by installation.

Run only with a closed editor for the target project. Use a rendering RHI;
`-NullRHI` is deliberately rejected:

```text
UnrealEditor-Cmd.exe <project.uproject> -run=MHPivotWindSetup
  -DestinationRoot=/Game/MH/Wind
  -MasterRoot=/Game/MimirHead/MasterMaterials
  -AllowCommandletRendering -RenderOffscreen -d3d12 -sm6 -unattended -nop4
  -ini:Engine:[SystemSettings]:r.ShaderCompiler.JobCacheDDC=0
```

Omit `-MasterRoot` to create resources only. The five resources are
`MPC_MHWind`, `MF_MHPivotWind_v1`, `T_MHPivotDefaultPos_v1`,
`T_MHPivotDefaultDir_v1` and `BP_MHWindController`.

Attaching the shared `rendinst_tree_colored` and
`rendinst_tree_colored_alpha_split` masters affects all their existing children.
It does not reparent or resave the material instances. MPC defaults keep wind
disabled until a controller publishes settings. Preserve the artist's manual
`rendinst_simple` overrides; bulk source-wins material reimport would erase them.

For the first field plant, `bush_beech_small_b`:

1. Reimport only its mesh through the managed mesh Reimport handler, to restore
   UV1 and color data lost by the old importer.
2. Select its bark and branch material instances and run **Reimport from MH
   Source** together. This checks and repairs their pivot texture dependencies.
   **Update Material from MH Source** can skip unchanged sources and is not a
   guaranteed settings repair. Reimport uses source values.
3. Place `BP_MHWindController` in the scene and adjust its ambient controls.

Do not use the existing unscoped `-run=MHImportSources` commandlet for this
pilot: that command imports the whole source inventory.

## Acceptance limits

Artist field acceptance remains pending. The analytic gust field and fine
flutter are MH adaptations; exact Dagor noise/weather parity is not claimed.
The initial native WPO bound is 200 cm and requires deliberate adjustment for
larger plants. Nanite, hardware ray tracing, full packaged-game cooking,
character interaction and an atlas generator are separate acceptance work.
