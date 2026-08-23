# 05 — MH Source Protocol v2: Clean Sources (NORMATIVE ACTIVE)

> **SUPERSEDED BY SOURCE PROTOCOL V4.** Документ целиком не является активным
> on-disk/runtime-контрактом. Действующий норматив —
> [`08_source_protocol_v4_plan.md`](08_source_protocol_v4_plan.md); body ниже
> сохранён как история Source Protocol v2.

Статус: единственный активный on-disk и runtime-контракт Blender → UE.
Имя файла документа сохранено только ради стабильных ссылок. Замороженная
Source Schema v1 по SHA `d52520c47544a6e36b3bac32b16237ad670abb20`
является исторической и читается исключительно одноразовой миграцией (§12).
Production reader не имеет manifest/uid8/dual-read fallback.

Порядок authority при расхождении документов:

1. этот документ;
2. `ADR_V2_passport_first.md` — hashes, explicit writer и reader conflicts;
3. `AMENDMENT_combined_lod_fbx.md` — Combined-LOD;
4. `04_source_workflows.md` и `06_final_v1_plan.md` — UX и execution gates.

## 1. Проверяемые инварианты

1. В source tree лежат только primary payload: `*.mesh.fbx`, `*.composite`,
   `*.material`. `export_manifest.json`, registry, sidecar, marker и cache в
   source tree запрещены.
2. Идентичность ресурса — полный UUID из самого payload. Имя файла и display
   name не участвуют в resolve.
3. Каждый static mesh UID представлен ровно одним Combined-LOD FBX.
4. `.composite` и только он задаёт node graph. FBX содержит geometry payload и
   passport, но из него никогда не извлекаются composite nodes.
5. `.material` — самостоятельный ресурс. Mesh passport ссылается на него по
   MaterialUID; материал не принадлежит первому использовавшему его mesh.
6. Любой explicit Export всегда публикует запрошенный payload атомарно. Writer
   не делает hash-skip, diff, global scan или cache update.
7. Runtime reader принимает только v2-source модель. Legacy manifests и
   uid8-filenames разрешены только migration utility.
8. `MH_E_*` блокирует текущую операцию; `MH_W_*` никогда её не блокирует.

## 2. Source root и стерильное дерево

`source_root` — граница reader scan и UID resolution. Он является настройкой
проекта и не записывается в payload. Recursive scan рассматривает только три
разрешённых расширения. Blender writer не сканирует root и не ведёт index.

UE хранит импортный **Ledger** вне source tree и сравнивает новый scan с ним.
Blender Import Composite может построить lazy disposable cache при первом
импорте; это только accelerator resolver'а, не writer state и не authority.
Отдельного artist-facing **Rebuild** действия нет: stale/missing cache
перестраивается автоматически.

`texture_root` — граница basename-поиска текстур (§8). По умолчанию он равен
`source_root`; явная project setting может сузить или перенести поиск. Ни один
из этих абсолютных root-путей не входит в canonical payload или semantic hash.

## 3. Чистые имена файлов

Первичное имя всегда:

```text
<sanitized_name>.mesh.fbx
<sanitized_name>.composite
<sanitized_name>.material
```

`sanitized_name` — lowercase ASCII, полученный детерминированной заменой
неразрешённых символов на `_`, схлопыванием повторных `_` и удалением `_` по
краям. Пустой результат невалиден. Resource authoring name обязан пройти
проектную ASCII-валидацию; транслитерации нет.

Filename — только удобная подпись. Rename display name может переместить файл,
но не меняет UID и классифицируется как `MOVE`. Resolver никогда не связывает
ресурсы по basename.

Если requested target уже существует с другим embedded UID, writer блокируется
с `MH_E_NAME_COLLISION_DIFFERENT_UID`. UI предлагает только:

- **Rename mine**;
- **Fork existing as new resource**;
- **Cancel**.

Silent overwrite и выбор «последнего по дате» запрещены. Одинаковые clean names
в разных каталогах легальны. Экспорт того же UID в другой каталог также
разрешён; duplicates/conflicts классифицирует reader scan, не writer.

## 4. FBX passport `mh.fbx_passport` v1

### 4.1 Каноническая форма

Каждый static-mesh FBX содержит одну каноническую JSON-строку:

```json
{
  "schema": "mh.fbx_passport",
  "schema_version": 1,
  "resource_uid": "2db5574c-3aca-43cc-9ab5-8242403e18cd",
  "kind": "static_mesh",
  "name": "wall_a",
  "lod_policy": "authored",
  "lod_levels": [0, 1],
  "geometry_hash": "xxh3:9f2c01ab34cd56ef",
  "material_slots": [
    {
      "slot_name": "wall_surface",
      "material_uid": "7d995e54-d084-4466-a613-a1cd8f3248b2",
      "material_name_hint": "m_stucco"
    }
  ],
  "properties": {
    "role": "wall"
  },
  "exporter": "mh4blend 0.x"
}
```

Все top-level fields required. Unknown top-level fields запрещены; открытая
resource extension bag — только `properties`.

- `schema` ровно `mh.fbx_passport`, `schema_version` ровно `1`;
- `resource_uid` — полный canonical UUID;
- `kind` ровно `static_mesh`;
- `lod_policy` — `authored`, `generated` или `nanite`;
- `lod_levels` — отсортированный уникальный dense-массив integer, начинающийся
  с `0`; single-LOD имеет `[0]`;
- `geometry_hash` — `mh.meshser:2` semantic hash всех уровней;
- `material_slots` отсортирован по `slot_name`; `material_name_hint` участвует
  в descriptor hash, но служит только диагностике и не заменяет UID resolution;
- `properties` — asset-level JSON bag;
- `exporter` — диагностическая версия writer.

### 4.2 Carrier B

Passport записывается custom property `mh.fbx_passport` на **каждую**
экспортируемую FBX Model node. Все копии должны существовать и совпадать
побайтово. Отсутствие, malformed JSON, unknown version или consensus mismatch
карантинирует весь payload с `MH_E_PASSPORT_INVALID`.

Carrier B является финальным transport-решением v2, но до включения production
writer обязан пройти блокирующий transport gate из §13: оба Blender FBX пути,
shared datablock, custom normals, длинный JSON, Combined-LOD, import/re-export,
pivots и hierarchy. Нельзя подменять доказательство предположением о FBX SDK.

### 4.3 Три hash-величины и reader-side diff

| Величина | Назначение | Где живёт |
|---|---|---|
| `geometry_hash` | `mh.meshser:2` semantic hash evaluated Blender geometry всех LOD | passport + reader Ledger/cache |
| `descriptor_hash` | hash канонического passport без hash fields | только reader Ledger/cache |
| `payload_fingerprint` | byte fingerprint FBX; size+mtime допустимы как fast-path | только reader Ledger/cache |

Reader никогда не пересчитывает `geometry_hash` из FBX: он читает паспорт.
Изменившийся fingerprint при неизменных semantic hashes даёт
`MH_W_PAYLOAD_EXTERNAL_MODIFIED` и требует подтверждения.

Blender writer вычисляет hashes для паспорта, но **не использует их, чтобы
пропустить explicit export**. Каждый вызов Export выполняет:

```text
collision guard -> write sibling temporary -> atomic replace -> exit
```

Writer не читает Ledger, не обновляет Blender import cache и не строит diff.
Даже semantic no-op перезаписывает requested FBX. После startup/watcher scan UE
сравнивает `geometry_hash` и `descriptor_hash` с Ledger: no-op даёт
`NO_CHANGE`, geometry change — `UPDATE_GEOMETRY`, metadata-only change —
соответствующий descriptor/property update.

## 5. Combined-LOD FBX

Blender authoring сохраняет dag4blend-конвенцию: container `<base>.lods` имеет
direct child Collections `<base>.lod00`, `<base>.lod01`, … . Суффиксы нужны
только extractor'у Blender-сцены и не переносятся как transport semantics.

Все уровни экспортируются одним вызовом в один `<base>.mesh.fbx`. Каждый mesh
Model node несёт integer custom property `mh_lod_level`. Отсутствующее свойство
означает `0`, чтобы single-LOD payload оставался простым. Имена Model nodes не
определяют уровень.

- несколько mesh nodes на один уровень легальны;
- уровни обязаны быть dense `0..N`, иначе `MH_E_LOD_LEVELS_SPARSE`;
- фактический набор узлов обязан совпасть с passport `lod_levels`, иначе
  `MH_E_LOD_PASSPORT_MISMATCH`;
- material slots LOD `1+` обязаны быть подмножеством LOD0, иначе
  `MH_E_LOD_SLOT_NOT_IN_BASE`;
- `UCX_*` и `SOCKET_*` всегда относятся к LOD0; auxiliary node, помещённый в
  LOD `1+`, игнорируется с `MH_W_LOD_AUX_NODE_IGNORED`.

`mh.meshser:2` сериализует все export-affecting mesh и auxiliary данные в
порядке `(lod_level asc, mh_uid asc)` и пишет `lod_level` как `uint32` для
каждого объекта. UCX/SOCKET участвуют в durable hash и reader classification.
Изменение любого уровня или auxiliary payload меняет общий `geometry_hash`;
следующий explicit Export всё равно пишет единый FBX, а UE reader классифицирует
полный re-import одного ресурса.

Legacy `lods[]` rows и `.lod<N>.mesh.fbx` не являются частью v2. Runtime не
игнорирует их и не пытается объединить эвристически; они доступны только
миграции §12.

## 6. Composite `mh.composite` v2

```json
{
  "schema": "mh.composite",
  "schema_version": 2,
  "uid": "f53d93af-94c3-472f-98d0-ff36eb93c417",
  "name": "window_set_a",
  "properties": {
    "role": "architecture"
  },
  "nodes": [
    {
      "node_uid": "6866f569-4d42-472f-a676-a836a3df18ec",
      "parent_uid": null,
      "kind": "mesh",
      "display_name": "window_a.001",
      "resource_uid": "5839a2e1-7118-41a7-a226-0edbcc6941da",
      "local_transform": {
        "translation_cm": [0.0, 45.0, 210.0],
        "rotation_quat": [0.0, 0.0, 0.0, 1.0],
        "scale": [1.0, 1.0, 1.0]
      },
      "properties": {}
    }
  ]
}
```

Top-level required: `schema`, `schema_version`, `uid`, `name`, `properties`,
`nodes`. Unknown fields запрещены вне bags. Top-level `properties` — asset-level
bag; node `properties` — placement-level bag. Они не наследуются друг в друга.

Nodes — flat table, канонически отсортированная по `node_uid`; hierarchy задаёт
`parent_uid`. Required node fields: `node_uid`, `parent_uid`, `kind`,
`display_name`, `local_transform`, `properties`. `parent_uid` равен `null` или
UID узла того же файла. Duplicate UID, dangling parent и parent cycle запрещены.

Kind rules активного slice:

- `group`: `resource_uid` отсутствует;
- `mesh`: required `resource_uid` static mesh;
- `composite_ref`: required `resource_uid` composite.

Остальные kinds не угадываются по имени и блокируются
`MH_E_UNSUPPORTED_NODE_KIND`, пока не получат отдельный runtime contract.

`local_transform` содержит ровно `translation_cm[3]`,
`rotation_quat[x,y,z,w]`, `scale[3]`. Quaternion normalized и
sign-canonicalized; scale по каждой оси строго больше нуля. Координаты: UE
centimeters, Z-up, left-handed.

Dependency graph вычисляется только из `nodes[].resource_uid`. Он не хранится
ни в sidecar, ни в reader cache/Ledger как authority. Blender import cycle back-edge
становится placeholder с `MH_W_COMPOSITE_CYCLE`; Blender export и UE import
блокируются `MH_E_COMPOSITE_CYCLE`.

## 7. Material `mh.material` v1

Material payload остаётся самодостаточным и не получает отдельного passport:

```json
{
  "schema": "mh.material",
  "schema_version": 1,
  "uid": "7d995e54-d084-4466-a613-a1cd8f3248b2",
  "name": "m_stucco_concrete",
  "shader_class": "rendinst_perlin_layered",
  "params": {
    "micro_detail_layer": 0,
    "sides": 0
  },
  "textures": {
    "tex0": "manmade_common/textures/whitewash_plain_a_tex_d.tif",
    "tex2": "C:/DagorLibrary/textures/whitewash_plain_a_tex_n.tif"
  }
}
```

Все семь полей required. Unknown top-level fields запрещены; extensible
material semantics живут только в `params`. `textures` содержит только непустые
`tex0`…`tex15` string slots. Материал без dagormat либо с пустым shader
экспортируется как `rendinst_simple` с пустыми `params` и `textures`.

Material identity — `uid`; filename/name hints не участвуют в resolve. Rename
обновляет payload и clean filename как MOVE, сохраняя UID. Если target clean
name занят другим UID, действует §3, а не silent overwrite.

## 8. Текстуры и Actualize Texture Paths

`textures: {texN: string}` остаётся единственной on-disk формой; флагов
`external_path` нет. Blender `//...` сначала разворачивается относительно
сохранённого `.blend`; `//` из несохранённого файла — ошибка. Путь
нормализуется лексически, использует `/`, не разворачивает environment variables
или `~`.

Relative path разрешается от `texture_root`; absolute path проверяется точно.
Resolver texture slot выполняет каскад:

1. exact normalized path;
2. поиск `basename` под `texture_root`;
3. unresolved.

Одно basename-совпадение означает stale path: Blender material import может
автоматически актуализировать `.material` с записью в лог. Несколько совпадений
дают `MH_W_TEXTURE_BASENAME_AMBIGUOUS` и требуют выбора пользователя; ноль —
unresolved diagnostic с basename.

Оператор **Actualize Texture Paths** и UE commandlet проходят все materials и
выдают `fixed/ambiguous/missing`. Actualization является обычной правкой
`.material`, меняет его semantic hash и вызывает `UPDATE_PROPERTIES`.

Путь под `texture_root` записывается относительно него с `/`; внешний путь —
нормализованным absolute. `texture_policy=transitional` (default) даёт
`MH_W_TEXTURE_OUTSIDE_ROOT`; `strict` повышает тот же факт до
`MH_E_TEXTURE_OUTSIDE_ROOT`. Policy — project setting, не payload data.
Текстуры никогда не копируются и не перемещаются этим протоколом.

## 9. Reader state и resolver

Writer не имеет resolver index. Reader state разделён по host:

- Blender **Import Composite** при первом вызове молча сканирует `source_root`;
  optional lazy cache может ускорить последующие imports, но автоматически
  инвалидируется/перестраивается и не имеет artist-facing Rebuild UI;
- UE startup сканирует source root и сравнивает embedded metadata с **Ledger**;
  default — silent auto-import, optional project preference включает prompt;
- UE watcher повторяет per-file scan и то же сравнение с Ledger.

Resolve всегда подтверждает candidates текущими payload bytes/passports. Cache
miss/stale в Blender означает silent scan, а не ошибку и не ручной repair.
Ledger — состояние reader/import результата, не source authority и не input
Blender writer.

| Состояние | Нормативная реакция |
|---|---|
| UID отсутствует в Ledger, один валидный payload | Auto-import/adopt (silent default; prompt optional) |
| Старый path исчез, новый unique path с тем же UID | MOVE автоматически + log |
| Два path одного UID, fingerprints равны | один semantic resource; duplicate paths перечисляются в warning/log |
| Два path одного UID, fingerprints различны | `MH_E_DIVERGENT_REVISIONS`, только ручной выбор |
| Loose file выбран вручную | Update existing / Fork as New Resource / Cancel |
| Passport/embedded identity отсутствует | runtime quarantine; warning допустим только migration utility |
| Passport malformed/unknown version/consensus mismatch | `MH_E_PASSPORT_INVALID`, quarantine |

**Fork as New Resource** назначает новый UID и переписывает embedded identity.
Для composite UI отдельно предлагает, какие внутренние ссылки форкнуть; original
UID не изменяется.

## 10. Explicit writer, startup scan и watcher

Каждый вызов Blender Export всегда обрабатывает requested target(s): проверяет
target collision, пишет temporary sibling, flush/close, делает atomic replace и
завершается. Ни geometry/descriptor comparison, ни source-root scan, ни Ledger,
ни lazy import cache не входят в writer flow. Короткий per-file lock защищает
только target от двух одновременных writers; timeout даёт
`MH_E_PAYLOAD_LOCK_TIMEOUT` и zero writes.

Crash до replace оставляет старый target; crash после replace оставляет новый.
Глобальных markers/locks нет.

UE startup (silent auto-import default, prompt optional) и watcher используют
size+mtime как fast-path, затем fingerprint/passport validation и сравнение с
Ledger. Стабилизируется конкретный файл, не весь root. Batch order:

```text
textures -> materials -> geometry -> composites -> finalize -> ledger
```

Semantic diff полностью reader-side. Writer не печатает diff как условие
записи. `tools/diff_bundles.py` работает как отдельный reader: сравнивает
passports/payloads/ledger-like snapshots; manifest fallback запрещён.

## 11. Диагностики

Обязательные v2 codes:

| Code | Условие |
|---|---|
| `MH_E_NAME_COLLISION_DIFFERENT_UID` | clean target filename занят другим UID |
| `MH_E_PASSPORT_INVALID` | passport отсутствует/повреждён/не согласован в runtime |
| `MH_E_DIVERGENT_REVISIONS` | два payload одного UID имеют разные fingerprints |
| `MH_E_EXTERNAL_MODIFICATION_CONFIRMATION_REQUIRED` | reader/writeback видит внешнюю правку и требует явного подтверждения перед применением/overwrite |
| `MH_E_PAYLOAD_LOCK_TIMEOUT` | writer не получил per-target lock в срок; payload не меняется |
| `MH_E_RESOURCE_NOT_FOUND` | required ResourceUID отсутствует; блокируется затронутый resource/edge, batch может продолжиться |
| `MH_E_SOURCE_INDEX_INVALID` | reader построил/получил невалидный ephemeral source snapshot и не может безопасно продолжить |
| `MH_E_SOURCE_INDEX_PATH_OUTSIDE_ROOT` | candidate reader snapshot вышел за `source_root` |
| `MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED` | payload set изменился между reader preflight и apply |
| `MH_E_PENDING_EXPORT_MARKER` | migration scan нашёл незавершённую v1 writer-транзакцию; zero writes |
| `MH_E_V1_MIGRATION_INVALID` | legacy manifest/payload не соответствует frozen v1 |
| `MH_E_V1_MIGRATION_FAILED` | migration не смогла выпустить и проверить v2 payload |
| `MH_E_V1_MIGRATION_CLEANUP_FAILED` | v2 payload готов, но безопасная очистка legacy-файла не завершилась |
| `MH_E_V1_MIGRATION_IDENTITY_MISMATCH` | legacy UID не совпал с Blender/payload identity |
| `MH_E_MIGRATION_MESH_COLLECTION_NOT_FOUND` | для re-export static mesh не найдена уникальная Blender Collection с нужным UID |
| `MH_W_PAYLOAD_EXTERNAL_MODIFIED` | fingerprint изменён при прежних semantic hashes; observation до решения policy |
| `MH_W_DEPRECATED_PER_LOD_PASSPORT_MIGRATION_REQUIRED` | только migration scan: найден legacy per-file LOD passport |
| `MH_W_DUPLICATE_IDENTICAL_PAYLOAD` | несколько byte-identical candidates одного UID |
| `MH_W_FBX_CARRIER_READER_UNAVAILABLE` | FBX carrier capability недоступна для отдельного probe/migration path; production FBX resource затем fail-closed |
| `MH_W_LEGACY_COMPOSITE_V1_MIGRATION_REQUIRED` | только migration scan: composite v1 требует conversion; production resolver не считает его candidate |
| `MH_W_LEGACY_PAYLOAD_NO_PASSPORT` | только migration scan; production runtime quarantines payload |
| `MH_W_MATERIAL_NOT_FOUND` | referenced MaterialUID отсутствует; spelling `MH_W_MISSING_MATERIAL` не используется |
| `MH_W_TEXTURE_BASENAME_AMBIGUOUS` | basename имеет несколько кандидатов |
| `MH_E_LOD_LEVELS_SPARSE` | набор LOD не равен `0..N` |
| `MH_E_LOD_SLOT_NOT_IN_BASE` | LOD1+ использует slot вне LOD0 |
| `MH_E_LOD_PASSPORT_MISMATCH` | заявленные и фактические уровни различаются |
| `MH_W_LOD_AUX_NODE_IGNORED` | UCX/SOCKET авторился на LOD1+ |

Существующие semantic codes composite/material остаются в силе, если не
ссылаются на manifest authority. Deprecated-manifest codes разрешены только
миграционной утилите; production runtime не имеет legacy reader.

## 12. Одноразовая миграция v1 → v2

Migration operator — единственное место, где разрешено читать frozen v1:

1. сканирует legacy manifests и uid8 filenames;
2. строит полный preflight и ничего не пишет при любой неоднозначности;
3. переносит identity/metadata static mesh в FBX passport; предпочтителен
   re-export из Blender, SDK patch допустим только при отсутствии сцены;
4. переносит composite resource `properties` внутрь `mh.composite` v2;
5. переименовывает payload в clean names; collisions выдаёт в ручной отчёт;
6. удаляет manifests только после успешной проверки всех затронутых payload;
7. выдаёт `migrated/renamed/conflicted/failed` receipt и инструкции rollback.

Legacy composite v1 обнаруживается только здесь с
`MH_W_LEGACY_COMPOSITE_V1_MIGRATION_REQUIRED`; production resolver не добавляет
его в candidate set.

Per-file LOD требует one-shot подготовки/реэкспорта в Combined-LOD; подробности
— в `AMENDMENT_combined_lod_fbx.md`. Production resolver не импортирует и не
repair'ит legacy rows. Постоянный dual-read запрещён.

## 13. Implementation gates

1. **Carrier B proof — blocking.** Реальные FBX, один/несколько объектов,
   shared datablock, custom normals, длинный JSON, Combined-LOD, оба Blender
   пути, UE reader, re-export, pivots/hierarchy и byte consensus.
2. **Clean explicit writers.** Clean names, mandatory passports, composite v2,
   material self-identity, `mh.meshser:2`; каждый Export всегда пишет requested
   target, manifest/index/diff code отсутствует. No-op export переписывает файл,
   а reader классифицирует `NO_CHANGE`.
3. **Readers/resolver/conflicts.** Blender lazy Import Composite scan/cache и UE
   startup/watcher Ledger diff; полная conflict matrix, Fork, M8/M9/M10 v2,
   texture basename actualization. Artist-facing Rebuild отсутствует.
4. **Migration/import.** One-shot v1 reader только в migrator; v2 recursive
   import, unresolved placeholders и Resolve Missing.
5. **Crash/concurrency.** Crash до/после atomic replace, два writers,
   Blender+UE reader, stale/deleted lazy cache/Ledger, per-file stability.
6. **UE parity.** UE подключается после Blender gate 2; C1 доказывает равную
   passport/hash/change классификацию, последующие C/D gates используют только
   этот контракт.

Внешний аудитор принимает крупные gates. Owner выполняет финальную полевую
приёмку на реальном Blender/UE проекте. До receipts соответствующая часть
контракта считается запланированной, а не доказанно реализованной.
