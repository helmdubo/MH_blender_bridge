# GameObj admission baseline — independent diagnostic receipt

Date: 2026-08-28. Read-only production audit for owner doc13 section1.4;
follow-up question is `OPEN-V5-19`. This is not a completed slice or a fix.

## Scope and provenance

- Repository: `E:/GITHUB/Mimirhead_UE5Exporter/MH_blender_bridge`.
- Baseline: `5f566c7b16e36fa68e1e5ef1391675d1c5febf2d`, branch
  `v5/s6.1-dag4blend-bridge`, before any UE changes in this slice.
- Contract read in full:
  `C:/Users/helmd/.codex/attachments/f602a374-6782-4410-b10b-5714ff32382b/pasted-text.txt`.
- Relevant existing contract: `docs/10_source_protocol_v5_plan.md` section6.1
  and section6.4, especially lines464–500 and698–711.
- Engine: stock UE **5.7.4**, CL **51494982**, installed at
  `D:/PersonalProjects/UE5/UE_5.7/Engine`. Version confirmed by this Editor run.
- Host: `E:/MimirComposite_GameObj_Audit_20260828`; template copied from
  repository `tools/ue_s6_host`. Plugin is a **physical copy**, not a junction.
  Only `Source`, `Config`, and `MimirComposite.uplugin` were copied; binaries
  and intermediates were built fresh. All **126** original Source files were
  SHA256-identical to the repository before adding the diagnostic test.
- Added test only in this host:
  `Plugins/MimirComposite/Source/MimirCompositeTests/Private/MHGameObjAdmissionAudit.cpp`.
- No checkout/reset/stash/index/commit/push, no repository edits, no Engine
  edits, and no owner-project/session changes were made by this audit.

## Fixture and measured results

The fixture starts at the existing converted wire shape `kind: actor`, not
inside Blender. Each case is a fresh in-memory applied composite and an
EditorPreview world; it restores ActorClassRegistry and discards its objects.
It tests an ordinary `dummy_pivot` leaf and a one-option random node containing
`loot_audit_gameobj`, weight1. Seed100; translation `(123,-45,67)`, yaw90,
scale `(1,2,3)`. No external blueprint/gameplay code is executed: the collision
control registers only stock `/Script/Engine.StaticMeshActor`.

| Automation test, prefix `Mimir.Audit.GameObj.` | Result | Observation |
|---|---|---|
| `UnknownMustAdmitPlaceholder` | RED | Both tokens: source admission0, applied admission0, fresh preview plan0, spawned children0; `MH_E_UNRESOLVED_COMPOSITE_REFERENCE` |
| `RegistryCollisionMustNotSpawn` | RED | Both tokens: ordinary plan1, actual spawned child actor1 |
| `LowLevelPlaceholderPreservesNameAndTrs` | GREEN | Admit/resolve while registry exists; remove registry; call existing compiler with that plan: one placeholder leaf, zero child actor components, name/TRS preserved, one warning, canonical UE apply/extract bytes identical |

Report totals: **1 succeeded, 0 succeededWithWarnings, 2 failed, 0 notRun**.
The two RED tests assert the new doc13 requirement; they do not show that
existing legal MH actor semantics regressed. Their wire input currently has
no discriminator from a legitimate native MH actor token.

The low-level GREEN does not establish fresh-import admission or the full
Dagor/Blender reverse-export route. No RHI, PIE, packaged, or complete suite
acceptance is claimed. Two contextless EditorPreview-world cleanup warnings
appear in the collision test; its failed assertions specifically record the
observed child count1 versus required0.

## Reproduction commands

Run against the preserved physical host, not against the owner project.

```powershell
& 'D:\PersonalProjects\UE5\UE_5.7\Engine\Build\BatchFiles\Build.bat' `
  MimirCompositeV5S6Editor Win64 Development `
  '-Project=E:\MimirComposite_GameObj_Audit_20260828\MimirCompositeV5S6.uproject' `
  -NoEngineChanges -ForceUnity -DisableAdaptiveUnity -NoPCH -NoSharedPCH -WaitMutex `
  2>&1 | Tee-Object -FilePath 'E:\MimirComposite_GameObj_Audit_20260828\build.log'
```

Measured: **Succeeded**, 20 actions, 50.24 seconds. This diagnostic build
is not a substitute for both slice unity gates.

```powershell
& 'D:\PersonalProjects\UE5\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' `
  'E:\MimirComposite_GameObj_Audit_20260828\MimirCompositeV5S6.uproject' `
  -nullrhi -unattended -nop4 -nosplash -nosound -NoAssetRegistryCache -MHS6ParityHost `
  '-MHGoldenRoot=E:\GITHUB\Mimirhead_UE5Exporter\MH_blender_bridge\golden' `
  '-ExecCmds=Automation RunTests Mimir.Audit.GameObj' `
  '-TestExit=Automation Test Queue Empty' `
  '-ReportExportPath=E:\MimirComposite_GameObj_Audit_20260828\Reports' `
  '-log=E:\MimirComposite_GameObj_Audit_20260828\audit.log' `
  -stdout -FullStdOutLogOutput
```

All three tests ran at UTC16:19:44–16:19:45. **This TestExit command returned
process exit0 despite the two failed tests**; use JSON result states, not
process exit0, as the test verdict. Console output was returned to the tool;
the requested `audit.log` file was not found and is not claimed as an artifact.
The JSON report includes the test messages, counters, diagnostics, and failures.

## Evidence and call sites

Repository-relative paths at the baseline above:

- `addon/mh4blend/scene/import_dagor_composite.py:65`: `gameobj -> actor`.
- Same file, lines801–805: actor collection is only a transport hint, not a
  distinct output authority/carrier.
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositeImporter.cpp:328`:
  import calls `MHProbeCompositeBuildV5` before asset mutation.
- `.../MHCompositeCompiler.cpp:126–140,203,259`: absent/invalid registry entry
  rejects both ordinary nodes and every actor option.
- `.../MHCompositeResolvedPlan.cpp:203–205`: applied graph repeats admission.
- `.../MHCompositeActor.cpp:232–295`: fresh failure has no previous per-leaf
  plan; only the general diagnostic view is built.
- `.../MHCompositePlacementCompiler.cpp:142–174,188`: absent class becomes
  low-level placeholder; valid class selects ChildActorComponent and invokes
  `SetChildActorClass`.
- `.../MHCompositeRuntimeBridge.cpp:473,487–496`: runtime input admission also
  requires the actor registry and a shipping class.

This conflicts with the new uniform gameObj placeholder policy unless the
owner specifies how gameObj is distinguished from an executable MH actor.
`name` cannot become a hidden discriminator: doc10 makes it display-only and
does not permit it on random options. No global actor disable, role registry,
prefix heuristic, or new carrier was implemented or proposed as a decision.

## Artifact SHA256

All paths below are relative to this audit host.

| Artifact | SHA256 |
|---|---|
| `Reports/index.json` | `C56EAF149DE742A7EFAF7F66E2620ADAFA3B7E584FD20D1BC822F18DC3F915E5` |
| `Plugins/MimirComposite/Source/MimirCompositeTests/Private/MHGameObjAdmissionAudit.cpp` | `05ED8176DADCDA5977D524D7A070B9F6BEF57451C7909A1DA3A0BB7497D4E98D` |
| `build.log` | `0223D9BFD0284576443ABDE5CFA1F15F87C2A521D79AB5A7C4C3D10E63438B68` |
