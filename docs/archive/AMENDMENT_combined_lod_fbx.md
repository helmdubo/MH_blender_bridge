> Status: HISTORY · Do not use for implementation · Superseded by docs/16_recipe_model.md

# Combined-LOD FBX — normative v2 profile

> **PARTIALLY SUPERSEDED BY SOURCE PROTOCOL V4.** Выживает только решение
> Combined-LOD: один FBX содержит все dense LOD-уровни ресурса. Части о
> passport, `mh_lod_level`, meshser/hash и migration superseded. Единственный
> норматив — [`08_source_protocol_v4_plan.md`](08_source_protocol_v4_plan.md),
> особенно §4 и §12.

Статус: **ACTIVE NOW** как часть MH Source Protocol v2. Этот документ больше
не является временной поправкой к v1. Его прежние uid8/manifest/per-file LOD
формулировки superseded Clean Sources CONTRACT и сохранены только как
миграционный контекст §7.

Мотивация: один static mesh resource = один FBX на диске, включая все уровни
детализации. Владелец осознанно принимает цену: правка любого LOD полностью
реэкспортирует и реимпортирует этот ресурс.

## 1. Формат payload

`<sanitized_name>.mesh.fbx` содержит mesh nodes всех LOD-уровней ресурса.

Принадлежность задаёт явная custom property `mh_lod_level : int` на mesh Model
node. Node без свойства относится к level 0. Имена node, `.lod00` suffix и
порядок FBX nodes семантики уровня не несут.

- Несколько mesh nodes на один level легальны.
- Фактический набор уровней обязан быть dense `0..N`; разрыв даёт
  `MH_E_LOD_LEVELS_SPARSE`.
- `UCX_*` и `SOCKET_*` относятся к level 0. Auxiliary node, авторенный внутри
  level `1+`, игнорируется с `MH_W_LOD_AUX_NODE_IGNORED`.
- Material slots уровней `1+` обязаны быть подмножеством slots level 0;
  нарушение даёт `MH_E_LOD_SLOT_NOT_IN_BASE`.

## 2. Blender authoring и export

Dag4blend-compatible authoring остаётся:

```text
<base>.lods
  <base>.lod00
  <base>.lod01
  ...
```

Только direct `<base>.lodNN` Collections принадлежат container. Extractor
валидирует ровно один `lod00`, unique numeric levels и отсутствие gaps. Суффикс
является Blender authoring convention; в transport metadata он не переносится.

Exporter собирает объекты всех levels и выполняет один FBX export. Перед
вызовом он временно назначает `mh_lod_level`, затем гарантированно восстанавливает
исходные Custom Properties в `finally`. Persistent mutation допустима только
если отдельный implementation decision и field UX это явно примут; default v2
— временный export context.

Single-LOD Collection экспортируется тем же writer с `lod_levels: [0]`; явная
node property `0` не обязательна.

## 3. Passport и validation

FBX passport содержит один resource UID и полный заявленный состав:

```json
{
  "schema": "mh.fbx_passport",
  "schema_version": 1,
  "kind": "static_mesh",
  "lod_policy": "authored",
  "lod_levels": [0, 1]
}
```

Fragment выше не является полным passport; полная required форма — 05 §4.
Per-file `mh_lod_level` в passport запрещён: level принадлежит node, не файлу.
Reader группирует mesh nodes по property и сверяет фактический dense set с
`lod_levels`. Несовпадение даёт `MH_E_LOD_PASSPORT_MISMATCH` и блокирует ресурс.

`lod_policy=authored` означает, что уровни находятся внутри этого FBX.
`generated`/`nanite` сохраняются как resource policy, но не разрешают reader'у
игнорировать заявленный/фактический mismatch.

## 4. Durable hash и reader diff

Combined-LOD использует `mh.meshser:2`. Все export-affecting mesh data и
auxiliary UCX/SOCKET входят в общий semantic stream. Порядок объектов:

```text
(lod_level ascending, mh_uid ascending)
```

Для каждого объекта level записывается `uint32`. Auxiliary nodes нормативно
принадлежат level 0 и также участвуют в durable stream. Поэтому UE reader
обнаруживает изменение collision/socket geometry как geometry change.

Explicit Blender Export всегда перезаписывает requested FBX независимо от
hashes. Любое изменение level, material assignment, auxiliary payload или level
set меняет общий `geometry_hash`; UE startup/watcher сравнивает его с Ledger и
классифицирует `UPDATE_GEOMETRY`. Semantic no-op тоже пишет FBX, но reader
классифицирует `NO_CHANGE`. Обновление с `mh.meshser:1` один раз честно даёт
`UPDATE_GEOMETRY` для всех mesh resources.

## 5. UE import

`FMHFbxBackend` одним проходом:

1. валидирует Carrier B consensus;
2. читает integer `mh_lod_level` с mesh nodes (missing → 0);
3. валидирует dense levels, passport set и material-slot subset;
4. собирает nodes одного уровня в `SourceModel[N].MeshDescription`;
5. применяет UCX/SOCKET только к level 0;
6. обновляет существующий StaticMesh in place.

Screen sizes в текущем C-slice вычисляются автоматически через
`bAutoComputeLODScreenSize`; authored distances — ROADMAP. Отдельный
`MH_E_LOD_IMPORT_FAILED` не нужен: malformed level делает malformed весь
resource и операция использует точный validation code причины.

`fbxdump` печатает passport UID, `mh_lod_level` per node и сводку уровней.

## 6. Golden и gates

Позитивная fixture: `<base>.lods` с levels 0 и 1; level 1 содержит два mesh
objects. Негативные fixtures:

- sparse `0, 1, 3`;
- slot на LOD1, отсутствующий в LOD0;
- auxiliary node, авторенный в LOD1;
- passport `lod_levels` mismatch;
- divergent Carrier B copies.

Мутации:

- `edit_lod1_geometry` → один `UPDATE_GEOMETRY`, один combined payload;
- `add_lod_level` → rewrite того же payload и новый passport set;
- `remove_lod_level` → rewrite того же payload;
- `edit_ucx_geometry` → rewrite того же payload;
- no-op explicit export → payload rewritten, reader result `NO_CHANGE`.

C2 field case: правка geometry LOD1 в Blender → re-export единого FBX → один
resource `UPDATE_GEOMETRY` → UE пересобирает все SourceModels, сохраняя тот же
asset object (reimport-in-place).

## 7. Migration from legacy per-file LOD

Production resolver не читает `lods[]`, legacy manifests или
`.lod<N>.mesh.fbx` как resource set. Legacy reader существует только во внешней
one-shot utility/full v1→v2 migration operator.

Migration discovery emits
`MH_W_DEPRECATED_PER_LOD_PASSPORT_MIGRATION_REQUIRED`; legacy FBX без passport
uses `MH_W_LEGACY_PAYLOAD_NO_PASSPORT`. Оба warning разрешены только в migrator.

Безопасная подготовка старой выгрузки обязана:

1. проверить отсутствие pending legacy marker;
2. найти ровно одну выбранную static-mesh row по UID;
3. проверить primary и все declared LOD payload как regular files внутри
   допустимого legacy root;
4. проверить unrelated rows строгим legacy codec;
5. ничего не писать при ambiguity, missing/non-file, path escape или backup
   collision;
6. сохранить точные исходные bytes в non-scanned backup;
7. атомарно подготовить состояние к немедленному re-export из Blender;
8. не удалять legacy payload автоматически;
9. выдать structured receipt и безопасный restore только пока prepared state
   не изменился.

Предпочтительный следующий шаг — re-export исходной `<base>.lods` Collection с
тем же ResourceUID. Он создаёт один clean `<base>.mesh.fbx` с паспортом и
`mh.meshser:2`. Полная v2 migration после проверки удаляет legacy manifest;
runtime dual-read после этого не сохраняется.

Существующая `tools/migrate_per_file_lods.py` является внешним переходным
инструментом для старых локальных выгрузок, а не библиотекой production reader.
Её receipt/restore safety остаются обязательными до поглощения полной v2
migration operator.
