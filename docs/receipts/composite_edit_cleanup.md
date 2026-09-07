# CE-6b2 — Composite Edit cleanup, 2026-09-08

Status: VERIFIED; ready for merge. Owner accepted the interaction and Break field fixes and
explicitly requested cleanup, commit, push and merge into main. Integration
starts from `a67e271` on `codex/composite-break-undo`; the accumulated stack
since main `0f46f95` is included in PR #166.

## Changes

One editing backend remains: subsystem, session, transactional draft, projection
and editor mode. Removed the backend setting, actor-owned editing graph/document,
actor edit Tick, scope handles and extraction path, legacy subsystem commit/cancel
branches and the old global Enter/Apply input preprocessor. Escape from the
Outliner defers Cancel with a session epoch guard; Enter passes through.

Removed unused standalone Outliner selection/fallback code and API wrappers.
The panel is owned by the active Edit session. Native actor/atomic secondary
selection, projection node selection, fast drag and pooled placement rendering
remain. Seed/runtime-operation guards query the actual subsystem session.

Bake Current Result now reads the current projection plan instead of the sealed
placement plan. Its existing test edits the draft before baking and verifies
the copied transform. Source publication failure keeps the current draft.

No Engine, portfolio assets, source format or reference/golden data changes.
Historical audit documents and receipts remain available.

## Test migration

The suite changes from 297 to 292 tests: six obsolete tests removed and one
single-backend regression added. Three tests are renamed while retaining their
purpose under the current architecture.

| Removed test (prefix `Mimir.V5.Composite.`) | Current coverage |
| --- | --- |
| `EditMode.Mode.LegacyBackendNeverActivatesIt` | `EditMode.SingleBackendSettings`, `EditMode.Mode.FollowsTheSessionAndLocksTheContext` |
| `EditMode.Characterization.InlineDescendantResolvesToTopAncestorHandle` | `EditMode.Selection.ClicksGrabTheProjectionNode`, `EditMode.Projection.ComponentsMapToDraftNodes` |
| `EditMode.Characterization.DraftPreviewFollowsEveryInvocationInThisPlacement` | `EditMode.Projection.ShowsTheOccurrenceAndSuppressesItsInstances` |
| `EditMode.Characterization.UndoInsideSessionEndsIt` | `EditMode.Session.CommandIsUndoableWithoutClosing`, `EditMode.Transform.GizmoGestureWritesTheDraftInOneUndoStep` |
| `EditContext.ScopeHandlesAreGrabbableAndFramed` | `EditMode.Selection.ClicksGrabTheProjectionNode`, `EditMode.NodeFrame.BindsNestedAndRandomVisualsToStableAuthoredFrames` |
| `Outliner.InstanceSelectionRetention` | `EditMode.Structure.OutlinerPinsActiveSessionRoot` |

`EditContext.EscapeCancelsEnterApplies` becomes `EscapeCancelsEnterPassesThrough`;
`QueuedApplyIgnoresLaterSession` becomes `QueuedCancelIgnoresLaterSession`.
`Pool.UndoDuringEditRestoresPreview` becomes `Pool.UndoRedoRestoresPreview`:
placement Undo/Redo verifies live pool rows, while transactional draft Undo is
covered by the session/gesture tests above. Other legacy fixture operations now
use draft commands and projection components.

## Validation

Own hosts only: `E:/temp/MH_CEI1_host_20260907` and
`E:/temp/MH_CEI1_package_20260907/HostProject`, stock UE 5.7.4, BuildId 47537391.
Logs and JSON reports are in the first host directory.

RED: `CLEANUP_RED_BUILD.log` / `CLEANUP_RED.log` / `CleanupRedReport`.
With production still at `a67e271`, `EditMode.SingleBackendSettings` fails because
the reflected backend toggle still exists. Perf baseline: `CLEANUP_BEFORE.log`
and `CleanupBeforeReport`, 2/2 pass.

Initial guarded full run (`CLEANUP_FULL.log`, `CleanupFullReport`) ran all 292
tests and exposed three migration failures. The procedural profile parent is
correctly protected by the modern transform command; its test must edit an
ordinary child and separately verify rejection. Geometry counting must include
projection SMCs as well as unsuppressed pool instances. The panel retained a stale tree after closing a session without a native
selection change; its ownership refresh now follows mode Enter/Exit events. Final results follow below.

The intermediate Outliner lifecycle patch used deprecated global editor-mode
notifications; guarded and strict builds rejected them with warnings-as-errors
(`CLEANUP_GUARDED_FIXED.log`, `CLEANUP_STRICT_FINAL.log`). It was replaced with
notifications scoped to the level editor mode manager. A prematurely launched
`CLEANUP_FULL_FINAL.log` run was stopped because its build failed; it is not
validation evidence.

Completed source comparisons: all 238 Source files match both validation
hosts after CRLF normalization (`CLEANUP_SOURCE_HASHES.json`). The deleted
standalone Outliner test is absent from both builds.

| Check | Evidence | Result |
| --- | --- | --- |
| Guarded non-unity/no-PCH | `CLEANUP_GUARDED.log`, `CLEANUP_GUARDED_FINAL.log`, `CLEANUP_GUARDED_VERIFIED.log` | Success; initial 173 actions / 324.13 s, final incremental 7 / 28.72 s |
| StrictIncludes non-unity/no-PCH | `CLEANUP_STRICT.log`, `CLEANUP_STRICT_VERIFIED.log` | Success; initial 113 actions / 287.08 s, final incremental 7 / 32.33 s |
| PerfTrace after, strict binaries | `CLEANUP_AFTER.log`, `CleanupAfterReport` | 2/2 |
| Force-unity / adaptive-off / no-PCH | `CLEANUP_UNITY_VERIFIED.log` | Success, 21 actions / 89.15 s |
| Focused repaired cases | `CLEANUP_FOCUSED.log`, `CleanupFocusedReport` | 3/3 |
| Full Mimir + golden, guarded | `CLEANUP_FULL_VERIFIED.log`, `CleanupFullVerifiedReport` | 178 success + 114 warning = 292 passed, 0 failed |
| Full Mimir + golden, strict binaries | `CLEANUP_STRICT_FULL.log`, `CleanupStrictFullReport` | 178 success + 114 warning = 292 passed, 0 failed |
| D3D12 / RenderOffscreen, strict binaries | `CLEANUP_RHI.log`, `CleanupRhiReport` | 34 success + 63 warning = 97 passed, 0 failed |

Both `RecipeShadowParity` and `RecipeShadowParityApplied` pass in both full
runs. Warning outcomes include isolated-world and expected-diagnostic fixtures;
JSON report failures and not-run counts are zero. Full suite changes are listed
above rather than comparing totals as if the old backend still existed. RHI
covers EditMode, EditContext, Selection, Async, selected dependency persistence,
save-proof warnings, both recipe parity gates, Thumbnail, Break and Pool.

The first force-unity attempt (`CLEANUP_UNITY.log`) stalled with all eight
compiler processes waiting and no completed actions for several minutes. Only
that validation build and its owned compiler children were stopped. The retry
uses force-unity/adaptive-off with no PCH/shared PCH and four actions. No source
changes or warning suppression were used to work around the build stall.

PerfTrace counter invariants before/after: map load retains zero sync package
loads, endpoint lookups, identity admissions and compilation waits; two
components reused, none created. Targeted reimport retains zero full scans,
recipe recompiles, parent recompiles and actor rebuild time; bucket migrations
remain 1/0. Map load total is 0.134 → 0.124 ms, targeted reimport totals are
128.827/117.918 → 151.836/150.954 ms. The after run uses the strict host and
partly overlaps unity compilation; these are not controlled latency comparisons
and no speedup/regression claim is drawn from the durations.

## Remaining scope

Read-only review identified an existing separate limitation: the older scoped
`BakeScopeDocument` API looks up an exact invocation node, whereas a selected
Random composite option can use an `/options[N]` invocation address. This
cleanup does not extend that API's option-address support.

Deletion of ignored repository-local `ue/MimirComposite/Binaries` and
`Intermediate` was rejected by automatic approval review with `blocked by
policy`, without a more specific reason. Those generated directories remain
on disk and are not part of the commit. Validation hosts, evidence and the
portfolio rollback backup are retained.
