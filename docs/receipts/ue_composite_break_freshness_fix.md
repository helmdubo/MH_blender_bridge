# UE preview patch correction — Build / deferred invalidation / Break

Status: READY FOR OWNER REVIEW — local gates PASS. Branch:
`codex/fix-ue-composite-preview`. Commit and push explicitly requested by owner
after validation. No merge or installation into the owner project performed.

## Ownership and scope

- Accepted S5 comparison base: `8cb1a54` (PR #27).
- GitHub confirms S6 PR #28 is already merged as `00776c6`; current
  `origin/main` is `7b46df5`. Treating S6 as an unmerged candidate is outdated.
- The preview cache and its dependency-epoch getter were introduced in the
  **unmerged preview patch `21624a7`**, not in the S5 base. This correction
  belongs to that patch; it is not V5-S6.1 or a new numbered slice.
- Break remains a read-only, fail-closed consumer. An exploratory uncommitted
  Break readmission implementation was discarded after owner clarification.
- No protocol, Seed, resolver, golden, Blender production, Engine, or owner
  project changes are in scope.

## Comparison method

The same added Automation source is compiled against S5 and the pre-fix patch.
`NestedBuildSurvivesDeferredRegistryUpdate` performs the real `BuildComposite`
operation on a nested random composite plus a StaticMeshActor, fixes placement
Seed to 100, broadcasts the actual Asset Registry `OnAssetUpdatedOnDisk`
delegate, drains an editor ticker, and calls the unchanged Break operation.
It checks world X=1125/1500, selected resources, signature stability within each
run, and no mutation if Break is invoked during the expired-lease interval.
Unique resource names differ between runs, so cross-run signatures are not
compared as if the graphs had identical identities.

The fixture pauses the independent startup/watcher importer using its existing
test-only lifecycle gate and restores it after cleanup. The first exploratory
red run lacked this isolation: a startup full import unexpectedly rebuilt the
actor and emitted unrelated historical host-receipt errors. That contaminated
run is **not** evidence of the cache regression or a successful fix.

The owner log contains the persistent generic Break rejection but no internal
invalidation trace. The earlier `break_after_build_diagnosis.md` was a code/log
analysis, **not** a captured internal event trace. The injected AR test isolates
the failing callback path; it does not prove which Engine callback fired in the
owner's original session. The DirectoryWatcher/publication-order hypothesis is
not established by that log.

## Prior diagnostic environment incident

During the earlier mall-corpus probe (before this correction), Blender
`--factory-startup` / `read_factory_settings` without isolated user resources
removed the shared extension `xxhash` cache and reported a locked `.pyd` during
cleanup. The **same xxhash 4.0.1** was restored offline from the wheel already
bundled with the installed mh4blend extension. Import and hash computation with
Blender's Python succeeded. A final probe with isolated `BLENDER_USER_RESOURCES`
exited cleanly. User settings were not intentionally saved; Engine files were
not involved. No Blender process is used for this UE-only correction.

## Results

### Ownership experiment (before production changes)

The comparison source SHA256 was
`968AF36FFF207DC85F81D6EBC8F4D211029767146BB61920264AD96AA83B81A8`.
S5 worktree: `E:/MimirComposite_Break_S5Source_20260828`, with only the added
test source; host: `E:/MimirComposite_Break_S5Host_20260828`.
Pre-fix host: `E:/MimirComposite_UEFix_GateHost_20260828`.

| Production tree | After Build/Seed | After AR event | After tick | Break |
|---|---|---|---|---|
| S5 `8cb1a54` | current | current | current | PASS |
| Preview patch `21624a7` | current | expired | **expired** | FAIL, reported generic diagnostic |
| Corrected preview patch | current | expired | **current** | PASS |

Logs: S5 `probe-isolated.log`; pre-fix `break-red-isolated.log`; corrected
`break-fix.log`. Report folders carry the same names. Pre-fix all three new
regressions fail; corrected all three pass. The batch regression fails before
the fix for both same-root placements and parent-before-child selection, and
passes for both afterwards. Break itself is byte-for-byte unchanged.

Captured corrected trace, shortened to relevant fields (not owner-session data):

```text
12:25:03.626 Notify composite:fresh_built_* actors=0
12:25:03.832 Notify composite:fresh_built_* actors=0
12:25:03.833 New placement begins preview
12:25:03.888 Registry event; invalidate serial=5547
12:25:03.889 Deferred refresh, seed=100, attempt=5547
12:25:03.908 Preview end: available=1 current=1 error=<empty>
```

Both synchronous publication/import notifications precede the new placement in
this reproduction. There is no demonstrated missing-dependency publication
prefix here; adding a new publish-batch mechanism would not fix the absent AR
refresh path. The actual defect is the cache's invalidation-without-refresh.

### Implementation

- Immediately revoke stale cache leases, then coalesce refresh into one editor
  ticker. Resolve applied assets only; no source scan/import or actor `Modify`.
- Track the event serial at the **start** of each admission attempt separately
  from the successful plan lease. Failed admission does not create a fresh plan
  and does not cause per-frame retries. New dependency events can retry it.
- Do not repopulate shared cache if relevant claims changed during admission.
  A successful old candidate cannot acquire a newer lease.
- Pending meshes use the existing completion path; active Edit, game worlds,
  templates and closing worlds are excluded. Shutdown cancels both tickers.
- Gather weak actor references before rebuilding, outside `TObjectIterator`.
- Explicit multi-actor Rebuild invalidates all root keys **before** rebuilding
  any actor, then uses `RebuildComposite(false)` for each.
- Optional `Verbose` categories `LogMHCompositePreview`, `LogMHCompositeActor`
  and `LogMHCompositePlacementEvents` expose invalidation/attempt/notify traces.
  They are disabled at normal log verbosity and do not add ordinary log spam.

Independent read-only subagent review found no blockers. It recommended a
quiet-tick regression after failed admission; those assertions were added after
the initial full-suite run. They do not change the S5 comparison scenario.

### Validation

- Initial full corrected UE suite: **114/114**, 89 Success + 25
  SuccessWithWarnings, zero Fail (`Reports/break-full/index.json`).
- Pure Python: **229 passed, 11 skipped**. Nine skips are bpy-hosted modules;
  two are Windows symlink privilege restrictions, not algorithm failures.
- Blender-hosted: **NOT RUN** in this UE-only correction. No Blender process or
  user environment was touched in this correction; do not reuse old counters
  as if they were new gates.
- Final force-unity with adaptive unity disabled and no PCH/shared PCH: PASS.
  `break-fix-build.log`: production delta, 9 actions / 50.47 s.
  `break-final-force-unity.log`: last test delta, 4 actions / 35.03 s.
  Guarded host builds use `-NoEngineChanges -NoUBA`.
- Final `BuildPlugin -StrictIncludes -NoPCH -NoSharedPCH -DisableUnity`:
  PASS, Editor 91 actions, Game Development 10, Game Shipping 10, UAT exit 0,
  2m56s (`break-final-strict.log`). Stock UE 5.7.4 CL 51494982.
- One release-build attempt was rejected by UBT's global mutex because the
  force-unity build was still running. No compile failure/SDK defect:
  `break-strict-conflicting-instance.log` records `ConflictingInstance`.
  It was rerun serially after the other build finished; the final result above
  supersedes that attempt. The earlier strict package predating the quiet-tick
  assertion is not the delivered artifact.
- Final binaries replayed in **another host**,
  `E:/MimirComposite_BreakFix_ReleaseHost_20260828`, whose plugin junction points
  to the packaged plugin, not the repository. Its unchanged game-test harness
  binaries were reused after SHA256 verification of its source against the
  checked-in harness template. Plugin binaries come from the final strict run.
- ReleaseHost full Automation: **114/114**, 84 Success + 30
  SuccessWithWarnings, 0 Fail, 0 NotRun (`Reports/full/index.json`, `full.log`).
  This includes the final quiet-tick assertions and existing no-mutation tests.
  Frozen RNG/signature parity and the Automation, Editor-preview and real PIE
  parity tests also pass; no new packaged-game/cook claim is made for this
  editor-only correction.
- ReleaseHost RHI (D3D12, no null RHI): **3/3**, 1 Success + 2
  SuccessWithWarnings, 0 Fail (`Reports/rhi/index.json`). Native hit-proxy
  verification reports `HActor` on four passes, G off/on before and after
  rebuild/move/seed; viewport 479x339. Mesh-pending/completion is also tested.
- ReleaseHost ordinary `QUIT_EDITOR`: PASS, observed process exit **0**,
  `normal-editor-exit-verified.log` ends with D3D12 shutdown / `LogExit: Exiting`.
  This is not the forced Automation `TestExit` path.
- UAT generated its stock comment-only `Config/FilterPlugin.ini` in the repo
  plugin while packaging. The generated placeholder was identified against
  `BuildPluginCommand.Automation.cs:96` and removed from the working tree;
  it is not a production change. No Engine source was edited.
- `reference/`, `golden/`, `addon/`, `tools/`, protocol docs and QUESTIONS have
  no changes. `git diff --check` passes. Break's function body is unchanged.
- No owner-scene/large-mall field acceptance is claimed. The Build regression
  uses managed synthetic nested/random definitions and in-memory mesh assets,
  with the frozen axis FBX copied read-only into an isolated source root.

### Delivered artifact

- Plugin directory: `E:/MimirComposite_BreakFix_Release_20260828`.
- ZIP: `E:/MimirComposite_BreakFix_Release_20260828.zip`, **45,033,988 bytes**.
- SHA256: `855DFADB95148ACA2F8ED12A93C75618F80D33FD93BF38C6B5F0CE96F22338E8`.
- All **131 Source files** match the working tree by SHA256. All **213 ZIP
  files** were read back and SHA256-compared against the final package.
- Final test source SHA256:
  `35ACE349763CB478F277688A753A1B84A1352F5A7F23A4AF17C80E5AE9753B4C`.
  The only difference from the comparison-test source is the additional
  quiet-tick assertions in the invalid-dependency test.

Pure-Python counts (`break-python.xml`), excluding bpy module collection skips:

| Module | Passed | Skipped |
|---|---:|---:|
| test_batch_publish | 13 | 0 |
| test_canonical | 23 | 0 |
| test_composites | 15 | 0 |
| test_dagor_composites | 20 | 0 |
| test_dagor_random_parity_probe | 7 | 0 |
| test_materials | 31 | 0 |
| test_mesh_nodes | 23 | 0 |
| test_no_blender_seed_surface | 1 | 0 |
| test_payload_publish_v2 | 10 | 0 |
| test_pending_v4_surfaces | 1 | 0 |
| test_placement_publication | 6 | 0 |
| test_placements | 4 | 0 |
| test_project_textures | 17 | 0 |
| test_random_reference | 22 | 0 |
| test_runtime_parity | 9 | 0 |
| test_s4_ledger_purge | 2 | 0 |
| test_source_closure | 10 | 0 |
| test_source_inventory | 7 | 2 |
| test_transforms | 8 | 0 |
