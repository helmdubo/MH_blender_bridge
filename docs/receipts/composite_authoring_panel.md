# Composite Edit — authoring panel and in-session navigation

Contract: `docs/contracts/composite_authoring_panel.md`.
Baseline: `1e2814317551700624ffa55f39865a8d75f5712e`.
Branch: `codex/composite-authoring-panel`.

## Delivered behavior

The inspector separates Node, Transform and Content. Content is empty, one
managed reference, or weighted entity variants. Child nodes remain the actual
authored hierarchy; variants have no independent transform or child hierarchy.
An ordinary mesh, composite or random node can own authored children, as the
existing format/compiler already allow. Procedural placement remains protected
from ordinary transform edits.

The full placement tree stays in the Edit Mode toolkit. Entry reveals the
chosen occurrence before mirroring its authored-node selection. Locked context
is subdued; active scope is distinguished even while preview loading or errors
are displayed. Refresh preserves unrelated branch expansion.

Edit Contents and breadcrumbs change the definition inside the same session,
mode and toolkit. Dirty navigation offers Save and Discard; closing the choice
stays in the current scope. No camera operation or Edit Mode exit/re-entry is
part of navigation. Final Save/Cancel still finish the entire session.

Before switching, the target must resolve. A failed projection switch restores
the previous draft, selection, original bytes and epoch. Publishing updates the
baseline without closing the session. If the destination disappears after the
source was successfully saved, the current scope stays open with that saved
baseline; its suppression lease is rebound to rebuilt pooled handles. Undo is
cleared at the source-publication / successful-definition-switch boundary.

Content Browser multi-selection feeds two explicit commands:

- Add Nodes creates one identity-transform reference node for each selected
  managed MH mesh/composite under the selected authored node, or at the current
  definition root when no node is selected.
- Add Entities appends weight-one variants to the selected node. Its previous
  single reference becomes the first variant, also weight one. Node identity,
  transform, metadata and children survive this conversion.

Weights and removal are editable in Content, including when a variant row is
selected. Computed probabilities are displayed without rewriting authored
weights. Removing the final variant clears content but retains the node.

Asset batches validate completely before mutation, use one transaction and one
projection refresh, and retain Content Browser order. Above/below drops insert
an ordered sibling block; onto Random adds variants; onto other authored nodes
adds children. Deferred UI work checks session identity, scope epoch and draft
change serial before mutation.

The first full regression exposed a stack overflow on a cyclic draft. The
compiled-recipe guard tracks UObject identity, so it cannot see a logical-name
cycle introduced when a transient draft replaces its published definition.
The editor now checks the combined graph by logical name before preview layout,
including indirect references, child nodes and inactive/zero-weight variants.
An invalid graph retains an undoable draft and the last valid preview; a failed
scope switch rolls back in place. Save repeats source-graph admission before
asset application or source writing. The transform-only drag path is unchanged.

## Scope boundaries

Only managed MH meshes/composites can be added through Content Browser in this
slice. Ordinary UE assets need an explicit source-identity/export path; they
are rejected without inventing a resource name. Existing actor/gameobj content
is preserved. No wire-format, random-selection algorithm, Engine source or
Blender/reference implementation was changed.

Automatic arrangement/simulation and extracting a subtree as a standalone
composite remain future slices. Batch commands already accept individual local
transforms so future arrangement can reuse the same atomic authoring path.

## Verification

Isolated packaged host:
`E:/temp/MH_CEI1_package_20260907/HostProject`.
Evidence: `E:/temp/MH_AuthoringPanel_20260909`.
Engine: UE 5.7.4, `D:/PersonalProjects/UE5/UE_5.7`, read-only.

| Check | Evidence | Result |
| --- | --- | --- |
| Non-unity / no PCH / no shared PCH, `-NoEngineChanges` | `PUBLISH_GRAPH_STRICT.log` | 35 actions, succeeded |
| Final inspector and rendered-proxy test increments, same flags | `UI_STATUS_STRICT.log`, `RHI_FRAMES_STRICT.log` | 4 + 4 actions, succeeded |
| Targeted authoring/navigation recovery after cycle fix | `RECOVERY.log`, `RecoveryReport` | 7/7, exit 0 |
| Full NullRHI regression, normal exit | `FULL_FIXED.log`, `FullFixedReport` | 181 success + 117 warning, 0 failed, exit 0 |
| D3D12 Edit Mode / Outliner / thumbnails, normal exit | `RHI_EDIT.log`, `RhiEditReport` | 22 success + 41 warning, 0 failed, exit 0 |
| Final combined D3D12 including native ISM hit-proxy regressions | `RHI_FINAL.log`, `RhiFinalReport` | 24 success + 43 warning, 0 failed, exit 0 |

Automation JSON and process logs were inspected separately. Both successful
complete runs continue through object teardown to `LogExit: Exiting.` and
`Log file closed`; they use native `Automation SoftQuit`, not forced TestExit.
The NullRHI log explicitly skips five renderer-only bodies; the final RHI run
executes all five, with no NOT RUN entries. The cottage
metrics probe is not run because that portfolio asset is absent in this host.
It is not claimed as field-scene acceptance.

The first combined RHI run reported two legacy HActor expectations against the
current pooled ISM hit proxy, then a renderer assertion at final shutdown.
The tests now verify the rendered HISM component/index, plan-aligned leaf and
pool reverse lookup to its exact composite owner. Real ProcessClick still must
select that owner. The repeated-rebuild test now advances through real engine
frames rather than four synchronous draws in one frame. Cached hit proxies are
invalidated, scene-view lifetime ends before map teardown, and fixture assets
stay alive through a cleanup frame. The isolated four-test rerun and the final
combined 67-test rerun both pass and exit normally. These repairs modify only
test code; production pool/rendering behavior was not changed for them.

All 239 source files in the built host match the workspace by SHA-256.
The plugin was installed in the portfolio project; all 248 installed files
were verified after replacement.

## Original field-test package

`E:/temp/MH_AuthoringPanel_20260909/MimirComposite_AuthoringPanel_20260909.zip`
contains 248 plugin files, including current binaries, PDBs, source and config;
no Intermediate directory. BuildId: `47537391`.

SHA-256: `15C4A1045923EED9406CBD1B69FACF32CC013703AF3C42C6DC0A3E9D164B0BAC`.
Per-file hashes: `E:/temp/MH_AuthoringPanel_20260909/manifest.json`.
This archive predates the compact-tree follow-up below. It is retained as the
original authoring-panel artifact, not the latest installed UI.

## Compact-tree follow-up and installation

Owner requested only icon, name and Edit in tree rows. Removed textual kind,
access and selected badges plus the Variant prefix. Preserved icons, scope
dimming and selection colors; renamed the row button from Edit Contents to Edit.
Inspector information and switching behavior are unchanged.

`TREE_COMPACT_STRICT.log`: non-unity/no-PCH/no-shared-PCH build succeeded
(4 actions). This final presentation-only follow-up was compiled and diff-checked;
the full and RHI runs above precede it. No new tests were added for label removal.

Installed into `D:/PersonalProjects/UE5/MimirHead_portfolio 5.7/Plugins/MimirComposite`.
Backup: `Saved/MimirPluginBackups/MimirComposite_20260909_195151`.
Installation receipt: `Saved/MimirPluginBackups/installation_20260909_195151.json`.
All 248 files verified; BuildId remains `47537391`.

Owner explicitly requested commit, push and merge after installation. This is
publication authorization; the cottage metrics probe remains unexecuted in the
isolated host as documented above.

## Field checks

1. Enter Edit from a deeply nested mesh. Confirm the correct occurrence is
   expanded, selected authored node is visible, and unrelated branches stay dim.
2. Switch sibling, parent and root from the panel with clean and dirty drafts.
   Exercise Save, Discard and closing the choice. The panel/mode and camera
   should remain in place throughout.
3. Select several MH assets in Content Browser; Add Nodes at root and beneath
   a mesh/composite/random node. Undo/Redo once; verify the whole batch changes.
4. Add Entities to empty and single-reference nodes, edit weights, remove the
   last variant, and verify authored children are retained.
5. Drop several assets above/below an authored row, save, reopen and check order.
   Test normal transform drag and Cancel/Esc as before.
