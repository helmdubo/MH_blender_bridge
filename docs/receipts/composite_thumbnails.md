# Composite thumbnails — research and execution receipt

Execution history, not normative. 2026-09-07, branch
`codex/composite-thumbnails`, base `7f89ac0`. Field acceptance pending.

## Native UE 5.7.4 C++ mechanism

Source root: `D:/PersonalProjects/UE5/UE_5.7/Engine/`.

- `Config/BaseEditor.ini:42` registers ordinary UBlueprintThumbnailRenderer
  for Blueprint assets; Packed Level Blueprint uses the same route.
- `Source/Editor/UnrealEd/Private/ThumbnailRendering/BlueprintThumbnailRenderer.cpp:17`
  checks generated actor components; `:49` gets a cached preview scene, creates
  a view family with game flags, disables advanced effects/motion blur, renders.
- `Source/Editor/UnrealEd/Private/ThumbnailHelpers.cpp:1096` creates a transient
  actor in a separate preview world. `:1147` accepts UStaticMeshComponent,
  including ISM/HISM; `:1177` collects bounds; `:1198` frames the scene with
  sphere radius * 1.15 / tan(FOV/2), native orbit defaults, ground offset.
- `Source/Runtime/Engine/Private/PackedLevelActor/PackedLevelActorISMBuilder.cpp:69`
  constructs packed ISM/HISM; `PackedLevelActorBuilder.cpp:473` places packed
  components/instance data in the Blueprint construction script. The thumbnail
  does not unpack the source level or enter Edit Contents.
- `Source/Editor/UnrealEd/Private/UnrealEdSrv.cpp:452` generates asset thumbnails
  on save; `ObjectTools.cpp:5711` renders and caches FObjectThumbnail in the
  package thumbnail map; `Source/Runtime/CoreUObject/Private/UObject/SavePackage/SavePackageUtilities.cpp:1305`
  serializes the thumbnail table. This is separate from mesh/material DDC.

## Async seam verified in source

`AssetThumbnail.cpp:2234` checks compilation on the asset itself, not mesh
dependencies. CanVisualizeAsset=false is not an automatic retry signal.
`AssetThumbnail.cpp:1427` and `:1591` show that a failed render only hides the
image; its entry/subscriptions remain. `:2641` queues a thumbnail again after
the manager's thumbnail-dirtied notification. In UE 5.7 DECLARE_EVENT permits
direct Broadcast (`Runtime/Core/Public/Delegates/Delegate.h:219`).

The renderer therefore waits asynchronously outside Draw and dirties only
the thumbnail after readiness; it does not synthesize property-change events
(which would also invalidate recipes). No new IInterface_AsyncCompilation
implementation is added: its global compilation-manager contract is unrelated
to thumbnail dependency loading.

CanVisualizeAsset=false prevents generating a black raster while pending.
Stock save may cache an empty thumbnail in that case (`UnrealEdSrv.cpp:474`);
the ready image is persisted by a later ordinary save, without a forced save.

## Implementation and validation

Existing test `ThumbnailRenderingDisabled` was explicitly superseded by the
owner's request. Renamed replacement `Thumbnail.RendererRegistered` failed
before implementation: `THUMBNAIL_RED_BUILD.log` SUCCESS,
`ThumbnailRedReport/index.json`: 0 passed, 1 failed. Baseline PerfTrace:
`ThumbnailBeforeReport`, 2/2. Artifacts under `E:/temp/MH_CEI1_host_20260907/`.

The first RHI focus run proved actual geometry pixels and cold retry; its
package-table lookup exposed a fixture error: saving under /Temp changes the
implicit package path in the thumbnail table. The fixture now saves at its
canonical isolated-host path and calls the native save thumbnail generator.
The corrected RHI suite passed 68/68 before the final async hardening below.

Read-only review found that endpoint pool admission reads mesh render data and
may wait for async properties *before* the caller checks IsCompiling. Thumbnails
therefore own selected-only streamable handles and use the existing pure
MHAdmitEndpointIdentity after IsCompiling is false. They never enter the pool's
interface-hash/material-usage admission (including its load callback). The pool
registry itself and placement behavior are unchanged. Failed loads are cached
until resource invalidation, ready mesh references are bounded with the cache.

Final source verification (UE5.7.4 CL51494982 / BuildId47537391):

- StrictIncludes non-unity/no-PCH: `THUMBNAIL_STRICT_ASYNC.log` SUCCESS,
  9 actions, 41.98 s.
- Guarded ownhost non-unity/no-PCH: `THUMBNAIL_GUARDED_ASYNC.log` SUCCESS,
  9 actions, 39.15 s.
- D3D12 offscreen: `ThumbnailRHIFinalReport/index.json`, **68 passed, 0 failed**
  (26 success + 42 success-with-warnings). Real raster differs from hidden
  geometry background, native save generator returns an image, thumbnail
  table loads from the saved package. PNG visually inspected:
  `E:/temp/MH_CEI1_package_20260907/HostProject/Saved/Automation/CompositeThumbnail.png`.
- Cold random fixture saves two real meshes and unloads both: only the selected
  one is requested/loaded; completion sends one thumbnail-dirtied event and
  no property event. Child notification updates the parent thumbnail transform.

The first full NullRHI run exposed another fixture assumption: stock
UThumbnailManager::RegisterCustomRenderer deliberately leaves Renderer null
when FApp::CanEverRender is false (`ThumbnailManager.cpp:238`). The preparation
test now instantiates the renderer directly only in that headless case; the
RHI lane still requires the actual native registration. Production unchanged.

- Final test-only correction compiled under strict non-unity/no-PCH
  (`THUMBNAIL_STRICT_VERIFIED.log`, 26.04 s), guarded non-unity/no-PCH
  (`THUMBNAIL_GUARDED_VERIFIED.log`, 24.76 s) and force-unity/adaptive-off
  (`THUMBNAIL_UNITY_VERIFIED.log`, 44.66 s): SUCCESS.
- Full NullRHI Mimir/golden: `ThumbnailFullVerifiedReport/index.json`,
  **295 passed, 0 failed** (195 success + 100 success-with-warnings), including
  both RecipeShadowParity checks. Count increased by 3; disabled expectation
  replaced, no acceptance coverage removed.
- RHI thumbnail tests after the headless fixture correction:
  `ThumbnailRHIHeadlessFixReport/index.json`, **4 passed, 0 failed**.

- Before/after PerfTrace: `ThumbnailBeforeReport` / `ThumbnailAfterReport`,
  **2/2** each. Map load 0.184 → 0.137 ms; sync package loads, registry lookups,
  identity admissions, compilation waits and components created remain 0;
  reused components remain 2. Reimport 219.896/196.871 → 143.694/132.517 ms;
  full scans, recipe recompiles and actor rebuild ms remain 0, migrations 1/0.
  Timing is not a benchmark or a speedup claim (baseline overlapped a build);
  the acceptance evidence is unchanged work counters and passing fixtures.
- Normative docs checker and `git diff --check`: PASS.

Status: READY FOR FIELD TEST. No manual owner acceptance claimed.

The existing importer already uses UEditorLoadingAndSavingUtils::SavePackages
(`MHSourceImportBatch.cpp:101`), so ready imported composites participate in
the native save-thumbnail path without another importer/persistence backend.

Package, manifest and installation receipt: `E:/temp/MH_thumbnails_20260907/`.
