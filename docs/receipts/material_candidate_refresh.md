# Material import after deleting a duplicate — 2026-09-14

## Defect and correction

The owner removed `E:/blender_plugin/gaz53/decal_moss.material`, but native import
of `cottages/cottage_i/decal_moss.material` still reported both paths. Disk
inspection confirmed that only the cottage source existed. The direct import
refreshed only the selected path; a previously indexed competitor remained
ambiguous when no DirectoryWatcher deletion batch had reconciled it.

`UMHSourceImporter::ImportMaterialFile` now revalidates every known candidate of
an ambiguous selected material with the existing transactional
`FMHProjectResourceIndex::UpsertPaths`, then resolves the key again. Missing
paths are removed from the index; real duplicate files remain blocked. The
operation does not trigger a full source-root scan or a bulk import. The normal
material importer and resource-identity rules are unchanged.

The correction is limited to direct material import and its existing admission
test. No resolver, watcher, schema, public API or diagnostic code was changed.
The source index's derived diagnostics are recomputed by the existing upsert.

## Regression evidence

Evidence directory: `E:/temp/MH_MaterialCandidateRefresh_20260914`.

`Mimir.V4.Material.NativeFileImport.Admission` first verifies that a real
duplicate is rejected and an unrelated unique material is importable. It then
deletes the competitor without delivering any watcher event, verifies that the
warm index still reports ambiguity, retries direct import, and requires a
successful material update, exactly one surviving candidate and no full scan.

- `RED_BUILD.log` compiled the extended test against the previous production
  implementation. `RedReport/index.json`: admission fails while recovery of a
  deleted MI passes. The failing retry reports exactly the stale two-candidate
  diagnostic seen by the owner; the index retains both paths.

- `STRICT_UAT.log`: `BuildPlugin -StrictIncludes` succeeded for Editor,
  Development and Shipping, 6m32s, with unity/PCH disabled.
- Both native material tests pass in the final strict-binary full run; the
  extended admission test is the RED-to-GREEN regression for this defect.
- `host_source_parity.json`: all 257 Source files match the working tree by
  SHA-256. `owner_source_state.json` records the surviving source hash and the
  missing competitor without modifying either location.

- `FullReport/index.json`: **185 success + 133 success with warnings = 318**,
  0 failed and 0 not-run test entries. Both RecipeShadowParity tests and the
  single-FBX regression pass. Fourteen entries retain conditional NOT RUN
  checks, listed in `conditional_checks.json`; this is not full RHI/PIE field
  acceptance.
- `PERF_BEFORE.txt` comes from the preceding installed material-import build's
  `E:/temp/MH_MaterialFile_20260914/FULL.log`; `PERF_AFTER.txt` comes from this
  final full run. `InstrumentationCounters` passes in both. Warm map load still
  uses one component and zero synchronous loads, registry lookups, identity
  admissions and waits. Targeted mesh reimport still has zero full scans,
  recipe recompiles and actor rebuild time, with one incremental path and one
  resource/actor notification. No performance improvement is claimed.

- `FORCE_UNITY.log`: guarded `-NoEngineChanges -ForceUnity
  -DisableAdaptiveUnity -NoPCH -NoSharedPCH` build succeeds, 17 actions / 114.78s.
- `RhiUnityReport/index.json`: both native material tests pass on the final
  force-unity binaries with D3D12 / RenderOffscreen (RTX 3070), 0 failed.
- `git diff --check` and `python tools/check_normative_docs.py` pass.

All tests ran on the executor-owned
`E:/temp/MH_MaterialCandidateRefresh_20260914/Automation/MimirCompositeV5S6.uproject`.
Interactive drag/drop in the owner project and owner content recovery were not
performed; native factory discovery/recovery and the stale-candidate case were
verified by automation. Engine, external sources and owner assets/settings were
not modified.

## Installation

Status: **installed and SHA-256 verified** after the owner confirmed the editor
was closed; the installer rechecked process state before copying.

Destination:
`D:/PersonalProjects/UE5/MimirHead_portfolio 5.7/Plugins/MimirComposite`.
Eight plugin files were replaced, and all **269** installation inputs match the
verified build. Engine BuildId matches (`47537391`). The exported package's
editor binaries were synchronized from the verified final build.

Backup:
`D:/PersonalProjects/UE5/MimirHead_portfolio 5.7/Saved/MimirPluginBackups/MaterialCandidateRefresh_20260914_175223`.
Before/after hashes: `E:/temp/MH_MaterialCandidateRefresh_20260914/INSTALL.json`.
At this installation stage, changes were local on `codex/import-single-fbx`.
The owner subsequently confirmed that the material imported successfully and
authorized integration of the full session. Final combined validation and
installation are recorded in `dependency_selection_scope.md`.
