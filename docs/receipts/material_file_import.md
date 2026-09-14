# Native material file import — 2026-09-14

Follow-up: stale duplicate candidates after external deletion are corrected in
[`material_candidate_refresh.md`](material_candidate_refresh.md).

## Behavior

`UMHMaterialFactory` registers `.material` for Unreal's native file import.
Drag-and-drop into Content Browser and **Import to Current Folder** discover
the factory automatically, including multiple selected files. The factory calls
`UMHSourceImporter::ImportMaterialFile`, which verifies the selected source and
uses the existing standalone `MHImportMaterialV4` pipeline. Missing MI assets
are recreated; existing MI assets are updated in place, without importing FBX.
The shared material pipeline resolves textures, applies the parent and
parameters under `FMaterialUpdateContext`, completes compilation, saves applied
receipts and updates the project resource index.

Like composite import, source identity determines the managed destination:
`/Game/MH/Generated/Materials/<logical_name>`. The Content Browser folder does
not relocate managed assets. Only selected materials and their referenced
textures are imported. An unchanged source can restore a deleted MI. A warm
source index is reused.

Files must have canonical lowercase names and be inside configured Source Root.
Outside-root files are rejected with instructions to place them in Source Root.
Duplicate logical names remain ambiguous across the whole root: selecting one
file does not override resource identity. The diagnostic lists candidate paths.
An unrelated ambiguous material does not block a unique selected material.
PIE, concurrent import, missing files and noncanonical names are rejected.

The owner's two `decal_moss.material` sources in `gaz53` and
`cottages/cottage_i` remain unchanged. Their existing ambiguity must be resolved
before that material can be imported. Other unique decal materials can be
imported independently.

## Verification

Evidence directory: `E:/temp/MH_MaterialFile_20260914`.

- Before implementation, `RED_BUILD.log` compiled the recovery test against the
  prior plugin. `RedReport/index.json` records one failed test: native AssetTools
  import returned **0 assets instead of 2**. This confirms the missing factory.
- `Mimir.V4.Material.NativeFileImport.RecoverDeletedMI` uses real
  `AssetTools.ImportAssetsAutomated` with no explicitly supplied factory. It
  checks discovery, multi-file import, canonical asset paths, parent/overrides,
  saved receipts, an unselected material remaining absent, deletion and
  restoration at the original path, source byte preservation and warm indexing.
- `Mimir.V4.Material.NativeFileImport.Admission` checks invalid selections,
  PIE, duplicate diagnostics with both paths and no mutation, and in-place import
  of a unique material despite unrelated ambiguity.

- `STRICT_UAT.log`: `BuildPlugin -StrictIncludes` succeeded for Editor,
  Development and Shipping, 6m39s. `GUARDED_STRICT.log`: final tests compiled
  with `-NoEngineChanges -DisableUnity -NoPCH -NoSharedPCH`, 4 actions / 25.07s.
- `GreenFinalReport/index.json`: both native material import tests pass.
- `FullReport/index.json`: **185 success + 133 success with warnings = 318**,
  0 failed and 0 not-run test entries. Both RecipeShadowParity tests, the
  single-FBX import test and the two new native material tests pass.
- Fourteen entries retain conditional checks marked NOT RUN; exact messages
  are in `conditional_checks.json`. Full NullRHI is not RHI/PIE field acceptance.
- `host_source_parity.json`: all 257 compiled Source files match the working
  tree by SHA-256.
- PerfTrace before: `PERF_BEFORE.log` / `PerfBeforeReport`, 1/1; after:
  `FULL.log` / `PERF_AFTER.txt`, `InstrumentationCounters` passes. Warm map load
  retains zero synchronous package loads, endpoint lookups, identity admissions
  and compilation waits, with one reused component. Targeted mesh reimport
  retains zero full scans, recipe recompiles and actor rebuild time, one
  incremental path and one notified resource/actor. Timings under different
  concurrent build load are evidence of execution, not a performance claim.

The first green attempt exposed two fixture errors: `UFactory::ImportObject`
marks the returned package dirty and calls `PostEditChange` after the factory
has saved it; `ObjectTools::DeleteSingleObject` only clears asset flags and
leaves the object alive until garbage collection. The final test asserts the
native dirty state and uses `DeleteObjectsUnchecked` for complete editor
deletion/cleanup before recovery. No production behavior was changed to hide
these assertions. `GREEN.log` / `GreenReport` preserve the initial failures.

All automation uses the executor-owned content-only host
`Automation/MimirCompositeV5S6.uproject`, with engine plugins disabled by default
and MimirComposite enabled. Native drag/drop and file-dialog UI interaction in
the owner project is not manually verified; automated tests use the standard
AssetTools factory-discovery path shared by those entry points.

- `FORCE_UNITY.log`: guarded `-ForceUnity -DisableAdaptiveUnity -NoPCH
  -NoSharedPCH` succeeds, 17 actions / 116.81s.
- `RhiUnityReport/index.json`: **2/2 pass**, 0 failed, using final force-unity
  binaries with D3D12 / RenderOffscreen on RTX 3070. No conditional NOT RUN
  checks in these two tests. This covers native factory discovery, deletion and
  recovery, plus admission, with the real renderer.
- `git diff --check` and `python tools/check_normative_docs.py` pass.

## Installation

Status: **installed and SHA-256 verified**. The owner explicitly confirmed the
portfolio editor was closed; process state was rechecked before copying.

Destination:
`D:/PersonalProjects/UE5/MimirHead_portfolio 5.7/Plugins/MimirComposite`.
Engine BuildId matches (`47537391`). Twelve plugin files were replaced/added;
all **269** installation inputs match the verified build by SHA-256. The prior
single-FBX import remains included. The exported `Package` Source and editor
binaries were also synchronized to the final verified host.

Backup:
`D:/PersonalProjects/UE5/MimirHead_portfolio 5.7/Saved/MimirPluginBackups/MaterialFile_20260914_173225`.
Before/after hashes: `E:/temp/MH_MaterialFile_20260914/INSTALL.json`.

Owner content, project settings and external `.material` sources were not
modified. No Engine or reference sources were edited. Actual owner MI recovery
and interactive UI acceptance were still pending at this installation stage.
The changes were then local on `codex/import-single-fbx`. The owner subsequently
confirmed MI recovery and authorized integration of the full session; see
`dependency_selection_scope.md` for the final combined validation and installation.
