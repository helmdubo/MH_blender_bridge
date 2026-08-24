# 08 — MH Source Protocol v4: name-keyed, one-way meshes (PLAN, ACTIVE)

Статус: **утверждённый owner план**. Это нормативная база для срезов
`09_v4_agent_slices.md`. Все прежние нормативы (01/05/07, ADR_V2, ADR_V3,
AMENDMENT_combined_lod, AMENDMENT_node_hierarchy, старые UE-QUESTION)
supersede-ятся в части identity/UID/round-trip; что именно выживает — §12.

## 1. Формула

```text
Имя файла определяет identity. Расширение определяет тип.
Папка — только организация и текущее расположение.
StaticMesh — односторонне генерируемый asset (Blender -> UE).
Material и Composite — двусторонние JSON (explicit overwrite publish).
Project Resource Index — rebuildable кэш, не authority.
Applied state живёт внутри соответствующего UE asset.
Дубликат имени одного kind не выбирается автоматически.
Rename — сознательный breaking change.
UUID не существуют нигде: ни в payload, ни в Blender, ни в UE.
```

## 2. Identity

```text
ResourceKey = Kind + LogicalName
static_mesh: garage_a          <- garage_a.mesh.fbx
material:    m_stucco          <- m_stucco.material
composite:   garage_type_a     <- garage_type_a.composite
texture:     brick_a_tex_d     <- brick_a_tex_d.<img-ext>
```

- Алфавит logical name: `[a-z0-9_]+`. Неканоничное имя файла (регистр,
  пробелы, точки внутри stem, не-ASCII) — fail-closed reject на скане, без
  тихого lowercase. Каноничный составной суффикс — строго lowercase
  (case-sensitive). Машинный код любого нарушения каноничного имени файла
  (stem вне алфавита ИЛИ не-lowercase суффикс) —
  `MH_E_NONCANONICAL_RESOURCE_NAME`: вводится в S2 и заменяет legacy
  `MH_E_NON_ASCII_RESOURCE_NAME` во всех call sites, реестре и golden
  (решение OPEN-V4-4).
- Stem получается срезанием ПОЛНОГО составного расширения (`.mesh.fbx`,
  `.material`, `.composite`; для текстур — одиночного `<img-ext>` из
  allowlist §5); `foo.bar.mesh.fbx` невалиден.
- Одинаковый stem у разных kind — три разных ресурса; одинаковый stem одного
  kind в разных папках: `MH_W_DUPLICATE_RESOURCE_NAME` на скане,
  `MH_E_AMBIGUOUS_RESOURCE_NAME` на resolve/import; блокируется ресурс и его
  dependents; mtime/размер/порядок папок никогда не выбирают победителя.
- Перемещение файла = та же identity (обновить path). Переименование =
  DELETE+CREATE; старые ссылки unresolved, UE asset — orphan. Индекс выдаёт
  `MH_W_PROBABLE_RESOURCE_RENAME` при «старый исчез + новый появился с тем же
  raw hash», но alias не создаёт. Re-bind сироты с сильно разошедшимся
  содержимым — `MH_W_ORPHAN_REBOUND_CONTENT_DIVERGED` (warning, не блок).
- Структура папок source-дерева ЛЮБАЯ (поправка owner №1). Нормативны только:
  `source_root` (корень) и расположение кэша вне source tree.

## 3. Project Resource Index

- `<UnrealProject>/Saved/MimirBridge/ProjectIndex.sqlite`; в `.gitignore`;
  удаление без потерь — полное восстановление сканом source tree.
- **Единственный писатель — UE** (scan/watcher/startup/publish). Blender
  индекс НЕ пишет и не читает: его экспорт — атомарная запись файлов,
  watcher подберёт. (Упрощение против черновика v3 §4.3.)
- Таблицы: ResourceCandidates (kind, name, path, size, mtime, raw_hash,
  parse_status, generation), ResourceKeys (resolution_status:
  unique|ambiguous|invalid|missing), Dependencies (owner -> target, role),
  GeneratedAssets (kind, name, ue_object_path, applied_hash, status),
  Diagnostics.
- Raw hash всюду (index, applied state, probable-rename) — engine-native
  BLAKE3-160 в self-describing форме `blake3-160:<40 hex lowercase>`
  (ратифицирован фактом S1); смена алгоритма — новый tag-префикс, не
  переинтерпретация старых строк.
- Self-publish: каждый publish получает token; watcher-событие с hash,
  совпадающим с опубликованным, классифицируется `SELF_PUBLISHED` — без
  повторного импорта.

## 4. FBX-контракт v4 (поправки №2, №3)

FBX — односторонний транспорт Blender → UE и **обычный FBX без MH-метаданных**:
паспорт/descriptor/`mh_*` custom properties УДАЛЕНЫ. Вся классификация — на
стороне кастомного UE-импортёра по FBX SDK узлам:

| Узел | Правило распознавания | Результат в UE |
|---|---|---|
| Render mesh LOD N | суффикс имени `_lodNN` (нет суффикса = LOD0) | `SourceModel[N]` |
| Collision | префикс `UCX_` ИЛИ суффикс `_cls_phys` / `_cls_trace` / `_cls_both` | BodySetup shape c CollisionEnabled по суффиксу (`phys`→PhysicsOnly, `trace`→QueryOnly, `both`→QueryAndPhysics; `UCX_` = both) |
| Socket | null node с префиксом `SOCKET_` | UStaticMeshSocket |
| Group | прочие null nodes | структура; мировые трансформы детей учитываются, asset не создаётся |

- Blender exporter гарантирует конвенции имён на выходе: объекты
  `.lodNN`-коллекций получают суффикс `_lodNN` (временный rename в export
  context, если имя без суффикса), `UCX_`/`SOCKET_`/`_cls_*` проходят как
  есть. Никаких custom properties exporter больше не пишет. Совпадающий
  terminal-суффикс сохраняется; mismatched terminal-суффикс (например
  `wheel_lod00` внутри `.lod01`) — ПОСТОЯННЫЙ fail-closed reject
  `MH_E_INVALID_LOD_HIERARCHY`: exporter никогда не переписывает и не
  «чинит» авторские имена (решение OPEN-V4-3).
- Полная иерархия (пустышки + меши) транспортируется; замыкание по родителям
  fail-closed `MH_E_PARENT_OUTSIDE_RESOURCE`; кости зарезервированы.
  (Выжившие решения UE-QUESTION-19 / AMENDMENT r2.)
- **Импорт = полный rebuild ассета** (поправка №3): геометрия всех LOD,
  коллизии, сокеты, слоты — целиком, in-place (тот же UObject). Частичных
  доменных обновлений нет. Diff мешей: `raw hash не изменился -> NO_CHANGE`,
  изменился -> `REIMPORT`. Semantic-хэширование мешей на Blender-стороне
  (meshser) отменяется вместе с descriptor'ом.
- Привязка материалов: **имя material slot в FBX == material logical name**;
  импортёр резолвит `<slot>.material` через индекс до сборки меша; slot без
  `.material` — `MH_E_MATERIAL_NOT_FOUND`-класс, блок ресурса. Никаких
  post-import скриптов.

## 5. Материалы v4 (поправки №4–№7, №11)

Файл `<name>.material` — лаконичный JSON без schema/version/mode/uid/имени
(тип задан расширением). Режим определяется присутствием поля:

```json
{ "class": "rendinst_perlin_layered",
  "textures": { "tex0": "marble_a_grey_tex_d", "tex2": "marble_a_tex_n" },
  "params": {
    "invert_heights": [0.0, 1.0, 1.0, 1.0],
    "paint_details": [0.9, 0.8, 0.65, 63.0],
    "mask_gamma": [0.5, 1.0, 0.8, 0.45]
  }
}
```

```json
{ "library": "concrete_wet_01" }
```

- `class` → master: `<master_root>/<class>` (например
  `/Game/Mimir/MasterMaterials/rendinst_perlin_layered`) — **без префикса
  `M_`** (№5). `library` → `<library_root>/<name>` — **без `MI_`** (№6).
- Texture reference в `textures` — ТОЛЬКО logical name текстуры
  (`[a-z0-9_]+`, без расширения и path-разделителей); решение `OPEN-V4-2`.
  Текстура — полноправный kind (§2): резолв идёт по ResourceKey
  `texture:<name>` в границах source-дерева, Project Resource Index (§3) —
  кэш этого резолва, до S4 та же семантика выполняется прямым сканом.
  Каскад 07 §5 (`exact path → unique basename` под `texture_root`) в v4 НЕ
  выживает; путей в JSON нет — перемещение файла текстуры не меняет ссылок;
  понятие `texture_root` растворяется в `source_root`. Scanner
  классифицирует kind `texture` по allowlist расширений: `png, tga, tif,
  tiff, exr, jpg, jpeg, dds, hdr` (расширяется только поправкой owner).
  Same-stem файлы разных расширений — дубликаты одного kind: стандартная
  политика §2, приоритетов форматов нет. Неканоничный token (расширение,
  путь, недопустимые символы) — `MH_E_NONCANONICAL_TEXTURE_REFERENCE`;
  не резолвящийся — `MH_E_UNRESOLVED_TEXTURE_REFERENCE`; оба блокируют
  материал и dependents (коды регистрируются в S2). Writer'ы (Blender
  export, UE Publish) выводят token из stem имени файла/ассета сами и
  fail-closed падают на неканоничном stem; reader ничего не нормализует —
  legacy-нормализация абсолютных путей удалена из scope (миграции нет, §11).
- **Точная грамматика — закрытый набор полей (решение OPEN-V4-6).**
  Class-форма: `class` (обязателен), опционально `twosided`, `textures`,
  `params`. Library-форма: РОВНО одно поле `library`. Ключи `textures` —
  только `tex0`–`tex15` (без ведущих нулей), ключи `params` — `[a-z0-9_]+`.
  Значение `params`: число (UE scalar parameter) или массив РОВНО из 4
  чисел (vector parameter); других форм нет. `twosided` (bool) —
  единственный top-level флаг: это НЕ static switch, а MI Base Property
  Override (TwoSided); writer пишет его только при override, отсутствие =
  значение мастера. Любое неизвестное поле, неверный тип или недопустимый
  ключ — `MH_E_MATERIAL_GRAMMAR` (регистрируется в S2), блок материала и
  dependents; reader никогда не игнорирует неизвестное (ignore = тихая
  потеря данных на Publish). Статические bool-переключатели по-прежнему НЕ
  сериализуются и живут включёнными в мастерах (№7); `tex16support` из
  раннего примера — артефакт черновика, такого поля не существует.
- **Каноническая байт-форма JSON и applied state (решение OPEN-V4-7).**
  Blender writer и UE Publish обязаны выдавать байт-идентичный canonical
  JSON: UTF-8, LF, завершающий перевод строки, 2-пробельный отступ,
  порядок полей `class|library → twosided → textures → params`, ключи
  `textures` по номеру слота, ключи `params` лексикографически; float —
  кратчайшая round-trip десятичная запись (семантика C++
  `std::to_chars`), целые значения без дробной части. Точные байты
  фиксируются ОБЩИМИ golden-векторами, которые читают и pytest, и UE
  Automation. `UMHMaterialSourceData` хранит два хэша (формат — §3):
  `SourceHash` — raw hash применённого `.material`; `AppliedHash` — hash
  канонического JSON, извлечённого из MI сразу после apply тем же
  extractor'ом, что использует Publish (одна канонизация — одна истина).
  Детект локальной правки managed MI: extract сейчас → hash ≠
  `AppliedHash` → `MH_W_MANAGED_ASSET_LOCALLY_MODIFIED`; extract,
  падающий на non-roundtrippable локальном состоянии, тоже считается
  локальной правкой (warning, не блок). Здоровый apply обязан давать
  `extract(MI) == canonical(source)` байт-в-байт — это и есть NO_CHANGE
  acceptance S2.
- **Library-форма строгая (решение OPEN-V4-8).** `{"library": "<name>"}`
  без других полей и без overrides. Publish MI с library-parent и любым
  локальным override (scalar/vector/texture/base property) —
  `MH_E_MATERIAL_NOT_ROUNDTRIPPABLE`: художник очищает overrides или
  переводит материал в class-форму; молчаливый discard запрещён. Импорт
  library-формы — полный apply: reparent на `<library_root>/<name>` и
  очистка локальных overrides (source побеждает; до перезаписи
  срабатывает детект локальной правки). Blender-модель v4 — собственная
  property group mh4blend (`class|library` + поля грамматики); dag4blend
  `is_proxy`/`proxy_path` НЕ читаются и не конвертируются автоматически
  (proxymat-концепция superseded library-формой), `sides` не
  сериализуется: двусторонность выражается только полем `twosided`,
  «real two sided» — дело конкретного класса/params.
- **Publish Material = полная перезапись source-файла без сравнения** (№11):
  extract MI → canonical JSON → sibling tmp → read-back → atomic replace →
  index upsert. Материал без source: диалог «папка + имя» (Adopt). Blender
  симметрично: экспорт обновляет все файлы материалов при включённой опции.
  Three-way diff/merge/`MH_E_MATERIAL_CONFLICT` — УДАЛЕНЫ из scope.
- Импорт: source всегда побеждает; локально изменённый MI перед перезаписью
  получает warning `MH_W_MANAGED_ASSET_LOCALLY_MODIFIED` (не блок).

## 6. Композиты v4 (поправки №8, №10)

`<name>.composite` — JSON-дерево **без какой-либо информации о материалах**.
Kinds узлов: `mesh` (static mesh), `actor` (blueprint/игровой актор),
`composite` (вложенный композит), `group`. Без schema/version/uid полей.

```json
{ "nodes": [
    { "kind": "composite", "resource": "gaz53_a_window_front_cmp",
      "transform": { "translation_cm": [0,0,0],
                     "rotation_quat": [0,0,0,1], "scale": [1,1,1] } },
    { "kind": "mesh", "resource": "gaz53_a_milk_hood",
      "transform": { "translation_cm": [207.5, 155.8, 10.3],
                     "rotation_quat": [0,0,0,1], "scale": [1,1,1] } },
    { "kind": "group", "name": "lights", "transform": { "...": "..." },
      "children": [ { "kind": "actor", "resource": "lamp_point_warm",
                      "transform": { "...": "..." } } ] } ] }
```

- `name` опционален (читаемость/группы); идентичность узла НЕ несёт —
  сравнение композитов whole-resource.
- `actor` резолвится через registry в настройках плагина
  (`ActorClassRegistry: name -> SoftClassPath`); unresolved — блок композита.
- Самовключение и включение любого предка запрещены (cycle detection по
  графу зависимостей).
- Publish Composite — та же семантика, что материал (№11): полная
  перезапись source, read-back, atomic replace; импорт — source побеждает с
  warning при локальной правке. Node-level merge не существует.

## 7. Applied state в ассетах (поправка №9)

- `UMHStaticMeshImportData : UAssetImportData` на UStaticMesh: SchemaVersion,
  LogicalName, SourceRelativePath, SourceRawHash, RecipeHash,
  AppliedAssetHash, LastSuccessfulTransaction. Штатные source-file записи
  (path/timestamp/MD5) — бесплатно от базового класса. Это receipt, не
  identity.
- `UMHMaterialSourceData : UAssetUserData` на MI: LogicalName,
  SourceRelativePath, SourceHash (raw, §3), AppliedHash (canonical
  extract, §5 / решение OPEN-V4-7), ParentClass.
- `UMHCompositeAsset` — собственное поле applied state.
- Asset Registry tags: `MH.Kind`, `MH.LogicalName`, `MH.SourcePath`,
  `MH.AppliedHash`, `MH.Managed` — индекс строит GeneratedAssets из них.
- Commit applied state только после: build успешен → async compilation
  завершена → package сохранён.

## 8. Генерируемые пути UE

Без префиксов типов (обобщение №5–№6):

```text
/Game/MH/Generated/Meshes/<logical_name>
/Game/MH/Generated/Materials/<logical_name>
/Game/MH/Generated/Composites/<logical_name>
/Game/MH/Generated/Textures/<logical_name>
```

Путь детерминирован именем, не source-папкой: перемещение файла не двигает
`.uasset`.

## 9. Политика UE-редактирования

StaticMesh — source-generated: правки в Static Mesh Editor не экспортируются,
перезаписываются следующим reimport, детектятся по `AppliedAssetHash` как
`MH_W_MANAGED_STATIC_MESH_LOCALLY_MODIFIED` (dev: warning; strict/CI: error).
MI и Composite двусторонние только через явный Publish.

## 10. Архитектура импортёра

Stock Interchange НЕ используется как основа. Сохраняется seam:

```text
Autodesk FBX SDK -> IMHGeometryTranslator -> FMHSceneIR
  -> FMHStaticMeshBuildPlan -> FMHStaticMeshBuilder -> UStaticMesh
  -> UMHStaticMeshImportData
```

Порядок импорта: textures → materials → static meshes → composites.
Resolver: `IMHSourceResolver::Resolve(FMHResourceKey)`.

## 11. Миграция (поправка №12)

**Миграции файлов нет**: на v2 экспортирован один тестовый ассет. UID-код
(uid.py, arbitration, passport codecs, Carrier B consensus, node-uid repair,
meshser, миграционные утилиты v1→v2) удаляется сразу, без dual-read и без
migration release. Единственный тестовый ассет пересоздаётся заново.

## 12. Судьба существующих нормативов

| Документ | Судьба |
|---|---|
| 01/05 (bundle/source schema, embedded identity) | superseded (identity, passport, UID) |
| 07 (UE import contract) | superseded в §§ identity/resolver/passport/Ledger/textures; выживают: master registry идея §6, reimport-in-place, Message Log/commandlets; texture rules §5 НЕ выживают — заменены §5 этого документа (решение OPEN-V4-2) |
| ADR_V2 passport-first | superseded целиком |
| ADR_V3 interchange hybrid | superseded целиком (слои состояния и демоция Ledger→Index пересказаны здесь) |
| AMENDMENT_combined_lod | Combined-LOD (один FBX = все LOD, dense levels) выживает; passport/`mh_lod_level`/meshser части superseded |
| AMENDMENT_node_hierarchy r2 | иерархия+parent closure+кости выживают; `mh_uid`/repair части superseded |
| QUESTIONS | UID-вопросы закрываются как superseded |
| C1 (PR #7) | seams выживают; resolver/detector реализации заменяются по v4 |
