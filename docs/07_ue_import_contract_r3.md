# 07 — UE Import Contract for Clean Sources v2 (ACTIVE)

Статус: контракт этапа C для `MimirComposite`, UE 5.7.4 (целевой compatibility
5.8). Имя `r3` историческое. Старый manifest resolver/watcher, optional passport,
Source Schema v1 runtime и общий local index полностью superseded.

> **ADR v3 supersede notice.** `ADR_V3_interchange_hybrid.md` supersede-ит:
> §2 в части «Ledger = applied authority», §4 в части «diff против Ledger»,
> §12 в части «Interchange вне C-scope». C1 (PR #7) завершается по тексту
> ниже; первый post-C1 slice — C2.0 по ADR v3 §9. Остальные разделы 07
> действуют без изменений.

UE является **reader**: startup/watcher сканирует primary payloads и сравнивает
их с Ledger. Blender explicit writer всегда пишет и не сообщает diff.

Модули: `MimirCompositeRuntime`, `MimirCompositeEditor`,
`MimirCompositeTests`.

## 1. Reader transaction and batch

Source snapshot состоит из валидированных `*.mesh.fbx`, `*.material`,
`*.composite` под `source_root`. Manifest/marker не сканируются.

Startup scan default — silent auto-import. Optional setting `prompt` показывает
plan до Execute. Watcher использует тот же pipeline для изменившихся payloads.

```text
Scan/StableBytes -> Parse Identity -> Resolve -> Analyze vs Ledger -> Plan
-> Textures -> Materials -> Geometry -> Composites -> Finalize
-> LedgerCommit -> Report
```

Batch не является транзакцией UE packages: `MH_E_*` блокирует свой resource,
остальные продолжаются. Ledger обновляется только для успешных. `MH_W_*` не
блокирует.

## 2. Runtime/editor classes

**`UMHCompositeAsset`** (Runtime): CompositeUid, Name, ResourceProperties,
TArray nodes `{NodeUid, ParentUid, Kind, DisplayName, ResourceUid,
LocalTransform, PropertyBag}`, SourceJsonSnapshot, AssetImportData. Node хранит
ResourceUID authority и resolved FSoftObjectPath.

**`UMHImportLedger`** (Editor, `<content_root>/_MH/Ledger`):

```text
ResourceUID -> {
  Kind, Asset, SourcePath,
  AppliedGeometryHash, AppliedDescriptorHash,
  PayloadFingerprint, ImportedAt, ImportStatus
}
```

Для materials/composites semantic hashes вычисляются из canonical payload.
Ledger — производный reader state вне source tree. Потеря запускает full startup
scan/import; source data не теряются.

> ADR v3: начиная с C2.0 Ledger — только derived index/dashboard; applied
> authority — `UInterchangeAssetImportData` внутри каждого ассета (ADR v3 §§2–4).

**`UMHSourceImporter`** (EditorSubsystem) — публичный вход
`ImportSources(Scope)`. Стадии приватные; частичный import выражается Plan,
а не отдельными обходными API.

## 3. Resolver and conflicts

`IMHSourceResolver::Resolve(uid)` возвращает candidate payload paths и
resolved/conflict status. Реализация читает embedded identity:

- FBX — mandatory Carrier B `mh.fbx_passport` byte consensus;
- material — `mh.material:1` self-identity;
- composite — только `mh.composite:2` self-identity. Composite v1 не является
  runtime candidate; migrator отмечает его
  `MH_W_LEGACY_COMPOSITE_V1_MIGRATION_REQUIRED`.

Missing/malformed/unknown passport quarantines FBX с
`MH_E_PASSPORT_INVALID`. Manifest fallback и warning-only passport отсутствуют.

Conflict matrix:

| Source state | UE action |
|---|---|
| New valid UID | auto-import silently by default; prompt optional |
| Ledger old path gone, one new candidate same UID | MOVE + log |
| Multiple candidates, equal fingerprint | import one semantic resource, `MH_W_DUPLICATE_IDENTICAL_PAYLOAD`, log all paths |
| Multiple candidates, divergent fingerprint | `MH_E_DIVERGENT_REVISIONS`, manual choice |
| No valid candidate for required edge | `MH_E_RESOURCE_NOT_FOUND`, unresolved node/resource |
| Missing/malformed identity | quarantine |

Mtime never selects a revision. Reader confirms bytes after per-file stability.

Canonical C++ library (NFC, canonical JSON, UUID/path, XXH3, descriptor hash)
must pass `golden/canonical_vectors.json` at C0.

## 4. Analyzer, diff and report

Diff is entirely reader-side. UE compares current embedded semantic state with
Ledger:

- static mesh geometry hash changed → `UPDATE_GEOMETRY`;
- descriptor changed only → resource/property/material-slot update;
- source path changed with same UID → `MOVE`;
- canonical material/composite changed → `UPDATE_PROPERTIES`/
  `UPDATE_RESOURCE` as appropriate;
- hashes equal after explicit source rewrite → `NO_CHANGE`.

Payload fingerprint is a stability/external-modification signal, not semantic
identity. Changed fingerprint with equal semantic hashes does not reimport the
asset. Reader reports `MH_W_PAYLOAD_EXTERNAL_MODIFIED`; before accepting or
overwriting an unaccounted revision it raises
`MH_E_EXTERNAL_MODIFICATION_CONFIRMATION_REQUIRED`. Ledger is not silently
advanced past that confirmation gate.

`mh.diff_report` keeps CREATE/REMOVE/RENAME/MOVE/UPDATE_GEOMETRY/
UPDATE_TRANSFORM/UPDATE_PROPERTIES/REPARENT/UPDATE_RESOURCE/UPDATE_KIND/
EXTERNAL_UNRESOLVED. UE-only `LOCAL_EDIT`/`CONFLICT` remain outside parity.
`tools/diff_bundles.py` is an independent payload/passport reader; parity gate
uses the same fixtures, never manifests.

Report goes to Message Log category Mimir and JSON under Saved/ for CI.

Reader snapshot diagnostics are fail-closed:

- `MH_E_SOURCE_INDEX_INVALID` — ephemeral scan/index cannot be interpreted;
- `MH_E_SOURCE_INDEX_PATH_OUTSIDE_ROOT` — candidate escapes `source_root`;
- `MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED` — source changed between Plan and Apply.

These names describe reader snapshots, not a shared/persistent Blender writer
index and do not create a Rebuild UX.

## 5. Textures

Texture identity remains path-based. Resolve:

```text
exact normalized path -> unique basename under texture_root -> unresolved
```

Unique basename actualizes `.material`; multiple matches produce
`MH_W_TEXTURE_BASENAME_AMBIGUOUS`. Commandlet
`-run=MHActualizeTexturePaths` emits fixed/ambiguous/missing.

Internal texture maps to `<content_root>/<relative-path>/T_<basename>`.
External absolute in transitional policy warns and maps under
`<content_root>/_External/<hash>/`; strict policy blocks. Suffix policy:
diffuse `_d|_tex_d` sRGB on, normal `_n|_tex_n` sRGB off/Normalmap, masks
project-configured. Reimport fast-path may use mtime/size, confirmed by bytes
when needed.

## 6. Materials and three-way state

Master resolution: `shader_class -> <master_root>/<shader_class>` plus aliases.
Missing master gives `MH_E_MASTER_MATERIAL_NOT_FOUND` for that material.
Material Instance path mirrors source, prefix `MI_`. Scalar/vector/texN map to
Master parameters; unknown parameter warns.

Three-way comparison:

```text
base   = applied material semantic hash in Ledger
theirs = current .material canonical hash
ours   = canonical hash of current MI parameters
```

- theirs != base and ours == base → apply source update;
- theirs == base and ours != base → `LOCAL_EDIT`, do not overwrite MI;
- both differ → `CONFLICT`, apply configured prompt/overwrite/keep policy.

### 6.1 Export Material to Source

Explicit force operation dumps MI parameters to `.material`, shows line diff and
warning when source has unapplied changes, then writes temporary + atomic
replace. No manifest/registry/Ledger transaction. After successful write the
normal watcher reader observes payload and commits Ledger after import.

Per-target lock timeout gives `MH_E_PAYLOAD_LOCK_TIMEOUT` with zero source
writes. Referenced missing material uses the existing
`MH_W_MATERIAL_NOT_FOUND`; spelling `MH_W_MISSING_MATERIAL` is not introduced.

Non-roundtrippable static switches/parent/unknown schema block with
`MH_E_MATERIAL_NOT_ROUNDTRIPPABLE`. Every writeback is logged.

## 7. Geometry — `FMHFbxBackend`

Direct FBX SDK backend, without `UFbxFactory`:

- validate mandatory Carrier B before mapping;
- use declared axis/units and project conversion; mismatch →
  `MH_E_GEOMETRY_SOURCE_MISMATCH`;
- render mesh nodes → FMeshDescription with positions, polygons, per-face
  material index, normals/smoothing, UV and color attributes;
- `UCX_*` → collision, `SOCKET_*` → sockets;
- other nodes warn/ignore;
- slots come from passport `material_slots`, MaterialUID resolves through
  current Plan/Ledger; FBX polygon groups are cross-check only.

### 7.1 Combined-LOD

One FBX contains all levels. Mapper groups mesh nodes by integer
`mh_lod_level` (`absent` → 0 only) and builds
`SourceModel[N].MeshDescription` in one pass. Names/`.lodNN` suffixes are not
read.

- actual levels must equal passport `lod_levels`;
- levels must be dense `0..N`;
- LOD1+ slots subset LOD0;
- UCX/SOCKET belong to LOD0; authored on LOD1+ warns/ignores;
- malformed level blocks whole resource;
- authored screen sizes ROADMAP, current auto-compute.

Reimport updates the same UStaticMesh through MeshDescription commit/build;
recreating the asset is prohibited.

### 7.2 `mh.fbxdump`

`-run=MHFbxDump <file> [--full]` prints canonical passport, Model graph,
per-node `mh_lod_level`, level summary, slots, axis/units and counts. Golden
dumps define mapper fixtures. Tag `mh.fbxdump:1` may bump when dump format
changes; it is not source schema.

Legacy `FLegacyFbxBackend` may remain a test comparison backend only. It cannot
accept missing passport or become production manifest fallback.

## 8. Composites

Factory accepts only `mh.composite` schema_version 2. Top-level properties map
to asset ResourceProperties; node properties remain placement-level.

Dependencies are computed from `nodes[].resource_uid`; topological plan imports
children before parent. Cycle → `MH_E_COMPOSITE_CYCLE`. Missing resource keeps
an unresolved node and reports it; graph is never inferred from FBX names.

Reimport updates the same `UMHCompositeAsset`.

## 9. Finalize

Finalize rules operate only on assets in the current successful Plan. Current
examples: `role=decal`, Lumen Mesh Cards, UCX cleanup. Legacy name suffix may be
diagnostic fallback only where separately approved; it never determines source
identity.

## 10. Startup, watcher and commandlets

- Startup starts after Asset Registry files load.
- It scans three primary extensions under `source_root`.
- `startup_scan_mode=silent` is default; `prompt` is optional.
- Watcher observes primary payload changes, debounce >= 1 s, queues during PIE.
- Both paths call the same resolver/analyzer/Plan pipeline.
- `-run=MHImportSources` uses headless policy and nonzero exit on errors.
- `-run=MHVerifyMaterials` performs Analyze-only ours/theirs/base report.
- `-run=MHActualizeTexturePaths` repairs only unique basename matches.

There is no watcher on `export_manifest.json`, no registry generator and no
artist Rebuild index action.

## 11. Settings (`UDeveloperSettings`)

- `source_root` required;
- `content_root` default `/Game/MH`;
- `master_root` and shader aliases;
- asset prefixes `SM_/MI_/T_/CA_`;
- `texture_root` default `source_root`;
- `texture_policy` default transitional;
- `conflict_policy` default prompt for MI three-way conflicts;
- `startup_scan_mode` default **silent**, optional prompt;
- `lumen_cards_max` default 32;
- production `geometry_backend=mh_fbx`.

Ledger location is editor-owned and not configurable into source tree.

## 12. Out of C scope

Composite actor/compiler/ActorFactory/Rebuild (stage D), reserved node kinds,
ISM/LI, Interchange/USD, skeletal, authored LOD screen distances, Export
Selection closure. Source write is read-only except explicit Material writeback.

> ADR v3: исключение Interchange из C-scope superseded — Interchange входит в
> C2 начиная с под-gate C2.0 (ADR v3 §§5,9). USD/skeletal остаются вне scope.

Manifest resolver, optional passport, per-file LOD, shared local index and
registry generation are not roadmap: they are superseded architecture.

## 13. Gates

- **C0:** 5.7.4 skeleton; canonical vectors; mandatory Carrier B fbxdump;
  axis/geometry mapping measurements. External audit.
- **C1:** payload codecs + resolver + Ledger analyzer commandlet-testable;
  parity against payload/passport golden M8/M9/M10; startup silent/prompt;
  duplicate/divergent matrix. External audit.
- **C2:** factories/builders; Combined-LOD including multi-object LOD1 and
  UCX/SOCKET; edit LOD1 → one `UPDATE_GEOMETRY`; explicit no-op source rewrite
  → `NO_CHANGE`; material-only update; MI LOCAL_EDIT/CONFLICT; reimport same
  objects. External audit.
- **C3:** watcher, startup, Import/Verify/Actualize commandlets, material
  writeback; crash/stability/duplicate events. External audit.
- **C4:** owner field acceptance on real source root: Blender explicit export →
  startup/watcher auto-import → Combined-LOD/material/composite updates →
  LOCAL_EDIT/CONFLICT/writeback → same UE assets.
