# ADR v3 — Interchange hybrid: applied state, semantic IR, export

> **SUPERSEDED BY SOURCE PROTOCOL V4 IN FULL.** Этот ADR не является
> нормативным или implementation input. Действующий контракт —
> [`08_source_protocol_v4_plan.md`](08_source_protocol_v4_plan.md); body ниже
> сохранён как история Interchange-гипотезы.

Статус: **ACTIVE**. Это нормативный docs-коммит, которого ожидали
`UE-QUESTION-15/16`. Он ратифицирует итоговую выжимку архитектурного агента
(«расширенная версия импорта/экспорта», owner-confirmed) и supersede-ит:

- `07` §2 в части «`UMHImportLedger` — authoritative applied state»;
- `07` §4 в части «diff сравнивается с Ledger» (сравнение — с applied state
  внутри ассета; Ledger остаётся ускорителем/индексом);
- `07` §12 в части «Interchange вне C-scope»;
- D44 `[reader-ledger-and-conflict-matrix]` в части applied-authority
  (conflict matrix и quarantine правила D44 остаются в силе).

Не изменяет: Clean Sources v2 on-disk protocol (05/ADR_V2/Combined-LOD),
passport consensus, канонизацию, коды диагностик, правило «writer не делает
diff». Новые on-disk поля вводятся только отдельными numbered amendments.

## 1. Формула архитектуры

```text
External files          — content authority (FBX/.material/.composite/textures)
Compiler registries     — compiler authority (masters, collision/surface
                          profiles, build recipe; версионируются в UE project)
MH Source Catalog       — disposable текущий взгляд на source tree
MH semantic IR          — единая модель import и export
Interchange             — UE-native lifecycle, factories, per-asset applied state
UInterchangeAssetImportData — applied state внутри каждого UStaticMesh
UAssetUserData          — runtime metadata, не история импорта
Direct Autodesk FBX SDK — transport/parity backend и deterministic writer
Central Ledger          — derived dashboard/index, не authority
```

Content authority отвечает «что должно быть построено», compiler authority —
«как строить»; оба участвуют в `RecipeHash`.

## 2. Четыре слоя состояния

```text
1. Current source state   — Saved/Mimir catalog (disposable, восстановимый)
2. Immutable snapshot     — одна операция import видит замороженный набор
                            payload'ов (TOCTOU-барьер)
3. Applied asset state    — внутри .uasset: UInterchangeAssetImportData
                            (CachedNodeContainer = previous MH graph + hashes)
4. Runtime metadata       — UAssetUserData (surface/audio/fx/collision
                            profiles); никогда не в import data
```

Правило: на вопрос «какая source-ревизия реально применена в этом ассете»
отвечает ТОЛЬКО слой 3. Ledger не может его подменять; его потеря не теряет
ничего, кроме ускорения.

## 3. Applied state: маршрут и содержимое

- Маршрут — **asset import** (`ImportAsset`), не `ImportScene`.
  `UFbxSceneImportData`/Scene Blueprint не используются; `SceneImportAsset`
  остаётся пустым.
- Первый этап использует **штатный** `UInterchangeAssetImportData`.
  MH-состояние хранится как custom nodes/attributes в `CachedNodeContainer`:

```text
MHResourceNode  uid=mh:asset:<ResourceUID>
    RawHash, TransportIRHash, SemanticHash, RecipeHash,
    GeometryKey, CollisionKey, BindingKey, MetadataKey,
    AppliedSourceKey, BuiltAssetKey
MHRenderNode    uid=mh:node:<NodeUID>
    lod, GeometryHash, TransformHash, BindingHash, parent uid
MHCollisionNode uid, mode, owner uid, GeometryHash, MetadataHash
MHSocketNode    uid, TransformHash
MHMaterialBinding slot -> MaterialUID + AppliedMaterialKey
```

- `NodeUniqueID` ассета = `mh:asset:<ResourceUID>`.
- Собственный subclass import data (`UMHStaticMeshImportData` из первого
  архитектурного сообщения) **не является равноправной альтернативой**;
  он допускается позже только при доказанной необходимости одного из:
  Asset Registry tags сверх достижимого, компактный summary вне graph,
  миграция, editor details. Триггеры фиксируются новым вопросом, не молча.
- Applied state обновляется только после цепочки:
  BuildPlan применён → StaticMesh build завершён → async compilation завершена
  → package сохранён → transaction committed. Analyzer не пишет applied state.
- Generated MI: applied state в `UMHMaterialSourceData : UAssetUserData`
  внутри package MI (MaterialUID, source path, applied/recipe hashes,
  library key). Library-ref MI собственного applied state не имеет: binding
  живёт в graph зависимого ассета. `ParameterStateId` — только dirty hint,
  никогда не identity.
- `UMHCompositeAsset` владеет собственным AssetImportData (класс наш).

## 4. Ledger demoted

`UMHImportLedger` = derived index: dashboard, `UID -> asset path` accelerator,
orphan список, журнал незавершённых операций, диагностика. Он строится из
Asset Registry tags и applied state ассетов, удаляем и восстановим. Любая
строка кода, читающая Ledger как applied truth, после первого post-C1 slice —
дефект. Conflict matrix `07` §3 продолжает действовать поверх снапшота и
applied state.

## 5. Semantic IR — единая модель import/export

```text
FBX -> (translator) -> FMHSceneIR -> diff/plan -> Interchange factory nodes
                                                -> UStaticMesh
UStaticMesh + provenance -> FMHSceneIR -> FMHFbxWriter -> FBX + .material
```

- `FMHSceneIR` / `FMHMaterialIR` / `FMHCompositeIR` — компактные immutable
  структуры без UObject; canonical hashing и headless-тесты живут здесь,
  НЕ в `UInterchangeBaseNodeContainer`.
- Interchange container — UE-native представление, в него IR транслируется
  адаптером (`FMHInterchangeNodeBuilder` / `FMHInterchangeStateReader`).
- Importer и exporter не имеют двух независимых моделей данных.

### Translator policy

Первый под-gate C2 (§9) проверяет **stock** `UInterchangeFbxTranslator` на
production FBX: custom props (`mh_uid`, passport, `mh_lod_level`), несколько
render nodes, transforms, slots, collision nodes, normals/UV/colors.
Любая потеря обязательной семантики → собственный `UMHFbxTranslator` поверх
direct SDK, реализующий `IInterchangeMeshPayloadInterface`. Границы выше и
ниже неизменны.

`FMHFbxBackend` теряет роль владельца import lifecycle и остаётся:
low-level reader, golden/parity backend, axis/transform validator, fallback
payload provider, основа deterministic writer.

## 6. Лестница hash'ей и классификация изменений

```text
RawHash > TransportIRHash > {GeometryHash, CollisionHash,
                             BindingHash, MetadataHash} + RecipeHash
```

| Отличие | Классификация |
|---|---|
| ничего | `NO_CHANGE` |
| только RawHash | `REPACK_ONLY` — обновить fingerprint, без rebuild |
| только path/name | `MOVE`/`RENAME` |
| только BindingHash | `REBIND_MATERIALS` |
| только CollisionHash | `REBUILD_COLLISION` |
| только MetadataHash | `PATCH_METADATA` |
| GeometryHash | `REBUILD_STATIC_MESH` |
| RecipeHash | `REBUILD_RECIPE` |

Node-level diff в первом production-этапе — инструмент классификации,
diagnostics и export provenance. Изменение одного render node =
rebuild всего ресурса; partial patching render buffers запрещён до
отдельной ратификации.

## 7. Node identity contract v2 (target)

Целевые свойства узла (лягут on-disk отдельным amendment вместе с bump
Blender writer; до этого действует текущий passport/`mh_uid`/`mh_lod_level`):

```text
mh_node_schema = 1
mh_node_uid            (legacy mh_uid читается как migration alias)
mh_geometry_uid
mh_role                render | collision | socket | marker | group
mh_lod_level           int
mh_owner_resource_uid
mh_owner_node_uid      (optional)
mh_collision_mode      physics | query | both
mh_surface_profile     stable project key
mh_fx_profile          stable project key
```

Суффиксы `UCX_`/`SOCKET_`/`_cls_*` остаются authoring UX Blender; writer
преобразует их в metadata; UE mapper строит семантику из properties, suffix —
validation/fallback. Rename после этого не ломает контракт.

`group` — организационный null node (AMENDMENT_node_hierarchy r2): полная
иерархия транспортируется, UE не создаёт из групп ассетов, но хранит их в
node table provenance. Skeleton/кости — зарезервированное расширение
транспорта, отдельный amendment.

Collision mapping: `physics -> PhysicsOnly + ContributeToMass`,
`query -> QueryOnly + !ContributeToMass`, `both -> QueryAndPhysics`
на уровне `FKShapeElem`. Surface/fx профили в MVP — asset-level или
owner-node-level (`UMHStaticMeshRuntimeData`); per-hull gameplay surface —
отдельное расширение, не неявное свойство collision material.

## 8. Provenance и обратный экспорт

- При импорте в source `FMeshDescription` пишется polygon attribute
  `MH.SourceNodeIndex : int32`; таблица `index -> NodeUID` живёт в applied
  state. Вариант «UID строкой в polygon attribute» отклонён (объём,
  дублирование, несериализуемость дёшево).
- Обязательный тест: attribute переживает commit/save/load/reimport.
- Два режима export:
  - **Preserve Source Structure** — только при живой provenance;
    восстанавливает исходные node boundaries;
  - **Flatten Current UE State** — всегда доступен: один render node на LOD;
    ResourceUID сохраняется, node decomposition новая. Угадывание границ по
    материалам/connected components запрещено.
- Collision аналогично: authored structure при живой provenance, иначе
  текущие shapes из BodySetup; после convex decomposition исходная collision
  mesh не восстанавливается — это заявляется честно.

### Publish UE Changes to Source

Экспорт — не «Save as FBX», а транзакция:

```text
Base   = applied state ассета
Theirs = текущий source snapshot
Ours   = текущее фактическое состояние UE-ассета
```

1. three-way diff по доменам (geometry / node topology / bindings /
   collision / sockets / mesh properties / recipe / gameplay metadata);
2. пересекающийся домен → `CONFLICT`, непересекающиеся мержатся;
3. canonical IR → staging write (FBX + затронутые `.material`);
4. **обязательный read-back**: staged файлы читаются нашим reader'ом,
   IR обязан совпасть с исходным canonical IR;
5. divergence guard: applied hash != текущий source hash → блок
   (`Take External` / `Export UE As New Resource` / явный логируемый
   `Force Overwrite External`, никогда default);
6. staged публикация: сначала materials, затем FBX; per-resource locks;
   recovery journal под `Saved/Mimir`, не в source tree; watcher игнорирует
   staging.

`UStaticMeshExporterFBX` — только diagnostic exporter; `BakeMaterialInputs`
не является сериализацией MI. Interchange Writer допустим позже как adapter
над `FMHFbxWriter`, не как фундамент. Экспорт геометрии — source
`FMeshDescription` каждого LOD, не render/Nanite data.

### Materials на экспорт

FBX несёт только `slot_name` + `mh_material_uid` + name hint. Payload — в
`.material` (library_ref | instance) с полным `FMaterialParameterInfo`
(Name/Association/Index) и типизированными overrides (scalar/vector/texture/
RVT/static switch/component mask/font/parent/physical material/BPO).
Один MI на сотни мешей: payload не переписывается без изменения, дубликаты
в папках мешей запрещены; MI без MaterialUID требует `Adopt`. Component
overrides принадлежат `.composite`, не mesh asset.

## 9. Gates (ратификация UE-QUESTION-16)

Interchange spike — **первый блокирующий под-gate C2**, обозначение **C2.0**:

```text
C2.0  stock translator probe + UInterchangeAssetImportData save/reload
      + IR skeleton (FMHSceneIR) + решение stock/UMHFbxTranslator
C2.1  factories/builders поверх выбранного translator (бывший C2)
C2.x  Combined-LOD/material/reimport сценарии из 07 §13 без изменений
```

Внешний аудит остаётся на границе C2 в целом; C2.0 имеет собственную
квитанцию. Provenance polygon attribute входит в C2.1. Export side
(`FMHFbxWriter`, Publish) — отдельный этап после C2, до C4 field acceptance.

## 10. Отношение к C1 (PR #7)

| Элемент C1 | Статус относительно ADR v3 |
|---|---|
| `IMHSourceResolver`/`IMHChangeDetector` seams | подтверждены; за ними меняется state implementation |
| Scan/snapshot, byte re-read, divergent matrix | без изменений |
| Quarantine/diagnostics, fail-closed aliases | без изменений (UE-QUESTION-18 открыт) |
| Analyzer сравнивает с Ledger | легален в C1; первый post-C1 slice переключает сравнение на applied state |
| Ledger как applied authority | superseded настоящим ADR |
| Отсутствие Interchange-кода в C1 | корректно; вход — C2.0 |
| `MHFbxDump` | без изменений; ground truth транспорта (см. AMENDMENT_node_hierarchy) |

## 11. Модули (target)

`MimirBridgeCore` (IR/hashes/diff, без UObject) · `MimirBridgeRuntime`
(composite asset, runtime user data) · `MimirBridgeInterchange` (translator/
pipeline/adapters) · `MimirBridgeEditor` (catalog/watcher/coordinator/
builders/exporter/UI) · `MimirBridgeTests`. Переименование пакета — после
C2; границы обязательны уже при C2.0.
