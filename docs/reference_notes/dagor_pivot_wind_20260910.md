# Dagor pivot wind -> MH: source audit and adaptation

Date: 2026-09-10. Upstream pinned to
[`75723669297e48e200a0dc67b18c1629e0975daf`](https://github.com/GaijinEntertainment/DagorEngine/tree/75723669297e48e200a0dc67b18c1629e0975daf).
This is an implementation/research note, not a change to the composite grammar.

## What the supplied files contain

The two beech proxymats are shared material definitions. Their `tex7` and `tex8`
references contain `$(ASSET_NAME)` and therefore expand to each mesh's own
`*_pivot_pos.dds` / `*_pivot_dir.dds`. Per-mesh material instances preserve these
different bindings; removing the suffix and consolidating all instances would
lose the required atlas assignment.

The Blender fields intentionally retain `$(ASSET_NAME)_pivot_pos.dds` and
`$(ASSET_NAME)_pivot_dir.dds`. A full path or a connected Blender shader is not
required. `addon/mh4blend/scene/export_material.py` specializes those bindings
for the exported mesh; the inspected `small_b.material` already contains the
correct concrete logical names. Physical copying is a separate **Copy All
Textures to Project** operation: `scene/project_textures.py` reads the proxymat
from `dagormat.proxy_path`, resolves relative DDS names beside it and copies
them into SourceRoot. It uses an existing physical file before consulting
loaded Blender images. Export itself validates the resulting logical references
inside SourceRoot, rather than implicitly copying the files.

This route requires an already resolved proxymat folder and an identifiable
asset name. Copy All currently derives macro names from a managed collection
stamp or the `.lodNN` / `.lods` conventions; a first-time plain unstamped
collection is a separate known limitation. No Blender exporter changes are
part of this wind slice.

### Input-readiness follow-up after the owner's questions

The initial GPU probe read DDS directly from the supplied H:/CDK directory. It
did **not** execute Copy All in the owner's open Blender scene. This distinction
is essential: a successful shader probe does not establish the export workflow.

Read-only verification on 2026-09-10 found all fourteen atlas files already in
`E:/blender_plugin/assets/gameproj/nature_common/entities/vegetation/ground_plants`.
All SHA-256 hashes match the originals. The three installed Blender 4.5 scripts
`scene/project_textures.py`, `scene/export_fbx.py` and `scene/export_material.py`
match the repository. Macro texture support predates this slice (commit
`0e8fccf`, 2026-08-31). This does not identify which operation originally copied
those files or prove the current open scene passes Copy All preflight.

The supplied bark proxymat still contains old absolute `D:/dagor2/...` paths for
tex0/tex2. Copy All can resolve these through exactly one loaded-image basename;
without that fallback, its all-slot preflight fails before copying the atlases
too. A missing mesh asset identity is another explicit preflight failure. The
actual open scene has not been inspected or modified during this slice.

The Blender FBX writer currently inherits the stock Blender 4.5 `colors_type=SRGB`
default, which exports color attributes. Four UV sets and a color set were found
in each supplied exported FBX. No Blender exporter change was made here.

The owner also clarified the desired material hierarchy:

```text
M_rendinst_tree_colored
  MI_bush_beech_bark                 shared proxymat parameters/textures
    MI_bush_beech_bark__small_a      tex7/tex8 overrides
    MI_bush_beech_bark__small_b      tex7/tex8 overrides
```

The branch proxymat needs the corresponding separate shared parent under its
alpha-split master. Current exports contain complete per-mesh `.material`
documents and current UE instances inherit directly from the master. They do
**not** implement the shared parent-MI layer yet. Comparing all seven bark
documents and all seven branch documents confirms each group is identical once
tex7/tex8 are removed. A future change must preserve proxymat identity in the
source/export contract and update common properties through the shared parent;
inferring parenthood by stripping a suffix from a UE asset name is insufficient.
No material-instance consolidation or reparenting has been performed.

The supplied textures are **hierarchy data**, not frames of a baked animation:

| Payload | Exact supplied format | Meaning |
|---|---|---|
| tex7 / pivot_pos | 32 x 64, one mip, legacy DDS FourCC 113, RGBA16F | XYZ = local pivot position in Dagor metres; A = numeric parent index |
| tex8 / pivot_dir | 32 x 64, one mip, uncompressed RGBA8 | RGB decoded from [0,1] to [-1,1] = branch axis; A x 20.48 = branch extent in metres |
| Mesh UV1 | Second authored UV set, distinct from surface UV0 | Pixel-centre address of that vertex's branch record |
| Vertex color | First authored color set | Fine wind response/phase in addition to branch movement |

The shader follows parent indices from the selected branch to the root (a
self-parented entry) and evaluates up to four levels. Local hierarchy data is
transformed for each placed instance. See
[`pivot_painter.dshl`](https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/prog/gameLibs/render/shaders/pivot_painter.dshl#L47)
and the vertex inputs/call in
[`rendinst_vegetation.dshl`](https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/prog/daNetGame/shaders/rendinst_vegetation.dshl#L140).

## How the atlases are generated

Gaijin documents **Dagor Pivot Painter Vegetation Tool** for 3ds Max + GrowFX:
select the GrowFX object, choose an output directory/name, then run creation.
It generates the two DDS files and a final mesh in a WIND layer. The hierarchy
comes from the authored plant structure. Grouping by path color can reduce
GrowFX levels to four. A Houdini version is also documented, with different
handling of deeper hierarchies. Both descriptions specify a 2,048-element limit.
See the [official tool guide](https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/_docs/source/dagor-tools/addons/3ds-max/dagor-maxscript-toolbox/pivot_painter_vegetation_tool.md).

The checked public repository tree contains the runtime decoder and tool guide;
the actual MaxScript/Houdini generator implementation was not found. Therefore
the precise DCC operation that created these particular seven bushes is unknown.
Reusing the existing atlas/UV pairs does not require regenerating them.

The official workflow requires Max 2015+ and plugin 1.4+; its downloadable
example uses Max 2021+ and GrowFX 2.0.1+. In Max, **Group by Path Color** groups
the hierarchy using at most four authored path colours. For deeper hierarchies,
the guide says the Houdini graph merges levels beyond four into the last level,
whereas the Max script discards them. It does not publish the Houdini graph's
node network or the Max script's algorithm for extracting pivots from GrowFX.

The compatible encoding contract can nevertheless be read from the runtime:
number the plant elements, store each local pivot and parent index in `pivot_pos`,
store its remapped axis and normalized extent in `pivot_dir`, and give all
vertices of that element the corresponding pixel-centre UV1 address. A root
refers to itself. The shader follows this chain and computes motion from the
current wind and time. This describes the required output, not a recovered DCC
generator. The vertex-colour leaf response and the separate ambient-noise volume
are additional inputs; neither is a sequence of frames stored in these DDS files.

## Independent checks on the seven source FBX/atlas pairs

Read-only inputs: `E:/blender_plugin/cottages/cottage_i/bush_beech_*.mesh.fbx`
and the user-specified CDK `ground_plants` directory. All seven FBX files have
four UV sets and one vertex-color set. UV1 becomes the correct DDS row after
the existing FBX-to-UE `1-V` conversion. Every referenced address is a pixel
centre; every parent chain is valid and terminates within four entries.

| Mesh variant | Distinct pivot records referenced | Hierarchy depths 1 / 2 / 3 / 4 |
|---|---:|---|
| medium_a | 165 | 1 / 42 / 84 / 38 |
| medium_b | 136 | 1 / 40 / 71 / 24 |
| medium_c | 143 | 1 / 39 / 77 / 26 |
| small_a | 80 | 1 / 27 / 39 / 13 |
| small_b | 89 | 1 / 32 / 38 / 18 |
| small_c | 124 | 1 / 35 / 63 / 25 |
| small_d | 100 | 1 / 30 / 53 / 16 |

Exact SHA-256 values, UV counts and chain checks are recorded locally in
`E:/temp/MH_PivotWind_20260910/atlas_fbx_audit.json`.
Original DDS and source FBX files were not rewritten.

## Global wind versus material response

The user's Asset Viewer screenshot shows the ambient-wind controls. These
belong to the environment: enable, direction, Beaufort strength, noise strength,
noise speed, spatial scale and perpendicular contribution. Material parameters
instead describe how this plant responds (rotation limits, damping, hierarchy
level multipliers and parent contribution).

Sources: [`ambientWind.cpp`](https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/prog/gameLibs/render/wind/ambientWind.cpp#L29)
and [`wind_simulation_inc.dshl`](https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/prog/gameLibs/render/shaders/wind/wind_simulation_inc.dshl#L71).

MH adaptation keeps this separation:

- `BP_MHWindController` / `AMHPivotWindController` publishes per-world MPC values.
  Changing the controller updates all participating plants without rebuilding
  their material instances. There is no actor Tick; shader time advances wind.
- `MF_MHPivotWind_v1` is shared by both vegetation masters and reads the existing
  `tex7`, `tex8`, `is_pivoted` and plant-response parameters.
- Generated material code is embedded in the function asset, so runtime/cooked
  materials do not read source files from disk.
- Wind defaults to disabled without a controller. `is_pivoted=0` gives zero WPO
  and identity normal rotation. Materials/meshes without valid pivot bindings
  are not automatically converted into pivot vegetation.

## Import defects addressed by the implementation

Before this slice, `FMHSceneTriangle` contained only UV0; the FBX translator
selected only the first set; both mesh builders allocated one UV channel.
The FBX files retained the data, but generated UE meshes lost it. The new
transport preserves authored UV sets in source order plus the first color set.
Importer version 6 makes equal-source meshes eligible for a rebuild.

The previous texture policy covered only `*_tex_n`. In UE's DDS translator,
the half-float position format is preserved, but ordinary RGBA8 DDS defaults
to sRGB. Pivot atlases now have explicit data-texture settings and receipt
checks, including repair of an existing import with incompatible settings.
`tex7` retains numeric alpha values greater than one; `tex8` uses uncompressed
linear RGBA8. Both use nearest lookup, clamp, one mip and no streaming.

## Scope and limits of the first adaptation

This section describes the initial prototype. The subsequent source-based
motion revision is recorded in
[`dagor_wind_motion_parity_20260911.md`](dagor_wind_motion_parity_20260911.md).

The atlas decoder and hierarchy are compatible with the supplied Dagor data.
The wind field and fine flutter are MH implementations, not a promise of
pixel-for-pixel agreement with Dagor's 3D noise, weather system or interactions.
The same parameters describe similar controls, but artist tuning is necessary.
In particular, the checked Dagor C++ passes the nominal noise-speed setting
through directly; MH consistently converts its Beaufort-labelled speed to m/s.

The pivot-to-mesh conversion for these imported assets is
`100 * (Dagor.x, -Dagor.z, Dagor.y)`. This does not automatically cover arbitrary
subsequent mesh-space baking, pivot changes or topology edits in Blender; those
operations must keep the atlas and its addressing consistent. Reordering
vertices alone is harmless because binding is stored per corner in UV1.

The attached masters receive a native UE maximum WPO displacement of at least
200 cm. It expands culling bounds and clamps displacement; adjust it deliberately
for vegetation larger than this bush pilot. Raster/ISM checks do not constitute
hardware-ray-tracing, Nanite or complete game-cook acceptance. Character
interaction, fluid wind, burning, legacy non-pivot vegetation and a new atlas
generator are separate extensions.

Build, runtime checks and installation status are tracked in the slice receipt.
