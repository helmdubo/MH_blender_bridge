# Dagor leaf motion and ambient wind — revision 2

This revision replaces the MH sine flutter and analytic 2D gust approximation.
Upstream is pinned to DagorEngine `75723669297e48e200a0dc67b18c1629e0975daf`.
The implementation preserves the existing source atlas/UV import contract.

## Inputs traced before implementation

- [AssetViewer environment panel](https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/prog/tools/AssetViewer/av_environment.cpp): ambient controls are separate from plant response.
- `windSrv.cpp` maps azimuth to Dagor XZ `(cos(a),sin(a))`. Under the established
  mesh conversion this is UE XY `(cos(a),-sin(a))`.
- [ambientWind.cpp](https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/prog/gameLibs/render/wind/ambientWind.cpp): main strength alone uses `0.836 * Beaufort^1.5`.
  The control labelled Noise Speed (Beaufort) passes its numerical value directly
  to advection. Noise time wraps every 2000 seconds. Zero spatial scale produces
  zero inverse scale. A fallback direction texture quantizes each component as
  `clamp(floor((direction*.5+.5)*256),0,255)`, decoded with `2*byte/255-1`.
- [rendinst_opaque_inc.dshl](https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/prog/daNetGame/shaders/rendinst_opaque_inc.dshl): `tree_wind_params` holds time modulo 1000 seconds, branch amplitude,
  detail amplitude, and speed. `ApplyTreeWind` does not use its speed component.
- The supplied CDK `develop/application.blk` selects
  `develop/environments/global.blk`. That file sets branch/detail amplitude to
  **0.1/0.1**. These are the new controller defaults; the shader's standalone
  defaults are **0.5/0.5**. This is configuration evidence, not a capture of the
  owner's live AV shader globals.

## Equations retained

[wind_simulation_inc.dshl](https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/prog/gameLibs/render/shaders/wind/wind_simulation_inc.dshl)
samples a wrapping, linearly filtered 3D noise texture at
`((world.x - direction.x*noiseSpeed*rate*time)/scale, 0,
(world.z - direction.y*noiseSpeed*rate*time)/scale)` in Dagor metres. Texture R/B
are remapped to [-1,1]. They modulate the along-wind and perpendicular components;
the latter includes both noise strength and perpendicular multiplier.

[rendinst_vegetation.dshl](https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/prog/daNetGame/shaders/rendinst_vegetation.dshl)
multiplies the wind entering pivot deformation by **0.25**. Revision 1 omitted
this factor. Leaf detail is added separately after pivot motion, sampled at the
deformed world position; it must not receive that factor again. Colour-pass leaf
detail runs on alpha-tested foliage; opaque bark retains pivot motion.

[apply_tree_wind_inc.dshl](https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/prog/gameLibs/render/shaders/wind/apply_tree_wind_inc.dshl)
uses `vertexColor.rgb * wind_channel_strength.rgb`: R controls edge amplitude,
G contributes phase, B controls the additional vertical branch component. The
instance origin contributes object phase; the untransformed mesh position
contributes vertex phase. It combines the native smoothed triangle wave with
the four-frequency `frac`/quadratic wave, then redirects the displacement with
the sampled wind while retaining its pre-redirection length. MH evaluates this
in Dagor metres and converts the resulting offset to UE centimetres. Arbitrary
`sin(4*t)*sin(1.7*t)` flutter and `mh_leaf_flutter_cm` were removed.

The fractional-part operations are written as `x - floor(x)`. A numerical GPU
case with negative object phase exposed a different result from the initial
`frac` spelling in the generated Custom shader. Explicit floor restores the
upstream [0,1) periodic extension for those cases without changing the intended
equation; both sides of the time wrap are tested. This observation does not
establish a general compiler defect.

## Noise input and exactness boundary

[noiseTex.cpp](https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/prog/gameLibs/render/noiseTex.cpp)
prefers `perlin_voltex_bc7` or `perlin_voltex_dxt1`. Neither source texture was
found by a filename inventory of the supplied CDK `develop` tree. Its fallback
generator uses `coherentNoise.cpp`, 64³ voxels, seeds 10/101/1011, initial period 2,
six octaves, persistence .71, global multiplier .8, complete-volume per-channel
min/max normalization, and byte truncation. Although comments call byte zero
red, the BGRA upload makes shader R the seed-1011 field and B the seed-10 field.

An independent Windows/MSVC harness compiled the unchanged upstream Perlin
implementation with `/O2 /fp:precise`. The 1,048,576-byte BGRA oracle has SHA-256
`595E0CF126745EBC44549531F051BF360705C60A8AB2AB096F0AD22AF6537557`.
An extracted MH implementation compiled with `/fp:fast` plus its local precise
arithmetic pragmas produces the same bytes. MH uses a private Windows-CRT-compatible
PRNG without changing the application's global `rand` state.

The generated UE volume remains uncompressed and linear with wrapping 3D
interpolation and one mip. This reproduces the generator's pre-compression data.
Dagor's fallback normally BC1-compresses that data; a shipped BC7/BC1 asset could
also differ. Therefore matching formulas and oracle bytes are **not** proof of
pixel-identical noise in the owner's AssetViewer session.

## Upgrade and retained boundaries

The material-function asset keeps its original `MF_MHPivotWind_v1` path and
connector IDs; its implementation becomes revision 2. Explicit setup `-Upgrade`
admits only the intact generated legacy graph and upgrades it in place. Retained
parameter GUIDs, existing master surface connections, controller Blueprint
settings and material-instance overrides are preserved. A third MPC vector
`MH_WindTree` and `T_MHWindNoise_v2` are added. Existing controller placements
publish the new values after the updated class and upgraded assets are loaded.

Atlas generation, generic non-pivot vegetation, camera-distance fade, fluid wind,
character interaction, velocity-history parity, Nanite and hardware ray tracing
remain outside this bounded change. Missing/invalid pivot inputs keep the
existing no-deformation behavior. Global controller parameters are sanitized for
finite, nonnegative values. Noise speed and azimuth now follow Dagor semantics,
so an old controller's identical numbers can intentionally produce different
motion and direction after upgrading.

Build, numerical GPU checks and installation evidence belong in the revision-2
receipt. Field comparison with AssetViewer is a separate artist acceptance step.
