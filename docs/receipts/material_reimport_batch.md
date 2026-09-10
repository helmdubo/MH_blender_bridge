# Material reimport batch — 2026-09-10

Status: implemented, built, regression-tested and installed. Portfolio field
acceptance remains with the owner.

## Field incident

The portfolio log showed `MH_E_AMBIGUOUS_RESOURCE_NAME` followed by pairs of
material package saves, approximately three seconds apart per material, without
advancing the editor frame counter (frame 178). The process was making progress
through different names, but the editor could not respond during the synchronous
loop. This evidence does not establish a deadlock or an infinite loop.

Two different menu actions existed:

- `Update Material from MH Source` already calls the scoped `ImportSources`
  coordinator once; unchanged sources are skipped.
- `Reimport from MH Source` called `ReimportMaterial` separately for every
  selected object. Each call gathered the source inventory, applied a probe and
  the real material, waited for compilation, saved the material twice, and
  refreshed the generated-asset projection.

The latter path explains the paired saves and repeated whole-project work. The
captured log contains 240 save entries for 120 consecutive distinct materials in
frame 178 over 513.702 seconds (median 3.194 seconds between distinct materials).
The SQL index participates in the cost; it is not evidence of an SQL deadlock.

An assistant-created material backup had also been placed under the configured
`SourceRoot=E:/blender_plugin`, introducing duplicate source names. The backup
was moved to `E:/MH_material_backups/cottage_i_material_backup_20260910_001936`;
359 files (358 originals plus report) were hash-verified. With owner approval,
397 donor files were moved to `E:/MH_material_backups/cottage_i_material_donor`
and hash-verified. A subsequent filesystem inventory found zero duplicate
case-insensitive `.material` basenames under SourceRoot. No further donor
replacements were performed.

## Change

`ReimportMaterials` and its headless coordinator share one source snapshot and
one import batch. The UI passes the entire selection once; the single-material
API delegates to the same implementation. Selection is deduplicated. Invalid
or ambiguous targets are reported individually and are not mutated. Explicit
reimport still applies source state when its source hash has not changed.

A cancellable progress dialog covers source analysis, material preparation and
commit. Cancellation stops admitting new materials; already prepared materials
are committed and reported separately from cancelled targets. It does not
interrupt a package save or roll back the completed prefix.

Material changes share a lazily created `FMaterialUpdateContext`, established
before parameter clearing or parent mutation and closed before the compilation
barrier. Strong references keep registered materials, including transient
round-trip probes, alive until the context closes. Unchanged parents are not
reset. Receipt updates only mark packages dirty; they no longer trigger another
material `PostEditChange`, and standalone material import saves once.

Full-source scanning remains once per explicit batch so a duplicate elsewhere
in SourceRoot cannot be overlooked. The batch compilation barrier still uses
`FinishAllCompilation`; this change does not promise bounded latency for
unrelated pending engine compilation or cold shader caches. Generated-asset
projection occurs once after save. Material imports continue to suppress
composite placement notifications.

## Verification

Evidence directory: `E:/temp/MH_MaterialReimport_20260910`.

- Initial material-context strict build: `CONTEXT_STRICT.log`, succeeded.
- Initial existing material tests: 18/18 NullRHI and 18/18 D3D12, zero failures
  and zero not-run tests (`ContextReport`, `ContextRhiReport`). These predate the
  final coordinator and strong-reference follow-up.
- New regression coverage: `Mimir.V4.Material.BulkForceReimport` — 200 selected
  materials and repeated force, invalid targets/cancellation, duplicate sources,
  and registered live mesh component with parent/two-sided/parameter changes.
- Final production strict build: `FINAL_STRICT.log`, 30 actions, succeeded;
  non-unity, no PCH/shared PCH, `-NoEngineChanges`. An earlier build retained
  stale UHT line macros and was stopped after compilation errors; explicit
  header regeneration fixed the isolated build cache.
- First full run: 301 passed, one failure in the new test's two package-reload
  parent assertions. Fixture parents were not saved. The fixture now persists
  its parent assets before importing dependent instances; the original parent
  equality assertions remain. `FIXTURE_STRICT.log`: 4 actions, succeeded.
- Final full NullRHI: **302/302**, 181 success + 121 success with warnings,
  zero failed/not-run in `FullFinalReport/index.json`, `FULL_FINAL.log`.
  Existing rendering-only bodies and absent cottage metrics are explicitly
  skipped in this lane; this is not complete rendered portfolio acceptance.
- Final D3D12 material lane: **22/22**, 14 success + 8 success with warnings,
  zero failed/not-run, `FinalRhiReport/index.json`, `FINAL_RHI.log`. The live
  scene test checks registered mesh render state and non-null SceneProxy both
  before and after source parent/two-sided/scalar changes. All four new tests
  passed, including both force passes over 200 materials. One scan, one wait,
  one save and one final projection per pass were asserted. Host startup also
  logged an Asset Registry discovery-cache backup error; test execution and
  graceful engine shutdown still completed. The isolated discovery cache was
  moved aside before the final full run.
- Both final processes exited normally with Automation SoftQuit and closed
  logs. Normative docs and diff checks pass.

## Installation

Installed in `D:/PersonalProjects/UE5/MimirHead_portfolio 5.7/Plugins/MimirComposite`
with the editor closed. BuildId `47537391` matches the previous installation.
All **249** staged/installed files were SHA-256 verified; all **240** source
files match the repository and the built isolated host.

Previous plugin backup:
`D:/PersonalProjects/UE5/MimirHead_portfolio 5.7/Saved/MimirPluginBackups/MimirComposite_20260910_005034`.

Installation manifest:
`D:/PersonalProjects/UE5/MimirHead_portfolio 5.7/Saved/MimirPluginBackups/installation_20260910_005034.json`.

No portfolio reimport or scene modification was launched by this fix. A cold
200-material portfolio timing remains to be measured in the owner's field test.
