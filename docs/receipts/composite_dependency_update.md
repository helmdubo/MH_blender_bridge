# Update dependencies for selected composites — 2026-09-14

Selection behavior was subsequently corrected and extended to Static Meshes;
see [dependency selection scope](dependency_selection_scope.md). The evidence
below records the initial implementation and installation.

## Scope and behavior

Owner clarified that the selection scope is an MH Composite, in the scene or
Content Browser. `Update dependencies` is exposed in the composite asset context
menu and the scene's `Composite Options` submenu. Both routes use the same
`MHUpdateCompositeDependencies` operation. Scene selection is captured when the
menu opens. Multiple roots and shared dependencies are deduplicated.

The operation traverses the validated, applied composite definitions, including
nested composites, group children and every Random option (even weight zero).
It repairs mesh-to-MI assignments by visible `MaterialSlotName`, falling back to
`ImportedMaterialSlotName` only when the visible name is empty. The existing
canonical generated paths, managed-claim checks and embedded identity admission
are used for composites, meshes and MIs. There is no source scan, material
parameter overwrite or FBX reimport. A missing/cyclic branch or unavailable or
ambiguous MI is reported; other resolvable bindings can complete, and unresolved
slots retain their current assignment. Same-name assets are not guessed across
folders.

Changes use one native material-property edit per mesh, preserve slot names and
section mappings, wait for the changed meshes to finish compiling, then notify
the existing resource-change mechanism once per mesh. This reconciles endpoint
interfaces and pooled components. The UI refreshes selected scene placements or
loaded instances of the selected Content Browser assets. Seeds, transforms and
composite definitions are not rewritten.

The whole UI operation is undoable. Modified mesh packages remain dirty for
normal Save/Save All, including ordinary managed-mesh local-edit tracking;
receipts are not forged or reset. Repeating a completed binding repair performs
no mesh mutation. A shared mesh asset naturally changes in all of its uses.
PIE, reentrant execution, active source import and Composite Edit Mode are
rejected before mutation.

## Verification

Evidence directory: `E:/temp/MH_CompositeDependencies_20260914`.

- `RED_BUILD_FIXED.log` compiled the UI test against the previous production
  plugin. `RedReport/index.json`: 0 passed, 1 failed, because the composite
  context menu has no `MHUpdateCompositeDependencies` entry. The initial test
  harness used a private ToolMenu method; that compile error was corrected to
  the public section API before recording runtime RED.
- Tests exercise Content Browser action execution, nested/all-option traversal,
  shared dependency deduplication, a zero-weight option, an unrelated mesh left
  untouched, slot/section/geometry/receipt preservation, no-op behavior,
  Undo/Redo, missing and ambiguous bindings, active-import rejection and the
  scene action with live components.

- `GUARDED_STRICT.log`: guarded non-unity/no-PCH build succeeded (4 actions,
  42.23 seconds overall). `GreenReport/index.json`: all four new tests completed,
  3 success + 1 success with expected missing-package warnings, 0 failed.
- `EarlyRHIReport/index.json` also recorded four successful tests, including
  a non-null live D3D12 scene proxy. That process later exited with code 3 in
  WebBrowser/libcef shutdown, after the automation report and editor shutdown.
  `EARLY_RHI_NOCEF.log` / `EarlyRHINoCEFReport/index.json` repeats the same
  D3D12/offscreen tests with Unreal's supported `-nocef` switch: exit 0,
  3 success + 1 expected-warning success, 0 failed, clean shutdown. This is a
  test-host launch option only; owner project settings remain unchanged.
- Before/after performance smoke uses the existing `Mimir.V5.Composite.Perf`
  tests, which set `mh.PerfTrace` themselves. An initial baseline invocation
  set the cvar from the console and prevented the tests' lower-priority
  `ECVF_SetByCode` trace-zero step from taking effect. The corrected baseline
  `PerfBeforeVerifiedReport/index.json` has 3 completed, 0 failed;
  `PERF_BEFORE_VERIFIED.txt` contains the map-load and reimport trace lines.

- `STRICT_UAT_VERIFIED.log`: fresh `BuildPlugin -StrictIncludes` into
  `PackageVerified`, non-unity/no-PCH, Win64 Editor + Development + Shipping,
  `BUILD SUCCESSFUL`, exit 0, 6m40s. The first strict attempt only failed in
  the test harness (explicit `MaterialDomain.h` and the public
  `TryExecuteToolUIAction` API corrected the compile errors); production
  implementation compiled in both attempts.
- `host_source_parity.json`: all 261 Source/uplugin inputs equal the repository
  by SHA-256. Full automation uses the independent
  `AutomationVerified/MimirCompositeV5S6.uproject`, whose plugin junction targets
  the verified build host. No owner content was used as a test fixture.
- `FullReport/index.json`: 322 completed tests = 188 success + 134 success with
  warnings, **0 failed**, process exit 0, clean shutdown. Both
  `RecipeShadowParity` tests, native material admission/deleted-MI recovery,
  single-FBX import and all four dependency-update tests succeed. The 14
  pre-existing conditional checks requiring rendering or external fixtures are
  recorded separately in `conditional_checks.json`; this NullRHI run is not
  evidence for those checks.
- `PERF_AFTER.txt` captures the same map-load/reimport instrumentation as the
  corrected baseline. Reimport before/after is 114.500/97.617 ms with
  `full_scan_count_delta=0`, `incremental_paths=1`, one notified key/actor and
  no recipe recompilation in both. These are smoke samples under differing
  process load, not a controlled performance benchmark of the new action.

- `FORCE_UNITY.log`: guarded `-NoEngineChanges -ForceUnity
  -DisableAdaptiveUnity -NoPCH -NoSharedPCH` build succeeded, 17 actions,
  125.12 seconds overall. Engine source and global build configuration were
  not modified.

- `FINAL_RHI.log` / `FinalRHIReport/index.json`: the final unity-built DLLs pass
  all four dependency-update tests under D3D12/offscreen with `-nocef`:
  3 success + 1 expected-warning success, 0 failed, exit 0 and clean shutdown.
- `git diff --check` and `python tools/check_normative_docs.py` pass.

## Installation

The owner had already confirmed Unreal Editor was closed. Installation rechecked
the process state, verified matching Engine BuildId `47537391`, verified all
source inputs against the build host, backed up changed destination files,
copied the verified build and checked every installed input by SHA-256.

- Destination: `D:/PersonalProjects/UE5/MimirHead_portfolio 5.7/Plugins/MimirComposite`.
- Result: **11 files replaced/added, all 272 installation inputs verified**.
- Manifest: `E:/temp/MH_CompositeDependencies_20260914/INSTALL.json`.
- Backup: `D:/PersonalProjects/UE5/MimirHead_portfolio 5.7/Saved/MimirPluginBackups/CompositeDependencies_20260914_183915`.

No owner asset packages, source payloads or project settings were changed during
installation. On the next Editor start, select an MH Composite in the Content
Browser and use **Update dependencies**, or select its scene placement and use
**Composite Options > Update dependencies**. Save the modified meshes afterward.
The owner's actual restored decal/composite workflow remains their field check;
automated acceptance above uses generated fixtures in the independent host.
