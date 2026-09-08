# Composite editor shutdown crash — 2026-09-08

Status: VERIFIED; INSTALLED FOR FIELD TEST. Branch `codex/fix-editor-shutdown`.
Includes the preceding SaveRotation fix. Owner authorized commit, push and merge
on 2026-09-08; integration is tracked in [PR #168](https://github.com/helmdubo/MH_blender_bridge/pull/168).

## Evidence and fix

Owner crash occurs on EditorExit, after `Object subsystem successfully closed`.
The installed DLL's `+0xf1a9` resolves through its matching PDB to
`FMimirCompositeEditorModule::ShutdownModule`, original line 250, at thumbnail
renderer unregistration. `UThumbnailManager::TryGet()` returns a raw singleton;
non-null does not prove it is alive after UObject teardown.

Independent RED reproduction on the own packaged host:
`UnrealEditor-Cmd.exe <project> -unattended -nullrhi -ExecCmds=QUIT_EDITOR`.
`SHUTDOWN_RED.log` crashes in the same module method at line 249 after object
shutdown; process exits 1. No portfolio map was needed to reproduce it.

Thumbnail registration now has explicit module ownership. PreExit releases
preview resources and unregisters the renderer once, while UObject is alive.
Dynamic unload retains an idempotent fallback guarded by `UObjectInitialized()`.
Late shutdown cannot iterate thumbnail UObjects or dereference the old manager.
Actor Details cleanup also moves to PreExit, with an alive-object guard on its
fallback because it resolves `AMHCompositeActor::StaticClass()`.

Only editor module cpp/h changed for this fix. Independent read-only review
found no blocking issues in normal exit, dynamic unload or commandlet paths.

## Verification

Own hosts: `E:/temp/MH_CEI1_host_20260907` and
`E:/temp/MH_CEI1_package_20260907/HostProject`. Evidence in the former directory.

| Check | Evidence | Result |
| --- | --- | --- |
| Guarded packaged-host StrictIncludes-equivalent non-unity/no-PCH | `SHUTDOWN_STRICT.log` | 4 actions, succeeded |
| Guarded force-unity/adaptive-off/no-PCH | `SHUTDOWN_UNITY.log` | 4 actions, succeeded |
| Full NullRHI/golden followed by normal exit | `SHUTDOWN_FULL.log`, `ShutdownFullReport` | 293/293 passed, exit 0 |
| D3D12/RenderOffscreen thumbnails + two perf probes, normal exit | `SHUTDOWN_RHI.log`, `ShutdownRhiReport` | 6/6 passed, exit 0 |

Both RecipeShadowParity tests and SaveRotation regression remain green. RHI
covers renderer registration, selected nested layout, cold endpoint retry and
actual geometry thumbnail rendering/cache. Both process logs continue through
object teardown to `LogExit: Exiting.` and `Log file closed`, with no fatal error.

Crucial test-runner distinction: `-TestExit="Automation Test Queue Empty"` uses
forced process termination and did not exercise the failing shutdown phase.
These runs instead use `-ExecCmds="Automation RunTests <filter>,Automation
SoftQuit"` without TestExit. The native SoftQuit waits for queued tests and
requests a non-forced exit. This normal-exit check is the regression evidence;
an automation result alone does not prove shutdown succeeded.

## Installation

Installed with the editor closed into
`D:/PersonalProjects/UE5/MimirHead_portfolio 5.7/Plugins/MimirComposite`.
245 files hash-verified, BuildId `47537391`; includes the editor PDB to make
future crash stacks readable. Source files match the tested packaged host.

Backup:
`D:/PersonalProjects/UE5/MimirHead_portfolio 5.7/PluginBackups/MimirComposite_before_ShutdownFix_20260908_203654`.
Archive:
`E:/temp/MH_Shutdown_20260908/MimirComposite_ShutdownFix_UE5.7.4_Win64.zip`.
SHA-256: `ee9b9e68f161d93ed5f108013e248ffbe377359ac4522ca5e3c042321043c18a`.
Installation receipt: `E:/temp/MH_Shutdown_20260908/installation.json`.

Portfolio scene/source assets were not changed. Field check: open the scene,
browse composite thumbnails, then close the scene/editor normally.
