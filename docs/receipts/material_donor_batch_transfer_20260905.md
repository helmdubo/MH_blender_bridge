# Selected donor material transfer — 2026-09-05

Execution receipt; not owner/external-review acceptance.

Base: `ad6d206` (`origin/main`, including PR #101). Work branch:
`codex/material-donor-batch-export`. Implementation checkout:
`E:/GITHUB/Mimirhead_UE5Exporter/MH_blender_bridge_material_batch`.

## User-visible result

Content Browser → select polished `m_<name>` Material Instance assets →
**Transfer Donor Materials to MH Source...** → choose a folder inside MH Source
Root for new sources → review donor/file/target mappings → **Save and Update
Targets**.

Exactly one leading case-sensitive `m_` is removed. The remaining logical name
must be canonical. Existing unique `.material` files anywhere under Source Root
are overwritten at their existing locations; missing sources use the selected
folder. `/Game/MH/Generated/Materials/<name>` is updated in place. Existing mesh
material references therefore continue to refer to the same objects.

The review lists every create/overwrite before the first write. All admission
errors abort the preflight, including duplicate source names, two donors for one
target, donor/target overlap, future parent cycles and unsupported local state.
Parent dependencies determine apply order. Source-name mapping is checked again
after the review, and existing source content hashes are checked before each
write. Commit remains atomic per file, not per batch. Only successfully written
material keys are submitted to import. The scope explicitly reapplies targets
even when the written source hash is unchanged. Empty scope is never submitted.

There is no "save without applying" promise: the existing Source Root watcher
can also observe published files. Donor assets and their receipts are untouched.

## Format and implementation

Legacy `class/library` documents retain their previous grammar and canonical
bytes. The new exclusive `ue_instance` version 1 form carries the real parent
object path, scalar/vector/texture local overrides with complete parameter
identities, static switches/component masks with expression identities, and
the complete reflected UE 5.7 base override structure.

The adapter rejects unsupported parameter categories, material layer stack
overrides, terrain layer weights, atlas bindings and null/nonpersistent texture overrides. It resolves
parent/texture assets and rejects parent cycles before mutating a target.
Import uses the existing transient apply/extract proof. Full replacement uses
`FMaterialUpdateContext` and `FMaterialInstanceParameterUpdateContext`; legacy
apply now uses the same render-safe sequence as the clipboard path.

`AppliedParent=ue_instance:<path>` retains the new extraction mode across
registered/unregistered parent changes, reimport, local-modification detection
and Publish. The material proof path does not turn UE texture paths into source
resource keys. Details are documented in Source Protocol §5.1.

Main code: `Material/MHMaterialDonorTransfer`, `Material/MHUnrealMaterialDocument`,
mode integration in `MHMaterialProtocol`/`MHMaterialImporter`, guarded writer in
`MHMaterialDocumentExport`, scoped order/reapply in `MHSourceImporter`, and the
Content Browser action in `UI/MHSourceToolMenus`.

## Validation environment

- Installed UE **5.7.4**, CL **51494982**,
  `D:/PersonalProjects/UE5/UE_5.7`.
- Fresh isolated host:
  `E:/MimirComposite_MaterialBatch_20260905/MimirCompositeV5S6.uproject`.
  The plugin is a physical copy, not a junction to either checkout.
- Production portfolio project was not opened or edited. No installed-plugin
  update or merge was performed.
- Builds use Win64 Development, warnings as errors, non-unity, no PCH/shared PCH,
  `-NoEngineChanges`, `-NoHotReloadFromIDE`, `-NoUBA`, two compile actions.
- Initial complete build and incremental test build succeeded, but the first
  material run exposed an invalid mixed-ABI test binary: Copy-Item preserved
  source timestamps older than a previously built `.obj`, so UBT skipped a
  changed public structure. The first run crashed in the document writer.
  Host copies of every modified source/header were retimestamped and rebuilt
  together. That failed run is retained as `material-red-stale-abi.log`; it is
  not counted as validation of the final code.

## Validation results

| Check | Result | Local evidence under isolated host |
| --- | --- | --- |
| Current non-unity/no-PCH editor and test build | PASS | `build-consistent.log`, `build-final.log`, `build-test-cleanup.log` |
| Material suite, NullRHI | 26 success, 0 failed, 0 notRun | `MaterialFinalReport/index.json`, `material-final.log` |
| Full `Mimir.` suite, NullRHI | 204 reported success, 0 failed; three conditional lanes did not execute (below) | `FullFinalReport/index.json`, `full-final.log` |
| Legacy Python material suite | 32 passed | `python -m pytest tests/test_materials.py -q` |
| Normative docs | PASS | `python tools/check_normative_docs.py` |
| D3D12/SM5 material suite | 26 success, 0 failed, 0 notRun | `MaterialD3D12Report/index.json` |
| D3D12/SM6 material suite | 26 success, 0 failed, 0 notRun | `MaterialSM6Report/index.json`, `material-sm6.log` |
| BuildPlugin `-StrictIncludes` | PASS: Editor Development, UnrealGame Development and Shipping | `buildplugin-strict.log` |
| Final UI compatibility notice, same strict compile flags | PASS, 4 actions | `build-final-ui-strict.log`, `final-package-parity.json` |

The full suite logs `NOT RUN` for
`Mimir.Audit.MainBaseline.LoadedPlacementClickSelectsActor`,
`Mimir.Audit.MainBaseline.RenderedNativeHitProxy` (explicit RHI smoke flag
required), and `Mimir.V5.Composite.ISM.CottageMetrics` (real cottage absent).
These conditional lanes are not claimed as passed checks even though the
Automation JSON marks the test methods successful. No material test was skipped.
Both D3D12 runs used the NVIDIA GeForce RTX 3070; the SM6 run explicitly logged
`RHI D3D12 with Feature Level SM6 is supported and will be used`. No `NOT RUN`,
fatal error or unhandled exception appears in either material RHI log.

Validation also exposed and fixed numeric overflow conversion in the new reader
(`TryGetNumber` now fails instead of logging/coercing to zero), plus a missing
terrain-weight rejection. Eight new tests cover donor transfer and the UE codec.
The integration tests additionally exposed three stale claims left by existing
adoption fixtures: two material assets and `ue_s3_roundtrip`. Their owning tests
now clean up their own objects/packages; production receipt validation and test
success assertions were not weakened. Earlier failed reports are retained as
`MaterialConsistentReport` and `FullReport` for audit.

All 190 source/header/build files in the test host matched the implementation
checkout by SHA-256 before validation; copied timestamps were refreshed for each
later changed test translation unit so UBT could not reuse the obsolete ABI.

After the tests, the review dialog received one text-only compatibility notice
about the existing Blender reader. This final translation unit was rebuilt with
`-NoPCH -NoSharedPCH -DisableUnity -NoEngineChanges`. Its 31 manifest-listed
Editor build products were copied into the completed BuildPlugin package.
All 190 source files in both the final package and its retained strict build host
match the implementation checkout by SHA-256. No behavioral changes followed
the tests. Final package: `E:/MimirComposite_MaterialBatch_Strict_20260905`.

BuildPlugin required a temporary user UBT configuration to disable UBA and limit
compilation to two actions. Its exact original bytes were restored in `finally`;
`strict-config-restoration.json` records `Restored: true`, build exit code 0,
and identical before/after SHA-256:
`85382F0E3B2029D752E3CC42EF17F2C1727E1700D55D2451F050C474BA3772FB`.

To reproduce material automation in a UE 5.7.4 host containing this plugin,
run `UnrealEditor-Cmd.exe <host.uproject>` with
`-unattended -nop4 -nosplash -nosound -NoAssetRegistryCache`,
`-MHGoldenRoot=<checkout>/golden`,
`"-ExecCmds=Automation RunTests Mimir.V4.Material+Mimir.V5.Material"`,
`"-TestExit=Automation Test Queue Empty"`, `-ReportExportPath=<report-directory>`
and either `-nullrhi` or `-d3d12 -sm6 -RenderOffscreen`.
For the broader suite use the `Mimir.` prefix and the host project name
`MimirCompositeV5S6`; inspect the logs for conditional `NOT RUN` messages in
addition to the JSON totals. The strict package command is
`RunUAT.bat BuildPlugin -Plugin=<plugin.uplugin> -Package=<new-package-directory>
-TargetPlatforms=Win64 -StrictIncludes -NoDeleteHostProject`.

## Review boundaries

- `ue_instance` files are UE-only. Existing Blender readers reject them. Parent
  graphs and texture pixels are not embedded; referenced assets must remain
  available at the stored UE paths. The JSON hash covers instance state and
  dependency paths, not the contents of those ordinary UE assets.
- Source writes and generated-package saves are not one global transaction.
  Saved sources remain available if a target import fails. Existing importer
  behavior after late package-save failures has not been redesigned.
- Synthetic automation is not a field acceptance of the actual cottage scene
  or a reproduction of its historical D3D12 material crash. External review
  and a user-scene smoke test remain separate acceptance steps.
