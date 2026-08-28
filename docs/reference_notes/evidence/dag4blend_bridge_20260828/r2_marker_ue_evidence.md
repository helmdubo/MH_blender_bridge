# V5-S6.1 marker: bounded C++ implementation evidence

Status: marker-only implementation, focused gates and completed NullRHI suite;
full D3D12 suite CRASHED and remains an OPEN gate. NOT V5-S6.1 acceptance.
Date: 2026-08-28. Worker: ue_components_audit. No commit/push, Engine edits,
installed-plugin changes, or owner-editor operations were performed.

## Inputs and isolation

- Owner R2: `C:/Users/helmd/.codex/attachments/01df9fa7-c450-488c-bd6b-a31273e28cc8/pasted-text.txt`, SHA256 `07970B629A98CAD1C013E909366B0BEEF4A436E9231E29465629A999B9C6141C`.
- Working source: `E:/MimirComposite_V5S6_1_Direct_Source_20260828`, base `9de0f7c1119b07d26d67edfda24f353fc993f730`.
- Physical private host: this directory; no plugin junction to the repository.
- Engine: stock `D:/PersonalProjects/UE5/UE_5.7/Engine`, 5.7.4 CL51494982.
- Before full Automation: all 127 plugin Source files byte-identical between host and current working source. Four pre-existing baseline host files differed only in EOL; copies were aligned without repository edits.
- Historical `MHGameObjAdmissionAudit.cpp` moved out of host Source to this directory before the full suite. SHA256 remains `05ED8176DADCDA5977D524D7A070B9F6BEF57451C7909A1DA3A0BB7497D4E98D`; old diagnostic RED tests are not production tests and remain recoverable.

## RED before production

`Mimir.V5.Composite.Marker.CodecRoundTrip` built against unchanged baseline C++
and failed both admissions:

```text
MH_E_UNSUPPORTED_NODE_KIND: unsupported composite node kind 'marker'
MH_E_UNSUPPORTED_NODE_KIND: unsupported random option kind 'marker'
```

No marker production edits preceded this result. Baseline test source retained
as `marker_baseline_test.cpp`, SHA256
`C5F7390118DD3EE3775E1DAA7757DB364EEC8A5CD0B78366BDE54A72DA1D1D6E`.
The test function remains unchanged in the expanded production test file.

- `MarkerBaselineReport/index.json`: 0 passed / 1 failed, 2 assertions; SHA256 `07E98C0690147AD85C2FECC0032D7AC5613D9B39FFD89CF6C166B4D628E2C5F3`.
- `marker_baseline.log`: SHA256 `100BE80ED9F9B978981E72EDDE132B12257F8F1F03F2D21FE674CCA33F91FE49`.
- Baseline build: 4 actions, PASS, 34.47 s. Initial attempt was safely stopped by the active owner-editor Live Coding guard before compilation; owner session was not touched. The isolated build used `-NoHotReloadFromIDE` thereafter.

## Implemented mapping

All paths below are under `ue/MimirComposite/Source/` in the working source.

- `MimirCompositeEditor/Public/Composite/MHCompositeAsset.h`: append Marker to both asset enums; old ordinals preserved.
- `MimirCompositeEditor/Private/Composite/MHCompositeProtocol.cpp`: ordinary/option marker parse and canonical write; canonical resource required; option transform/profile still rejected.
- `MimirCompositeEditor/Private/Composite/MHCompositeResolvedPlan.cpp`: explicit enum conversion, never default marker to Group/Empty. Applied admission still resolves registry only for Actor.
- `MimirCompositeEditor/Private/Composite/MHCompositeCompiler.cpp`: marker has no generated endpoint; children/profile still traverse.
- `MimirCompositeRuntime/Public/Random/MHRandomStream.h` and `Private/Random/MHRandomStream.cpp`: append runtime ordinal 6; derived `Nodes.SemanticKind/Resource`. Ordinary marker retains name/TRS; selected marker gets its `options[i]` node path, identity local TRS and random world matrix. No Leaf, draw, selected dependency or signature-preimage field is added.
- `MimirCompositeRuntime/Private/Composite/MHRuntimeCompositeInput.cpp`: closed explicit ordinary/option kind admission replaces ordinal-bound checks; marker resource survives the existing binary carrier. No new binding/key kind.
- `MimirCompositeRuntime/Private/Diagnostics/MHDiagnosticRegistry.cpp` and `MimirCompositeTests/Private/MHRandomStreamV5Test.cpp`: requested warning mirror `MH_W_DAGOR_CONSTRUCT_DROPPED`; counts 52 E / 15 W. This is not a new marker error family.
- New `MimirCompositeTests/Private/MHCompositeMarkerTest.cpp`: six tests; final SHA256 `2231F5721532151FE9F7D8D4BFC7C7C6988E3733020D4BD1CFD51AE383636DD5`.

No index schema, ActorClassRegistry semantics, runtime spawning code, frozen
goldens, source-generation tag, RNG tag, resolver tag, or signature formula changed.
Placement/appearance metadata is deliberately not implemented by this worker;
native Blender carrier authority remains a separate owner question.

## Focused verification

`MarkerCleanReport/index.json`: 20 passed, 0 failed, 0 warnings, 0 not-run.
SHA256 `0B8583D1E5323AAB5CE0B1BA8339416850F96F2DB3EE931B112C2E07F74D491C`.
Log `marker_clean.log`, SHA256 `D4017D2343CE61491EBF9007F3721D50EB3263029CE2A53A322226E04FA30361`.

Breakdown: marker 6, existing Random 9 (including frozen cross-host vectors),
existing Runtime.Input 5. New marker tests cover codec/closed grammar,
source/applied admission under absent/abstract/spawnable registry entries,
identity/name/world125, selected option metadata, no extra draws, runtime binary
round-trip/corruption, no phantom actor binding, no preview/runtime marker spawn,
unchanged legitimate native actor spawning, and actual SQLite edges. The index
test proves a marker token produces no key/edge/diagnostic while its profile and
zero-weight composite dependency remain present; removing the latter blocks root.

An intermediate new assertion wrongly counted root in SelectedDependencies;
corrected to empty, matching existing frozen semantics. No production change was
made to satisfy it. A subsequent all-pass run had two test-world teardown
warnings; the new fixture now creates/destroys its own FWorldContext and the
recorded focused result is clean.

## Reproduction

Run from the private host in PowerShell:

```powershell
& 'D:/PersonalProjects/UE5/UE_5.7/Engine/Build/BatchFiles/Build.bat' MimirCompositeV5S6Editor Win64 Development '-Project=E:/MimirComposite_GameObj_Audit_20260828/MimirCompositeV5S6.uproject' -NoEngineChanges -NoUBA -NoHotReloadFromIDE -ForceUnity -DisableAdaptiveUnity -NoPCH -NoSharedPCH -WaitMutex
& 'D:/PersonalProjects/UE5/UE_5.7/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' 'E:/MimirComposite_GameObj_Audit_20260828/MimirCompositeV5S6.uproject' -nullrhi -unattended -nop4 -nosplash -nosound -NoAssetRegistryCache -MHS6ParityHost '-MHGoldenRoot=E:/MimirComposite_V5S6_1_Direct_Source_20260828/golden' '-ExecCmds=Automation RunTests Mimir.V5.Composite.Marker+Mimir.V5.Random+Mimir.V5.Runtime.Input' '-TestExit=Automation Test Queue Empty' '-ReportExportPath=E:/MimirComposite_GameObj_Audit_20260828/MarkerCleanReport' '-abslog=E:/MimirComposite_GameObj_Audit_20260828/marker_clean.log' -stdout -FullStdOutLogOutput
```

Read JSON results, not process exit 0: TestExit can exit 0 even on failed tests.
Full BuildPlugin StrictIncludes / final unity gates and packaged smoke are not
claimed here; integration waits for the remaining owner questions.

## Full Automation

### Whole-suite D3D12: CRASH / gate not closed

The real D3D12 run used `-d3d12 -RenderOffscreen -MHPreviewRenderSmoke` and the
whole `Mimir` filter. The log contains 19 started tests and 18 completed tests.
The process crashed while `Mimir.V4.Material.TextureImportPersistence` was
running, before any V5/marker test had run:

```text
Assertion failed: MipView.GammaSpace == LayerData.SourceGammaSpace
TextureDerivedDataTask.cpp:422
Background Worker #18
```

The affected texture fixture was `/Game/MH/Generated/Textures/s2_persist_tex_n`.
The command runner returned exit 1; Engine requested exit status 3. No final
`MarkerFullReport/index.json` was exported. Log `marker_full.log`, SHA256
`58436063E46E0E95F873F20051643F509CE21EE91B4FA48D394C2F708822C80A`.
Crash evidence is in
`Saved/Crashes/UECC-Windows-9377A43C47A10CBD83FB39BBDBA2962A_0000/`.

Texture/material implementation and its test were not changed by this worker.
That fact does NOT establish the cause or prove a pre-existing baseline defect.
No Engine/texture code was changed to bypass the failure.

### Whole-suite NullRHI: 108 reported Success, one RHI lane NOT RUN

`MarkerFullNullReport/index.json`: 85 clean successes + 23 successes with warnings,
0 failed, 0 in-process; 108 total (Audit 5, V4 45, V5 58).
SHA256 `8BDE4C8479863CCAFF3D8D60BFB034B8340598502466B848019FE46FA46AFDED`.
Log `marker_full_null.log`, SHA256
`A279458B9E7EDAA279B141938772BC7DEFFD424A191EA442C10691032F5BA69E`.

Although Automation JSON says `notRun=0`,
`Mimir.Audit.MainBaseline.RenderedNativeHitProxy` explicitly reports:

```text
RHI lane NOT RUN: requires -MHPreviewRenderSmoke in the isolated host without -nullrhi
```

Therefore 108 reported successes are NOT claimed as 108 executed rendering
checks or as a closed full-RHI gate. That lane was run separately below.

### Targeted D3D12 PreviewDefects + Marker: 9/9

Filter `Mimir.Audit.MainBaseline+Mimir.V5.Composite.Marker`:
8 clean successes + 1 success with warning, 0 failed, 0 not-run.
All six new marker tests are clean. The sole warning is in the unchanged
`TopLevelGrouping` fixture: `UWorld::DestroyActor: World has no context!` during
teardown. It was not suppressed or reclassified.

`RenderedNativeHitProxy` genuinely executed four native HActor checks:
G=0, G=1, G=0, G=1, all `hit=HActor` at 734x339. No NOT RUN message in this lane.
`MarkerRhiReport/index.json` SHA256
`8DD92D12BFD86802D648F19E36C96CEBE108895E30F973198ABECFA21A9C8DB5`.
Log `marker_rhi.log` SHA256
`906203E64F63D522B57FC9E868631AF0AF76C2841589E70BD87B032439499D18`.

### Separate old-host texture check: 1/1 PASS, crash not reproduced

One exact `Mimir.V4.Material.TextureImportPersistence` D3D12 run used the prior
physical host `E:/MimirComposite_V5S6_0_R2_Host_20260828`. Its plugin is not a
junction; it has the old enum without Marker. No source, binary, build, or
configuration changes were made there. The Automation test wrote its normal
isolated fixture content/Saved data; the report and log are kept in this audit
directory. This is a single-test diagnostic, not a whole-suite baseline rerun.

Before and after the run, DLL SHA256 values were unchanged:

- Editor: `F79A784F515AD3B7A15EBC1C232673C748B1D47DA5B42F37DC3BA36BBA38F3A4`
- Runtime: `BE9F6A43B4A66A27F577B692FA83DECDD6B3B8BF0C3784C0ECB073FFC416F0A3`
- Tests: `CCCD91E204AA4FDC9DE8854E1940BE698687706F064CA6B5C659B081CC4F7270`

The old-host and current `MHMaterialProtocolTest.cpp` are byte-identical,
SHA256 `35CD9A9532A215140AFF289D9F177B635EA642A149BED528B51175ABC60DFED6`.
`BaselineTextureRhiReport/index.json`: 1 clean success, 0 failed/warnings/not-run,
SHA256 `94460F191D10481597043DDB0FFB40182D569C29E8E83031D5080898FD414B81`.
Log `baseline_texture_rhi.log`, SHA256
`91831036ECCC2A31B44D994FBCD09B25A3AC5B204CE19BF0BA0D14CC646D028A`.
The baseline crash was NOT reproduced. A single pass does not exclude timing or
whole-suite order; causality of the current full-RHI crash remains unknown.

### Exact additional run commands

```powershell
& 'D:/PersonalProjects/UE5/UE_5.7/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' 'E:/MimirComposite_GameObj_Audit_20260828/MimirCompositeV5S6.uproject' -d3d12 -RenderOffscreen -unattended -nop4 -nosplash -nosound -NoAssetRegistryCache -MHS6ParityHost -MHPreviewRenderSmoke '-MHGoldenRoot=E:/MimirComposite_V5S6_1_Direct_Source_20260828/golden' '-ExecCmds=Automation RunTests Mimir' '-TestExit=Automation Test Queue Empty' '-ReportExportPath=E:/MimirComposite_GameObj_Audit_20260828/MarkerFullReport' '-abslog=E:/MimirComposite_GameObj_Audit_20260828/marker_full.log' -stdout -FullStdOutLogOutput
& 'D:/PersonalProjects/UE5/UE_5.7/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' 'E:/MimirComposite_GameObj_Audit_20260828/MimirCompositeV5S6.uproject' -nullrhi -unattended -nop4 -nosplash -nosound -NoAssetRegistryCache -MHS6ParityHost '-MHGoldenRoot=E:/MimirComposite_V5S6_1_Direct_Source_20260828/golden' '-ExecCmds=Automation RunTests Mimir' '-TestExit=Automation Test Queue Empty' '-ReportExportPath=E:/MimirComposite_GameObj_Audit_20260828/MarkerFullNullReport' '-abslog=E:/MimirComposite_GameObj_Audit_20260828/marker_full_null.log' -stdout -FullStdOutLogOutput
& 'D:/PersonalProjects/UE5/UE_5.7/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' 'E:/MimirComposite_GameObj_Audit_20260828/MimirCompositeV5S6.uproject' -d3d12 -RenderOffscreen -unattended -nop4 -nosplash -nosound -NoAssetRegistryCache -MHS6ParityHost -MHPreviewRenderSmoke '-MHGoldenRoot=E:/MimirComposite_V5S6_1_Direct_Source_20260828/golden' '-ExecCmds=Automation RunTests Mimir.Audit.MainBaseline+Mimir.V5.Composite.Marker' '-TestExit=Automation Test Queue Empty' '-ReportExportPath=E:/MimirComposite_GameObj_Audit_20260828/MarkerRhiReport' '-abslog=E:/MimirComposite_GameObj_Audit_20260828/marker_rhi.log' -stdout -FullStdOutLogOutput
& 'D:/PersonalProjects/UE5/UE_5.7/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' 'E:/MimirComposite_V5S6_0_R2_Host_20260828/MimirCompositeV5S6.uproject' -d3d12 -RenderOffscreen -unattended -nop4 -nosplash -nosound -NoAssetRegistryCache -MHS6ParityHost '-MHGoldenRoot=E:/MimirComposite_V5S6_1_Direct_Source_20260828/golden' '-ExecCmds=Automation RunTests Mimir.V4.Material.TextureImportPersistence' '-TestExit=Automation Test Queue Empty' '-ReportExportPath=E:/MimirComposite_GameObj_Audit_20260828/BaselineTextureRhiReport' '-abslog=E:/MimirComposite_GameObj_Audit_20260828/baseline_texture_rhi.log' -stdout -FullStdOutLogOutput
```

All worker UE runs have exited. Full StrictIncludes/BuildPlugin, final force-unity,
packaged smoke, and whole V5-S6.1 acceptance remain unclaimed. OPEN-V5-22/23 and
the full-RHI crash are not bypassed by these narrower successes.
