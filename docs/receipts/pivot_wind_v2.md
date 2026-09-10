# Dagor wind motion revision 2 — implementation receipt

Date: 2026-09-11 (local). Worktree: `codex/material-reimport-batch-fix`.
This receipt covers the owner's request to replace the initial smooth MH wind
approximation with the Dagor motion equations. Earlier material reimport and
revision-1 import work remains in the same uncommitted worktree.

## Delivered code

- Ported ambient-wind sampling and `ApplyTreeWind` from DagorEngine
  `75723669297e48e200a0dc67b18c1629e0975daf`; retained RGB response masks, instance
  phase, time periods, direction conversion and the 0.25 pivot-only multiplier.
- Replaced analytic gusts with a generated 64-cubed linear volume. All 1,048,576
  BGRA source bytes match an independent build of upstream's generator.
- Added controller branch/detail amplitudes, default 0.1/0.1 from the supplied
  CDK environment configuration. Corrected raw noise-speed and azimuth semantics.
- Added an explicit validated v1-to-v2 graph upgrade, retaining interface and
  parameter identities, existing master surface graphs and material overrides.
  The function keeps its `MF_MHPivotWind_v1` asset name for reference stability.
- Added upstream attribution/license and the reference note
  `docs/reference_notes/dagor_wind_motion_parity_20260911.md`.

## Verification

Engine: UE 5.7.4, CL 51494982, BuildId 47537391. Tests run in the isolated
`E:/temp/MH_CEI1_package_20260907/HostProject` with the `MimirCompositeV5S6`
automation host. Engine source was not edited.

- Non-unity/no-PCH build, `-NoEngineChanges`, explicit module dependencies:
  `E:/temp/MH_PivotWind_20260910/BUILD_PARITY_4.log`, exit 0, 25.20 seconds.
  Final HLSL resource sync/up-to-date build: `BUILD_PARITY_5.log`, exit 0.
- Eleven import, noise, controller and graph/upgrade tests passed in
  `E:/temp/MH_WindParity_20260910/RhiReport/index.json`. The two then-failing
  numerical/render checks were corrected and rerun separately below; this is
  not a claim that the original report was entirely green.
- Final D3D12/SM6 focused run: `FloorReport/index.json`, **2 passed, 0 failed,
  0 not run**, process exit 0; `FLOOR.log` has no failed automation or fatal exit.
  Thirty numerical GPU cases cover the production helper formulas against
  independent upstream equations. Maximum ambient error was 0.0122797446 m/s;
  maximum leaf displacement error including texture-filtered combined cases
  was 0.000111553474 m (within their 0.001 m tolerance).
- Explicit negative-phase fractional wrapping fixed the numerical discrepancy;
  the equivalent `.125` and `1000.125` time cases now agree. Test tolerances
  were not relaxed.
- Real bush mesh/atlas raster test: SMC and ISM leaf-time frames match exactly;
  each changes 2,464 pixels between frozen times with pivot rotation disabled.
  Pivot displacement and rotated normals also pass. The leaf test connects an
  opacity-mask expression so UE retains the actual masked shader permutation.
- `python tools/check_normative_docs.py`: PASS. `git diff --check`: PASS.

Raw-volume SHA-256:
`595E0CF126745EBC44549531F051BF360705C60A8AB2AB096F0AD22AF6537557`.

## Staged upgrade and installation

The seven original portfolio wind/master packages were copied and hash-recorded
in `E:/temp/MH_WindParity_20260910/portfolio_inputs.json` before the upgrade.
The actual copied masters were upgraded under D3D12/SM6 in the isolated host:

- `SETUP_UPGRADE.log`: exit 0, **5 changed packages saved, 2 masters checked**;
  complete material shader maps validated. Its single warning concerned the
  unavailable NTFS journal on D:, not material compilation.
- `SETUP_REPEAT.log`: exit 0, **0 changed packages, 2 masters checked**,
  0 errors and 0 warnings. SHA-256 hashes of all eight staged assets stayed
  identical on repeat (`staged_upgrade.json`).
- `INSTALL.log`: installed **267 plugin files and 5 wind/master assets** into
  `D:/PersonalProjects/UE5/MimirHead_portfolio 5.7`. Preflight verified the editor
  was closed, portfolio inputs had not changed, source files matched the tested
  build and the Engine BuildId matched. Every copied file was hash-verified.
- **608 material-instance package hashes remained unchanged**. The controller
  Blueprint and two default pivot textures were unchanged as well.
- Backup and complete installation manifest:
  `D:/PersonalProjects/UE5/MimirHead_portfolio 5.7/Saved/MimirPluginBackups/PivotWind2_20260911_002849/installation.json`.

Installed status is `installed_and_hash_verified`. Opening the portfolio scene
loads the upgraded function through the existing master/MI references; material
source reimport is not needed for this shader update.

## Acceptance boundary

Formula tests and matching uncompressed generator bytes do not establish
pixel-identical motion in the owner's AV session: its shipped BC7/BC1 noise
texture was not available. Artist comparison in the portfolio scene remains
pending. Nanite, hardware ray tracing and motion-vector history are not part of
this acceptance. This revision neither regenerates source DDS/FBX files nor
consolidates/reparents the mesh-specific material instances.
