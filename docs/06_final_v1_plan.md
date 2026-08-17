# 06 — Final Source Schema v1: execution plan

Статус: актуальный план для замороженной Source Schema v1 с совместимым
post-freeze operational/UX amendment D38 `[fbx-export-materials-toggle]`.
Amendment не меняет JSON/canonical bytes и не переоткрывает G0. План заменяет
Blender-последовательность из `02_mvp_plan.md`. Нельзя начинать следующий gate до
приёмки предыдущего. Крупные срезы проверяет внешний аудитор; field acceptance
в Blender выполняет owner.

## G0 — Schema package и FINAL v1 FREEZE

Scope: только contracts/docs, без Blender implementation.

Статус: **FINAL v1 FROZEN**. После freeze разрешены wording и operational/UX
уточнения без изменения on-disk bytes; любое byte-significant изменение создаёт
новую schema version.

Deliverables:

1. Финальная схема `mh.export_manifest` v1: только `resources[]`; kinds
   `static_mesh`, `composite`, `material`; `material_slots` только у mesh;
   `source` относительно owning manifest.
2. Финальная схема `mh.material` v1 и canonical hash rules; подтверждение
   неизменённой `mh.composite` v1 как единственного источника node graph.
3. Decision Log: D1 в форме «`.composite` задаёт граф, resolver находит payload
   по UID, FBX только наполняет target Collection»; свободные repository IDs
   для `uid-source-resolution` и `material-source-files`; правило «номера
   присваивает repo, директивы ссылаются на decision slug».
4. Нормативные resolver, texture-policy, unique-owner, cycle и diagnostics
   contracts из `04_source_workflows.md`.
5. Реестр кодов с инвариантой `MH_E_*` blocks / `MH_W_*` never blocks.
6. Формы и expected outcomes M8/M9 зафиксированы нормативно; физические
   golden/canonical artifacts регенерируются в G1/G4 без legacy-reader.

Gate:

- все JSON-примеры документа парсятся, а schema/path/hash rules проходят
  byte-level review;
- в нормативных документах нет двух описаний одного v1 поля;
- внешний аудитор принимает doc commit как **FINAL v1 FREEZE**;
- после gate любое byte-significant изменение — только `schema_version = 2`.

Pre-freeze artifacts не получают production legacy-reader. При необходимости
разрешён отдельный одноразовый migration script для локальных тестовых файлов.

## G1 — Pure codecs, dependency analyzer и resolver

Scope: Blender-free Python.

Deliverables:

1. Strict codecs для final `mh.export_manifest` v1, `mh.material` v1 и текущего
   `mh.composite` v1.
2. Canonical material serialization/hash, включая path normalization:
   `//`-вход после host expansion; под `source_root` → relative с `/`; вне root
   → normalized absolute; никаких stored `external_path` flags.
3. `texture_policy` diagnostics: transitional warning и strict error.
4. Manifest scanner под единственным `source_root`, без disk cache.
5. Registry-hint reader (`source_path`, optional `manifest_path`) с обязательным
   подтверждением полного scan.
6. Resolver result:
   `uid -> {payload_path, owning_manifest_path, manifest_row}`; kind/UID/source
   checks; unique-owner ambiguity error; stale registry warning+fallback.
7. Dependency analyzer: edges вычисляются из `.composite.nodes[].resource_uid` и
   mesh `material_slots[].material_uid`, но нигде не сериализуются.
8. Transaction-source snapshot: pending/changed manifest обнаруживается до
   apply.

Tests/gate:

- internal/external path table, включая Windows drive, UNC, `..`, slash case и
  absolute-inside-root canonicalization;
- stale/wrong-kind/out-of-root registry hints;
- zero/one/two owning manifests;
- missing payload, UID/kind mismatch, pending marker и manifest mutation;
- nested graph, deduplication, missing edge и cycle classification;
- no code path accepts pre-freeze `materials[]` as final v1;
- pure suite green и внешний audit receipt.

## G2 — Standalone writers

Порядок внутри gate: Material → FBX → Composite.

### G2.1 Material writer

- отдельный `.material` payload и material resource-row;
- первый export использует Folder явного Material Export либо каталог FBX при
  включённом `Export Materials`;
- повторный export находит unique owner и обновляет существующий source in
  place, независимо от текущего display-name/Folder;
- rename payload не переименовывает файл;
- canonical hash-skip и общий atomic payload+manifest protocol.

### G2.2 FBX writer

- один Collection → один FBX;
- mesh row содержит slot-name → MaterialUID;
- Boolean `Export Materials`, default ON;
- ON дедуплицирует материалы выбранной Collection hierarchy: unique owner
  обновляется in place, UID без owner создаётся рядом с выбранным FBX output;
- OFF записывает только FBX/mesh-row и не меняет material payload/rows;
- missing material в OFF/failed-material ветке не блокирует geometry export:
  structured warning + список для **Export materials…**;
- runtime order: geometry commit → material upserts; доказанный pending
  MaterialUID является исключением и recovery'ится первым по fail-closed §9.1;
- texture files не копируются ни в одной ветке.

### G2.3 Composite writer

- одна composite Collection → один `.composite`;
- node graph только из Empty Collection Instances;
- external mesh/composite UID обязан разрешаться тем же resolver;
- cycle и unresolved non-material edge блокируют export;
- dependency closure не экспортируется и не записывается в manifest.

Tests/gate:

- upsert сохраняет чужие rows/files;
- повтор без изменений — hash-skip;
- staged failure оставляет fail-closed marker и восстанавливается только
  разрешённым writer;
- material rename остаётся update одного существующего файла;
- ON обновляет shared material у существующего owner, а новый material впервые
  размещает рядом с FBX; OFF даёт warning без material writes;
- повторяющиеся MaterialUID экспортируются один раз;
- Blender integration suite green, затем внешний audit receipt.

## G3 — Recursive importer и UI

### G3.1 Import core

- root `.composite` запускает dependency wave;
- recursive composites и найденные FBX всегда ON;
- ResourceUID dedup создаёт одну sibling definition Collection под `GEOMETRY`;
- materials резолвятся непосредственно по MaterialUID;
- transactional preflight/apply/rollback использует стабильный manifest set;
- missing resource сохраняет NodeUID/ResourceUID в красном unresolved Empty;
- **Resolve Missing** рекурсивно наполняет найденное поддерево в тех же
  Empty/Collections, не пересоздавая существующие узлы;
- Blender-import cycle back-edge даёт `MH_W_COMPOSITE_CYCLE` placeholder и не
  останавливает остальной graph;

### G3.2 UI

- одна N-panel вкладка `MH`;
- секция `FBX Export`: Collection, Folder, `Export Materials` default ON,
  Export FBX;
- секция `Composites`: Import path / Export Collection+Folder, Import/Export;
- секция `Materials`: Material, first-export Folder, Export Material;
- `source_root`, optional registry path и `texture_policy` в project settings;
- быстрый переход **Export materials…** из FBX OFF/failed warning report;
- нет Bundle Export, Texture Root, `Recursive` или `With dags/FBX` toggles;
- structured log отображает UID, owning manifest и resolver reason.

Tests/gate:

- export → clean Blender import → re-export semantic round-trip;
- unresolved на любой глубине, поздний Resolve Missing без смены identity;
- cycle W на Blender-import, E на Blender-export;
- dagormat present/absent и lossless JSON fallback;
- packaged ZIP register/unregister и relevant tests проходят последовательно;
- owner выполняет короткую UX-приёмку, внешний аудитор принимает gate.

## G4 — Golden M8/M9 и field acceptance

### M8 `external_resource`

- `libB` владеет `lamp_set` composite и `lamp_mesh` static mesh; `lamp_set`
  ссылается на `lamp_mesh`, а root composite из `libA` — на `lamp_set`;
- resolver находит unique owner через общий `source_root`;
- без `libB` виден unresolved, возврат + Resolve Missing наполняет тот же target;
- duplicate UID owner даёт ambiguity error.

### M9 `shared_material`

- один `.material` в `common/materials/`;
- два mesh resources из разных owning manifests ссылаются на один MaterialUID;
- материал импортируется один раз;
- его UPDATE_PROPERTIES не обновляет FBX resources.
- FBX/ON обновляет существующий owner in place и не создаёт копию;
- новый UID при ON создаётся рядом с FBX, OFF оставляет material bytes/rows
  неизменными; texture files не копируются.

Final receipts:

1. Machine-readable expected artifacts/diffs для M8/M9.
2. Полный pure + Blender suite, packaged-addon smoke и hash артефакта.
3. Внешний audit полного среза.
4. Owner field acceptance: реальный проектный `source_root`, dag4blend/dagormat,
   cross-directory recursion, missing→resolve, shared material и transitional
   external texture, обе ветки `Export Materials`.

Только после owner acceptance Blender v1 slice считается завершённым. UE watcher,
asset import и placement compilation продолжаются по актуальным post-Blender
этапам проекта, используя замороженные v1 source contracts.

## Не входит в этот execution slice

- автоматический dependency-closure export за пределами явно ограниченного
  material-set выбранной FBX Collection hierarchy;
- `Structure only` import;
- ручной `Rename file to match`;
- disk cache manifest index;
- texture copying/mirroring implementation;
- production legacy-reader pre-freeze форматов.

Эти пункты могут войти в ROADMAP только отдельными решениями; они не должны
расширять v1 on-disk schema молча.
