# Composite Save: float32 quaternion read-back, 2026-09-08

Status: VERIFIED; INSTALLED FOR FIELD TEST. Branch
`codex/fix-composite-save-validation`, base `8826fa1`; changes are not merged.
Owner field report: moving a node in `sovmod_cottage_i_plants_cmp` cannot Save;
the session reports `MH_E_COMPOSITE_GRAMMAR: temporary read-back validation
failed:` with no reason, followed by `MH_W_NO_EXTERNAL_CHANGE`.

## Cause and scope

Read-only inspection of the field source identified
`nodes[0].children[105].transform.rotation_quat`:
`[-0.025757322, -0.025756564, -0.7060713, 0.7072032]`.
Both parser and canonical writer normalize float32 quaternions. Another
normalization changes this quaternion's X/Y low bits, so parse/write is not
a byte fixed point. A translation-only edit, even on another node, therefore
failed the whole-file `Rewritten == Bytes` check in `AtomicWriteComposite`.
That comparison supplied no diagnostic. It did not prove damaged disk bytes.

`MH_W_MANAGED_ASSET_LOCALLY_MODIFIED` is a separate local-state warning, not
the publication refusal. No index or asset-validation performance change is
part of this fix.

The caller still produces the payload with the existing canonical writer.
Atomic publication retains:

- exact disk read-back equality against those original bytes;
- successful parsing and writer admission of the read-back;
- sibling temporary file, cleanup on validation failure, atomic replacement;
- receipts calculated from the original bytes that were actually committed.

Only the invalid additional fixed-point requirement is removed. Read failures,
byte mismatches and admission failures now have a nonempty diagnostic.
Parser, canonical writer, source grammar, RNG, reference/golden and the separate
import apply/extract equality check are unchanged.

## Regression and verification

`Mimir.V5.Composite.PublishTranslatedRotation` uses the field quaternion on a
synthetic managed group. It applies a translation, proves that valid writer
bytes differ from a parse/rewrite, and calls the real publisher twice. It checks
exact published bytes, source/applied receipts, clearing of the local-modified
state, readable source and the edited position. Existing session tests retain
coverage of failed Save preserving the draft and retrying.

Own hosts:
`E:/temp/MH_CEI1_host_20260907` and
`E:/temp/MH_CEI1_package_20260907/HostProject`.
All 238 Source files match the worktree after CRLF normalization for GREEN.
Build/test evidence lives in the first host directory.

- RED: `SAVE_ROTATION_RED_BUILD.log` succeeded, then `SAVE_ROTATION_RED.log`
  reproduced the exact empty read-back error in the new test. Two perf probes
  passed. JSON: `SaveRotationRedReport`.
- Guarded non-unity/no-PCH GREEN build: `SAVE_ROTATION_GREEN_BUILD.log`,
  4 actions, succeeded.
- StrictIncludes-equivalent packaged-host non-unity/no-PCH:
  `SAVE_ROTATION_STRICT.log`, 7 actions, succeeded.
- Final strict build: `SAVE_ROTATION_STRICT_FINAL.log`, 4 actions, succeeded.
- Force-unity/adaptive-off/no-PCH: `SAVE_ROTATION_UNITY.log`, 25 actions;
  final isolated test fixture: `SAVE_ROTATION_UNITY_FINAL.log`, 4 actions;
  both succeeded.
- Full NullRHI/golden: `SAVE_ROTATION_FULL_FINAL.log` /
  `SaveRotationFullFinalReport`, 179 success + 114 warning = **293 passed**,
  0 failed/not-run. Both RecipeShadowParity gates and the new regression pass.
- Focused regression and before/after perf probes on the own host:
  `SAVE_ROTATION_GREEN_FINAL.log` / `SaveRotationGreenFinalReport`, **3/3 passed**.

Validation iterations: the first full run pinned `mh.PerfTrace` through a
console command, preventing two instrumentation tests from turning it off.
The full-suite command was corrected to leave that cvar under test control.
The repeated full run then exposed a fixture collision: its fixed package name
already existed on disk from the preceding run. The new regression now uses a
unique package and source directory per run. No production changes were needed
for either harness issue; final full/focused runs use the isolated fixture.

Perf invariants before/after: map load has zero sync package loads, registry
lookups, identity admissions and compilation waits; two components reused,
none created. Targeted reimport has zero full scans, recipe/parent recompiles
and actor rebuild time; bucket migrations remain 1/0. Map-load totals are
0.122/0.151 ms; reimport totals 512.498/569.746 vs 171.517/130.369 ms. Runs overlap
other validation work and use different compile modes; no speedup claim.

Initial RED build was refused because portfolio Live Coding was active.
The retry used `-NoHotReloadFromIDE` only in the separate own host with
`-NoEngineChanges`; portfolio binaries and the running editor were not changed.

Field source SHA-256 when investigated:
`73f0770eb396c7d64c6c53461ae54ff2799dbc533fcec33c2641a341e123bf4d`.
The reproduction uses copied numeric values, not writes to the field file.

## Field installation

Verified archive:
`E:/temp/MH_SaveRotation_20260908/MimirComposite_SaveRotation_Verified_UE5.7.4_Win64.zip`.
SHA-256: `25b14431215ea8b6ead4e7d4c7f5fdb7610498ce0121bbeb59d1f0caca54db62`.
All 244 archive/installed files checked against the manifest; Engine BuildId
`47537391`. The archive contains the final tested strict binaries and matching
source. Earlier non-final archive in that directory is superseded.

Installed after the editor was closed, without starting it:
`D:/PersonalProjects/UE5/MimirHead_portfolio 5.7/Plugins/MimirComposite`.
Backup:
`D:/PersonalProjects/UE5/MimirHead_portfolio 5.7/PluginBackups/MimirComposite_before_SaveRotation_20260908_181016`.
Installation receipt: `E:/temp/MH_SaveRotation_20260908/installation.json`.
Next field check: reopen the scene, move a node in `plants_cmp`, Save, then enter
Edit again and confirm the saved position. Source publication itself was tested
on synthetic assets, not performed against the user's composite.
