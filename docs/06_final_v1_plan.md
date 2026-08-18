# 06 — MH Source Protocol v2 implementation plan

Статус: active execution plan Clean Sources v2. Имя файла historical. Прежний
Final v1 plan и ранний v2 план с hash-skip/shared index superseded. Writer и
reader разводятся жёстко:

```text
Blender Export: collision guard -> tmp -> atomic rename -> exit
UE read: startup/watcher scan -> compare with Ledger -> import/diff
```

Каждый explicit Export всегда пишет requested payload(s). Writer не делает
hash-skip, diff, source-root scan, cache/Ledger read или index update. Крупные
gates принимает внешний аудитор; field acceptance выполняет owner.

## V2-G0 — Documentation freeze

Deliverables:

1. Clean Sources v2 — единственный active contract.
2. Sterile tree, clean filenames, mandatory embedded identity/passports.
3. Combined-LOD one-resource/one-FBX и `mh.meshser:2`.
4. Stateless always-write Blender exporters.
5. Blender lazy cache только для Import Composite.
6. UE startup/watcher Ledger diff; silent auto-import default, prompt optional.
7. Frozen v1 и legacy manifests — migration-only.

Gate: JSON examples/Markdown fences/cross-links валидны; legacy terms встречаются
только в migration/historical context; внешний аудитор принимает doc commit.

## V2-G1 — Carrier B transport proof (blocking)

Матрица реальных FBX:

- один/несколько objects, shared datablock, custom normals;
- длинная canonical passport JSON;
- single/Combined-LOD, multi-object LOD1, UCX/SOCKET;
- оба Blender FBX paths, UE `FMHFbxBackend`, Blender import→re-export;
- pivots/hierarchy/transforms до/после.

Gate: `mh.fbx_passport` есть на каждой Model node; copies byte-identical;
malformed/missing/mismatch даёт `MH_E_PASSPORT_INVALID`; carrier не портит
payload. Fallback carrier требует нового owner/reviewer decision.

## V2-G2 — Stateless explicit writers

### G2.1 Common publication

- strict passport/material/composite codecs;
- canonical JSON/NFC/path primitives и `mh.meshser:2`;
- clean-name sanitizer;
- target-only collision guard;
- temporary sibling + flush/close + atomic replace + per-file lock;
- никакого manifest, source scan, hash-skip, diff, Ledger/cache/index update.

Writer computes `geometry_hash` for passport. `descriptor_hash` and semantic
comparison belong to readers.

### G2.2 Material writer

- каждый Export пишет requested
  `<Directory>/<sanitized_name>.material`;
- target same UID → replace, different UID →
  `MH_E_NAME_COLLISION_DIFFERENT_UID`;
- `rendinst_simple` fallback;
- texture paths без copies.

### G2.3 FBX writer

- каждый Export пишет requested `<Directory>/<sanitized_name>.mesh.fbx`;
- mandatory Carrier B;
- Combined-LOD одним file/call;
- `mh.meshser:2` покрывает LOD и UCX/SOCKET;
- Export Materials ON всегда пишет затронутые materials в requested Directory;
  OFF даёт zero material writes;
- Blender state восстанавливается в `finally`.

### G2.4 Composite writer

- каждый Export пишет requested `<sanitized_name>.composite` v2;
- top-level resource properties;
- authoring graph validation/cycle check;
- disk dependencies не сканируются; missing resources обнаруживает reader.

### G2.5 Gates

- source output содержит только primary extensions;
- повторный semantic no-op физически переписывает target;
- writer report не содержит diff/skip claim;
- target collision разных UID даёт zero writes;
- export того же UID в другой folder разрешён;
- ON/OFF material writes и отсутствие texture copies;
- packaged ZIP, Blender integration и внешний audit green.

После G2 UE начинает читать v2. До G2 он не создаёт новый manifest/per-file LOD
runtime.

## V2-G3 — Blender Import Composite resolver

Deliverables:

1. Первый Import Composite молча сканирует primary payloads под `source_root`.
2. Optional lazy cache существует только для этого importer.
3. Cache miss/stale автоматически вызывает scan; artist-facing Rebuild нет.
4. UID conflict matrix, Fork и unresolved placeholders.
5. Recursive child composites/FBX/materials always ON.
6. Resolve Missing наполняет те же Collections/Empties.
7. Texture exact→basename actualization.

Gate:

- import работает с отсутствующим/удалённым cache без user action;
- writer никогда не читает/обновляет cache;
- identical duplicates логируются, divergent revisions блокируются;
- missing/malformed passport quarantined;
- semantic import→re-export round-trip;
- M8/M9/M10 Blender fixtures и внешний audit receipt.

## V2-G4 — UE startup/watcher Ledger reader

Deliverables:

1. Startup full scan primary payloads.
2. Default silent auto-import; optional project preference prompt.
3. Watcher per-file stability и тот же comparison path.
4. Ledger хранит предыдущие semantic hashes/fingerprint/path/import result вне
   source tree.
5. Reader-side classifications: `NO_CHANGE`, geometry/properties/move/conflict.
6. Batch order:

```text
textures -> materials -> geometry -> composites -> finalize -> ledger
```

Gate:

- explicit no-op export переписывает file, UE даёт `NO_CHANGE` и не пересобирает
  asset;
- LOD1/UCX edit даёт один `UPDATE_GEOMETRY` combined resource;
- metadata-only passport/material change даёт соответствующий reader diff;
- new valid UID auto-imports silently by default; prompt mode asks;
- MOVE only when old path gone/new unique;
- identical duplicate logged; divergent UID revision requires manual choice;
- existing UE asset reimports in place.

## V2-G5 — Migration-only legacy reader

- full no-write preflight frozen v1 tree;
- uid8 → clean names with collision report;
- manifest mesh metadata → FBX passport;
- composite v1/resource properties → composite v2;
- per-file LOD → Combined-LOD re-export same UID;
- manifests removed only after payload validation;
- structured receipt, exact backup, conditional restore;
- production Blender/UE packages do not import legacy codec.

Gate: success fixture плюс pending/collision/escape/missing/non-file/ambiguous
zero-write refusals; restore только при неизменённом prepared state; migrated
tree импортируется без manifest.

## V2-G6 — Crash/concurrency

Writer cases: crash before/after replace; two writers same target; target UID
changes between guard and replace; Blender state rollback. Expected payload is
old or new valid bytes, never partial.

Reader cases: startup while files change; watcher duplicate events; stale
Blender lazy cache; stale/deleted UE Ledger; sync-folder duplicates. Reader
revalidates current bytes and never resolves divergent revision by mtime.

## UE-C2+ — Asset/composite vertical slice

Combined-LOD case: edit LOD1 → explicit export rewrites single FBX → startup/
watcher compares against Ledger → one `UPDATE_GEOMETRY` → all SourceModels
rebuild in place. A subsequent no-op export rewrites source file but yields
`NO_CHANGE`.

Full slice: Blender composite export → UE startup/watcher imports assets → actor
placement → Blender transform/geometry/material edit → explicit export → reader
diff → reimport/recompile same UE objects.

## Final owner acceptance

1. Source tree sterile, clean filenames without UID suffix.
2. Every Export visibly rewrites requested payload; no skip/index status in UX.
3. FBX Export Materials ON/OFF and no texture copies.
4. Combined-LOD plus UCX/SOCKET reader diff.
5. Import Composite first-use silent scan and recursive Resolve Missing.
6. UE startup silent auto-import; optional prompt mode.
7. Watcher/Ledger no-op versus semantic changes.
8. Duplicate/divergent conflicts without mtime winner.

Only owner field receipt completes vertical v2.

## ROADMAP

- Structure only import;
- authored LOD screen distances;
- Export Selection closure for external transfer;
- texture mirroring/copy policy.

ROADMAP cannot reintroduce writer diff/index/manifest authority without a new
normative decision.
