# Single FBX import from MH Source Tool — 2026-09-14

Status: locally verified; installed in the owner project on explicit request. Branch
`codex/import-single-fbx`, base `992eff87a50b236ac4997b2a2b6c1bfc9e60b85b`.

## Behavior

`Tools > MH Source Tool > Import Single FBX...` selects exactly one canonical
`.mesh.fbx` already inside Source Root. `UMHSourceImporter::ImportStaticMeshFile`
updates that path in the index, verifies that the key uniquely resolves to the
selected file, and calls the existing `MHImportStaticMeshV4` with force enabled.
The existing batch completes compilation, saves packages and commits projection
and notifications before the UI reports success and selects the resulting mesh.

Only this mesh is imported, including its normal collision companion when
present. Materials must already have current applied receipts. The operation
does not execute the bulk import plan. Invalid names, outside-root paths,
missing files, duplicate keys, PIE and an active import are rejected. The index
may initialize with a full scan when recreated, as in existing targeted import;
a warm index uses only the selected path.

## Verification

Evidence directory: `E:/temp/MH_SingleFbx_20260914`.

- `STRICT_UAT.log`: `BuildPlugin -StrictIncludes` succeeded for Editor,
  Development and Shipping (7m48s).
- `FINAL_STRICT_VERIFIED.log`: final source, guarded `-NoEngineChanges`,
  non-unity/no-PCH/no-shared-PCH build succeeded, 9 actions (41.20s).
- `FORCE_UNITY.log`: `-ForceUnity -DisableAdaptiveUnity`, guarded build
  succeeded, 17 actions (110.41s). `UnityReport`: final single-file test passed.
- `FullFinalReport/index.json`: **185 success + 131 success with warnings =
  316**, 0 failed, 0 not-run test entries. Both RecipeShadowParity tests pass.
- Fourteen test entries contain conditional checks marked NOT RUN (rendering,
  external S6 parity, installed cottage fixture). Their exact names and messages
  are preserved in `conditional_checks.json`; this is not RHI/PIE acceptance.
- `package_source_parity.json`: all 255 packaged Source files match the working
  tree by SHA-256. The test host also matched all 255 files.
- `git diff --check` and `python tools/check_normative_docs.py` pass.

`Mimir.V4.StaticMesh.Importer.SingleFile` covers new import, forced reimport,
unchanged-source restoration of a local build-setting edit, leaving another
changed FBX's mesh and receipt untouched, one notification for one import,
zero warm-index full scans, and rejection of invalid selections and duplicates.

The first broad run used UAT's generic `HostProject`: 314 passed and two failed
because thumbnail tests require the `MimirCompositeV5S6` name and default engine
DataValidation checked a geometry-free persistence fixture. The final run used
a fresh content-only `Automation/MimirCompositeV5S6.uproject`, with the repository
test template's `DisableEnginePluginsByDefault=true` and only MimirComposite
enabled. Neither affected test nor production validation was changed.

An intermediate build incorrectly reported up-to-date after Copy-Item preserved
source timestamps older than the objects. The final strict build explicitly
recompiled both changed translation units; the final test name and uppercase
suffix assertion were present in the verified run. Pre-implementation runtime
RED was not recorded. Native file-picker interaction and owner field acceptance
remain unverified. Engine and reference sources were not edited. The `Package`
directory contains the updated plugin artifact.

## Installation

Owner explicitly requested installation after the verification report. Installed
into `D:/PersonalProjects/UE5/MimirHead_portfolio 5.7/Plugins/MimirComposite`
with the portfolio editor closed. Engine BuildId matched (`47537391`).
39 plugin files were replaced/added; all 267 installation inputs were SHA-256
verified against the built host afterward. Project assets and settings were not
part of the installation. `E:/temp/MH_SingleFbx_20260914/INSTALL.json` records
the before/after hashes. Backup:
`D:/PersonalProjects/UE5/MimirHead_portfolio 5.7/Saved/MimirPluginBackups/SingleFbx_20260914_162247`.
