# Update dependencies: secondary selection and Static Mesh — 2026-09-14

## Requested correction

The first implementation always used the selected actor's root composite.
The owner clarified that a secondary selection highlights one concrete mesh:
only that mesh must be repaired. Selecting a composite itself still repairs its
complete dependency closure. Selecting a chair inside the house interior must
not repair neighboring interior meshes or other house dependencies.

## Implementation

- The viewport composite action captures the secondary leaf path, logical mesh
  identity, mesh object and actor definition together when the submenu opens.
  It uses the existing native primary/secondary selection adapter state.
- Execution validates that the captured path still denotes that same mesh in
  that definition. Another highlighted object does not retarget an open menu.
  A stale/unavailable target fails before mutation; it never falls back to the
  actor root or nearest containing composite.
- Primary actor selection, World Outliner selection and Content Browser
  composite selection retain recursive/all-option closure behavior. Selecting
  a subcomposite itself limits that closure to the selected subcomposite.
- `MHUpdateAssetDependencies` accepts the union of composite roots and explicit
  mesh assets. The existing `MHUpdateCompositeDependencies` remains a wrapper.
  Explicit meshes need no MH import receipt. Slot lookup still admits canonical
  generated MIs and preserves ambiguous/missing assignments, names, geometry,
  Undo/Redo and normal dirty-package saving. Only admitted managed meshes emit
  the existing MH resource notification; ordinary meshes use their native
  material-property edit lifecycle.
- Static Mesh assets have **Update dependencies** in the Content Browser.
  Static Mesh actors have the action directly in their scene context menu;
  mixed selections of these actors and composite actors are supported.
- Secondary updates do not enqueue the owner root for traversal or explicit
  whole-actor rebuilding. Changes to a shared mesh asset naturally affect every
  instance that uses it. No FBX/material source import or MI parameter mutation
  was added.

## Verification

Evidence root: `E:/temp/MH_DependencySelection_20260914`.

- `RED_BUILD.log`: the new tests compile against the previous production code.
  `RedReport/index.json`: 4 completed successfully, 3 failed. The secondary
  test specifically fails because both the interior sibling and outer house
  mesh were changed. The two Static Mesh tests fail because their menu entries
  are absent. This reproduces the owner-reported scope bug.
- Regression coverage uses the native primary/secondary selection adapter,
  real generated ToolMenus and executed menu actions. It checks a mesh inside
  a random nested composite, sibling/outer exclusion, frozen menu selection,
  stale definition rejection, explicit subcomposite and root selection,
  Content Browser multi-mesh selection, a mesh without an MH receipt,
  imported-slot-name fallback and a standalone Static Mesh scene component.

- `GREEN_BUILD.log`: guarded non-unity/no-PCH incremental build passes,
  8 actions, 43.17 seconds. `GreenReport/index.json` / `GREEN_RHI.log`:
  all 7 focused tests complete under D3D12/offscreen (`-nocef`), 6 success +
  1 success with expected missing-MI warnings, 0 failed, exit 0 and clean
  shutdown. The stale-target test expects the explicit error and verifies no
  fallback mutation; this is not an ignored unexpected test failure.
- `change_scope.json` compares source files with the previously installed
  build: changes are confined to dependency implementation/API, source menus
  and their automation tests. Earlier material/FBX import changes are retained.

- `STRICT_UAT.log`: fresh Win64 `BuildPlugin -StrictIncludes` with unity/PCH
  disabled, Editor + Development + Shipping, **BUILD SUCCESSFUL**, exit 0,
  7m11s. `host_source_parity.json` verifies all 261 Source/uplugin inputs against
  the repository by SHA-256.
- Full automation uses the guarded non-unity development copy that passed the
  focused D3D12 tests; `full_host.json` records its source parity. The final
  packaged host can therefore perform the separate guarded unity gate while
  those tests run, without replacing loaded DLLs. All hosts are independent of
  the owner's project. Performance baseline `PERF_BEFORE.txt` is the trace from
  the immediately preceding implementation's full acceptance run
  (`MH_CompositeDependencies_20260914/FULL.log`).

- `FullReport/index.json` / `FULL.log`: 325 tests completed = 191 success +
  134 success with warnings, **0 failed**, exit 0, clean shutdown. Both
  RecipeShadowParity tests, NativePrimarySecondaryCycle, all 7 dependency
  tests, native material admission/recovery and single-FBX import succeed.
  The same 14 existing rendering/external-fixture conditional checks are
  listed separately in `conditional_checks.json`; NullRHI does not verify
  those conditional paths. `PERF_AFTER.txt` captures the existing map-load and
  reimport counters; before/after samples are smoke evidence, not a controlled
  performance benchmark of this new command.

- `FORCE_UNITY.log`: guarded `-NoEngineChanges -ForceUnity
  -DisableAdaptiveUnity -NoPCH -NoSharedPCH` passes, 17 actions, 112.86 seconds.
  Reimport smoke before/after: 97.617/120.695 ms while the latter run shares
  CPU with this build; both retain zero full scans, one incremental path,
  one notified key/actor and zero recipe recompilations.

- `FinalRHIReport/index.json` / `FINAL_RHI.log`: the final unity-built DLLs pass
  all 7 focused tests under D3D12/offscreen (`-nocef`), 6 success + 1 expected
  warning success, 0 failed, process exit 0 and clean shutdown.
- `git diff --check` and `python tools/check_normative_docs.py` pass.

## Installation

The owner requested installation for their own field testing. Their Editor was
closed; installation rechecked this immediately before copying. The installer
verified Engine BuildId `47537391`, matched repository source inputs against
the tested build, backed up replaced files and verified the final destination
by SHA-256.

- Destination: `D:/PersonalProjects/UE5/MimirHead_portfolio 5.7/Plugins/MimirComposite`.
- **10 files replaced; all 272 installation inputs verified**.
- Manifest: `E:/temp/MH_DependencySelection_20260914/INSTALL.json`.
- Backup: `D:/PersonalProjects/UE5/MimirHead_portfolio 5.7/Saved/MimirPluginBackups/DependencySelection_20260914_193603`.

Owner content, source payloads and project settings were not changed. The
Editor was not launched after installation. Owner acceptance is their next
manual test of the actual house/interior/chair placement; automated evidence
above uses isolated generated fixtures. Use Save/Save All after applying the
command to keep the modified mesh material assignments.

## Session documentation and integration authorization

On 2026-09-14 the owner requested documentation updates, commit, push and merge
of the full session: single-FBX import, native material import, candidate refresh
and context-aware dependency repair. README includes the MI recovery workflow;
Source Protocol §9.1 and §10.1 describe selection scope and targeted imports;
RECIPE_EXECUTION_STATUS links all stages and distinguishes automated validation,
confirmed MI recovery and the remaining secondary-selection field check.
Source hashes still match the 261 verified build inputs; this documentation
pass changes no production code or tests. The base equals current `origin/main`
at `992eff8` before integration, so no code reconciliation was necessary.
