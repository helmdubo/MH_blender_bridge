# Composite Break / Undo — 2026-09-08

Status: READY FOR FIELD TEST. Owner reports broken Undo and geometry without Outliner
actors. Break removes one composite layer. Branch `codex/composite-break-undo`,
base `1b36160` (thumbnail slice).

1. Break promotes this placement's direct entities: meshes become standalone
   StaticMeshActors, actor endpoints become actors, nested composites remain
   composite actors. Structural groups promote their descendants; random nodes
   contribute only their already selected option.
2. Repeating Break on a promoted composite removes its own layer, even when it
   carries an inherited stream namespace. Preserve selected geometry, world
   transforms, seeds, and appearance without resolving a different random choice.
3. One successful Break is one UE transaction. Undo restores the original
   placement and removes all promoted entities; Redo restores the split.
4. Transient pooled geometry follows actor lifetime through both native Undo
   callbacks. Removed actors cannot rematerialize. Cleanup uses owner identity
   even during the transaction's garbage-state transition.
5. Verify actual world geometry and instance counts through repeated Undo/Redo,
   including neighboring placements sharing pool buckets. Counts of components
   or owner lookups alone cannot establish absence of orphan instances.
6. Keep recipe/source data unchanged. No proof, source scans, mesh compilation
   waits, or Engine changes. Preserve the existing build and golden/parity gates.

Production scope: MHCompositeLevelSubsystem.cpp, MHCompositeActor.h/cpp,
MHInstancePool.cpp, MHSourceToolMenus.cpp. Tests: MHCompositeBreakTest.cpp.
Documentation: this contract, execution status, receipt.
