# Composite thumbnails — 2026-09-07

Status: READY FOR FIELD TEST. Owner requests native Packed Level Blueprint
Content Browser thumbnails for composites, with one selected random option.
Branch `codex/composite-thumbnails`, base `7f89ac0`.

1. Register a native UDefaultSizedThumbnailRenderer for UMHCompositeAsset.
2. Build an isolated FThumbnailPreviewScene from the recipe preview plan;
   use existing layout/appearance resolver with fixed seeds 0/0. Each random
   node contributes only its selected option. Nested selected composites
   contribute their contents; empty selections remain empty.
3. Render mesh leaves with their transforms, materials and per-instance
   appearance data. Group matching meshes into ISM components. Frame aggregate
   bounds using native thumbnail lighting/floor/orbit defaults.
4. Request only selected mesh endpoints asynchronously. Never wait for mesh
   compilation, build proof, read source files, or spawn level placements.
   Pending thumbnails retry through the native thumbnail-dirtied event.
5. Native package thumbnail serialization remains responsible for persistence.
   A save before dependencies settle may store the engine's empty thumbnail;
   completion refreshes the browser, the next normal save stores the ready image.
6. Invalidate observed thumbnails on recipe/resource change. Bound resident
   ready cache; release scenes, references and callbacks on shutdown/GC.
7. Validate deterministic random selection, nested transforms, async retry,
   raster output and existing full Mimir/golden/build/performance gates.

This is a mesh thumbnail view, not a new actor execution backend. Actor/gameobj
endpoints without mesh leaves and all-empty recipes keep the generic icon.
No wire format, resolver, placement, Edit Mode, or Engine changes.

Production files: MHCompositeThumbnailRenderer.h/cpp,
MimirCompositeEditorModule.cpp, MHCompositePlacementEvents.cpp.
Tests: MHCompositePlacementTest.cpp (replace disabled expectation),
MHCompositeThumbnailTest.cpp. Documentation: this contract, status, research receipt.
