# Composite Break / Undo — 2026-09-08

Status: READY FOR FIELD TEST. Branch `codex/composite-break-undo`, base `1b36160`.
Owner reported broken Undo and visible geometry without Outliner actors.

## Findings and implementation

Break already promoted top-level meshes and preserved nested composites, but
filtered out every resolved path containing `>`. A promoted child carries its
parent invocation namespace, including `>`, so a second Break produced no actors
and deleted that child. Layer filtering now strips the current placement's root
namespace first. Random option lookup converts that relative path back to the
child recipe's local path; selected layout and world transforms remain unchanged.

Undo of actor creation marks the actor garbage before post-undo callbacks.
`RemoveOwner` compared `Slot.Owner.Get()` with the actor address; weak Get is
already null at that point. Pooled instances consequently survived outside the
actor list. Cleanup now compares weak object index and serial identity.

The actor releases its nontransactional view in `PreEditUndo`, before UE applies
the transaction. Both native `PostEditUndo` overloads share restoration logic.
The general rebuild gate rejects invalid actors: Undo of creation does not call
`Destroyed()` or necessarily set `IsActorBeingDestroyed()`.

Native C++ evidence (stock UE 5.7.4, no Engine changes):

- `Editor/UnrealEd/Private/EditorTransaction.cpp`: PreEditUndo, garbage-state
  restoration, then annotated/nonannotated PostEditUndo dispatch.
- `Runtime/Engine/Private/ActorEditor.cpp`: AActor's two PostEditUndo overloads.
- `Runtime/CoreUObject/Private/UObject/Obj.cpp`: annotated UObject callback uses
  a qualified UObject::PostEditUndo call, bypassing derived no-argument overrides.

The shared level pool remains transient and hidden from Outliner. Visible
geometry is rebuilt from surviving placement records; pool state is not added
to transactions. Tooltip now describes one-layer Break.

## Validation

Own hosts only: `E:/temp/MH_CEI1_host_20260907` and
`E:/temp/MH_CEI1_package_20260907/HostProject`.
No portfolio assets or Engine files modified.

RED: `BREAK_RED_COMPLETE.log` / `BreakRedCompleteReport`: three regressions fail
against unchanged production; four existing Break tests pass.

- Repeated Break creates 0 actors instead of the expected selected mesh.
- Undo with an unrelated shared-bucket neighbor leaves 10 mesh instances instead
  of 8; Redo leaves 12 instead of 8. Three cycles reproduce the defect.
- Actual annotated Undo also leaves extra instances.

The earlier `BREAK_RED.log` captured the two primary regressions before production
edits. Annotation coverage was then added to the old-production staging host.
Two intermediate annotation fixture builds failed on UE API export/access;
the final fixture uses public `FindOrCreateTransactionAnnotation()` and real
editor transactions. These build failures are not regression evidence.

Tests inspect registered world mesh geometry as a multiset of mesh identity and
world matrix, count every ISM instance, and check membership in Level::Actors.
An unrelated placement shares the buckets throughout Undo/Redo. Existing
component bookkeeping assertions remain. No old tests removed; two tests added.

Completed checks (logs/reports under `E:/temp/MH_CEI1_host_20260907`):

| Check | Evidence | Result |
| --- | --- | --- |
| Guarded non-unity / no-PCH build | `BREAK_GUARDED.log` | Success, 79 actions, 367.25 s |
| StrictIncludes non-unity / no-PCH build | `BREAK_STRICT.log` | Success, 77 actions, 381.58 s |
| Force-unity / adaptive-off build | `BREAK_UNITY.log` | Success, 22 actions, 115.11 s |
| Focused Break + Pool | `BREAK_GREEN.log`, `BreakGreenReport` | 21/21 |
| Full Mimir + golden on guarded build | `BREAK_FULL.log`, `BreakFullReport` | 195 success + 102 warning = 297 passed; 0 failed |
| Full Mimir + golden on packaged StrictIncludes binaries | `BREAK_STRICT_FULL.log`, `BreakStrictFullReport` | 195 success + 102 warning = 297 passed; 0 failed |
| D3D12 / RenderOffscreen regressions | `BREAK_RHI.log`, `BreakRHIReport` | 39 success + 50 warning = 89 passed; 0 failed |
| PerfTrace before | `BREAK_BEFORE_VERIFIED.log`, `BreakBeforeVerifiedReport` | 2/2 |
| PerfTrace after | `BREAK_AFTER.log`, `BreakAfterReport` | 2/2 |

Both RecipeShadowParity and RecipeShadowParityApplied pass. RHI covers Break,
Pool, EditMode, Selection, Async, selected dependency persistence, save proof
warnings, recipe parity, and thumbnails. Warning outcomes include existing
isolated-world/expected diagnostic fixtures; they are counted as passes above.

PerfTrace: map load 0.158 → 0.350 ms; sync loads, endpoint lookups, admission,
compilation waits, and newly created components remain zero, two components
reused. Targeted reimport 141.531/116.597 → 212.152/160.902 ms; full scans,
recipe recompiles, and actor rebuild time remain zero, migrations 1/0. The
after run overlapped StrictIncludes compilation and the user's editor was open;
these durations are not a controlled timing comparison. No speedup or latency
regression conclusion is drawn from them. Counter invariants remain intact.
The first before attempt lacked `-MHGoldenRoot` and is superseded by the verified
before run listed above.

Documentation checker and `git diff --check` pass.

Package destination: `E:/temp/MH_break_20260908/`, with source/binary hashes and
Engine BuildId `47537391`. Portfolio installation is deferred while its editor
is running. The installer checks the archive and all manifest files, refuses
replacement while UnrealEditor is open, backs up the existing plugin outside
`Plugins`, and verifies the installed copy. It does not close the user's editor.

## Field check

1. Select a root composite and Break: standalone meshes/actors and intact child
   composites should occupy exactly the previous locations.
2. Break one promoted child: only its own layer is removed; random keeps the
   selected variant. Other siblings stay unchanged.
3. Undo and Redo several times. Geometry, Outliner objects and selection should
   agree; no doubling or leftover geometry should appear.
4. Keep a neighboring placement of the same composite visible during the test.

This change prevents new orphan instances. It does not scan a running editor
session to repair pool state already damaged by an older binary.
