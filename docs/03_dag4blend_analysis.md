# dag4blend 2.12.0 → модель для Blender→UE5 composite pipeline

> Исторический research-документ. При расхождении приоритет у Decision Log
> в `00_research_summary.md`, workflows `04_source_workflows.md` и замороженной
> Source Schema v1 `05_source_schema_v1.md`.

Разбор по реальному коду плагина (`cmp/cmp_import.py`, `cmp/cmp_export.py`, `cmp/composite_functions.py`, `cmp/cache_rw.py`, `cmp/node_properties.py`, `cmp/cmp_panels.py`, `colprops/colprops.py`, `object_properties/`, `dagormat/`, `exporter/exporter.py`).

---

## 1. Как composite реально устроен в dag4blend

### 1.1 Blender-представление (authoring model)

| Dagor-сущность | В Blender | Где лежит |
|---|---|---|
| composit ресурс | `Collection`, `col['type'] = 'composit'`, без под-коллекций (иначе `cmp_export` отказывается экспортировать) | сцена `COMPOSITS` |
| rendinst / prefab ресурс | `Collection` `name.lods` (`type='rendinst'`) с дочерними `name.lod00`, `name.lod01`... | сцена `GEOMETRY` |
| gameobj ресурс | `Collection`, `type='gameobj'` | сцена `GAMEOBJ` |
| **node** (placement) | `Empty`, `instance_type='COLLECTION'`, `instance_collection → ресурс` | внутри composit-collection |
| безымянный node (transform group) | `Empty` без instance_collection, дети — через object parenting | там же |
| **ent[]** (variant set) | node.instance_collection = `random.NNN` collection; внутри — безымянные Empty, каждый со своим `instance_collection` и `dagorprops['weight:r']` | сцена `TECH_STUFF` |
| параметры node (`place_type:i`, `placeOnCollision:b`, `ignoreParentInstSeed:b`, offset/rot/scale…) | `obj.dagorprops` (ID-props, ключ с типовым суффиксом `:t/:i/:b/:r/:p2/:m…`) | на Empty |
| override имени ресурса | `col['name']` (colprops) — потому что Blender вешает `.001` на дубли имён | на коллекции |

Иерархия composite = **object parenting Empty→Empty**, а не иерархия коллекций. Коллекции — только ресурсы. Это и есть "граф ресурсов + дерево placements".

### 1.2 Формат `.composit.blk` (то, что реально пишет/читает плагин)

```
className:t="composit"

node{
  name:t="asset_name:rendinst"      // ресурс + тип; тип может отсутствовать → resolve через cache
  tm:m=[[..][..][..][x y z]]        // либо tm...
  // ...либо рандомизированные offset_x:p2 / rot_y:p2 / scale:p2 / yScale:p2 — взаимоисключающе с tm
  place_type:i=...
  placeOnCollision:b=...
  ignoreParentInstSeed:b=...
  node{ ... }                        // дети
}

node{                                // "random" node
  ent{ name:t="a:rendinst"; weight:r=0.6; }
  ent{ name:t="b:composit"; }
  tm:m=...
}
```

Существенно:
- Идентичность **только имя** (`name:t`, lowercase). Плюс имя допускает один и тот же asset name сразу с двумя типами (`entities[name] = [['rendinst',path],['composit',path]]`).
- `tm` и рандомные offsets **взаимоисключающи** (`rand_tf` в `cmp_const.py`; при импорте `found_tm=False` → transform lock + offsets; UI-переключатель `use_tm:b`).
- Nested composit хранится **как ссылка по имени** (`name:t="x:composit"`), рекурсивно импортируется в отдельную коллекцию один раз (dedup через `dict.fromkeys`, повторный импорт пропускается если коллекция уже наполнена).
- Ось: Dagor Y-up, конвертация вручную в `apply_matrix`/`get_matrix` (в `apply_matrix` есть баг: `offset_y:p2` обрабатывается дважды, `offset_z:p2` — никогда). Для нас нерелевантно: FBX transport делает axis conversion сам.

### 1.3 Resource resolver (`cache_rw.py`)

`build_cache()` — рекурсивный обход папки проекта, ключ = имя файла до первой точки, lowercase. Тип угадывается по расширению (`.lod00.dag` → rendinst, `.dag` без точек → prefab, `.composit.blk`, `.gameobj.blk`). Результат pickle-ится в `<addon>/<project>.bin`.
Композит **не знает путей** — только имена. Это правильная идея; реализация (глобальный кеш, filesystem walk, name-only) — нет.

### 1.4 Explode / rebuild (`composite_functions.py`)

- `nodes_split()` — копирует объекты из `instance_collection` как реальных детей Empty, ставит `instance_type='NONE'`, **но оставляет `instance_collection` на Empty**. Именно поэтому возможен `node_revert()` (удалить детей, вернуть `instance_type='COLLECTION'`) и `node_rebuild()` (затолкать детей обратно в коллекцию-ресурс = обновить определение ресурса для всех пользователей).
- `nodes_to_composite()` — из выделенных nodes создать новый ресурс с pivot в parent_node (перебазирование матриц через `matrix_offset = parent_node.matrix_world.inverted()`).
- `node_make_unic()` — размножить `random.NNN` коллекцию для локального редактирования варианта.
- Для random-нод `node_rebuild` принудительно обнуляет transform детей: у entities не бывает собственных offsets.

Это тот самый паттерн "Expand instance без разрушения ссылки" — брать целиком.

### 1.5 Материалы (`dagormat/`) и объектные props (`object_properties/`)

- `dagormat`: `shader_class` (валидируется по `dagorShaders.blk` — конфиг шейдеров с типизированными props, default/soft_min/soft_max/description), `textures.tex0..tex15`, `optional` — типизированные custom props из конфига, `is_proxy` → в .dag пишется `name:proxymat` (материал-**ссылка** на внешний материальный ассет). Это прямой аналог "MI создаётся из metadata по schema, либо ссылка на существующий MI".
- Объектные `dagorprops` на мешах: `renderable:b`, `cast_shadows:b` уходят в флаги node, остальное — в node script (`gatherObjPropScript`). Невалидные — в `broken_properties:t`, не теряются. Хороший принцип: **неизвестные свойства транспортируются, а не выбрасываются**.
- Импорт: `col['name']`/`col['type']` и `dagorprops` — единственные места, где плагин хранит семантику; всё остальное — соглашения по именам (`.lods`, `.lod00`, `random.`, `node.`).

---

## 2. Что из этого брать в UE-pipeline

### 2.1 Таблица соответствия

| dag4blend / .composit.blk | Наша модель (`UMHCompositeAsset` / FBX protocol) | Статус |
|---|---|---|
| Collection `type='composit'` | `UMHCompositeAsset` (Content Browser asset, `UAssetDefinition`) | v1 |
| Collection `type='rendinst'` (`.lods` + `.lodNN`) | D39: один Mesh Resource, LOD0 в primary FBX, LOD1+ отдельными FBX через manifest `lods[]` | v1 |
| Collection `type='gameobj'` | `Kind = Actor`, `ActorClass` soft ref | v3 |
| Empty + `instance_collection` | `FMHCompositeNode { NodeUID, ResourceUID, LocalTransform, Kind }` | v1 |
| Empty без ресурса + parenting | `Kind = Group` (transform-only node) | v1 |
| `name:t="x:composit"` (nested ref) | `Kind = CompositeRef`, `Resource = SoftObjectPath(CA_x)`, **не разворачивать** | v4 |
| `random.NNN` + `ent{ weight }` | `Kind = VariantSet`, `Variants[] { ResourceUID, Weight }` | резерв в схеме, реализация позже |
| `tm:m` | `LocalTransform` | v1 |
| `offset_*/rot_*/scale/yScale` (взаимоискл. с tm) | `TransformDistribution` (optional; при наличии — `LocalTransform` игнорируется, как в Dagor) | резерв |
| `place_type:i`, `placeOnCollision:b`, `ignoreParentInstSeed:b` | `PlacementPolicy` | резерв |
| `col['name']` override (борьба с `.001`) | `ResourceUID` + `DisplayName` — override становится не нужен | v1 |
| `name:t` как идентичность | **не копировать**: имя = display/fallback, идентичность = GUID | v1 |
| `cache_rw` filesystem cache | Asset Registry + `UMHSceneSourceAsset` manifest (UID→SoftObjectPath) | v2 |
| `dags_to_import`/`cmp_to_import` globals, рекурсия | Dependency graph, топологический порядок, cycle detection | v4 |
| `nodes_split / node_revert / node_rebuild` | Composite Editor: Expand / Collapse / Push definition — ссылка сохраняется | v3–v4 |
| `nodes_to_composite` | "Make Composite from selection" в Blender-аддоне и в UE editor | v3 |
| `dagormat.shader_class` + `dagorShaders.blk` | Material schema (`ShaderGroup` → parent material + typed params) — у вас уже есть аналог в JSON metadata | v2 |
| `is_proxy` → `name:proxymat` | `Kind = MaterialRef` → существующий MI, не генерировать | v2 |
| `broken_properties:t` | Unknown metadata keys → сохраняются в `FMHPropertyBag` как raw, с warning в Preview | v1 |
| `renderable:b`, `cast_shadows:b` на объекте | `render.visible`, `render.cast_shadow` в metadata | v1 |

### 2.2 Что в Dagor-плагине сделано соглашениями по именам и что мы делаем явными полями

| Соглашение в dag4blend | Явное поле у нас |
|---|---|
| префикс `random.` | `mh_kind = "variant_set"` |
| точный root `<base>.lods` + direct children `<base>.lodNN` | `static_mesh` row: `.lod00` → `source`, `.lod01+` → отдельные payload-файлы в `lods[]` |
| `name:type` в строке | `mh_kind` + `mh_resource_uid` |
| `node.` префикс на Empty при импорте | `mh_kind = "group"` |
| Empty vs Mesh как признак node/geometry | `mh_kind` на всём, что имеет семантику |

Причина: `nodes_split` уже сейчас вынужден делать `re.search("\.lods($|\.\d*)")`, `search("^random\.\d*")` и т.п. — вся семантика восстанавливается регэкспами по именам. Для FBX-транспорта в UE, где импортер сам мангли имена, это гарантированно ломается. D39 оставляет одно точное authoring-правило на границе Blender, но переносит результат в явные `source`/`lods[]`: UE не угадывает ресурсную структуру общими regex по FBX nodes.

### 2.3 Активация замороженного LOD-контракта (D39)

Выбор Collection с точным именем `<base>.lods` означает один logical
`static_mesh` resource. Direct child `<base>.lod00` экспортируется в primary
`.mesh.fbx`; каждый `<base>.lod01+` — в отдельный `.lod<level>.mesh.fbx`.
Manifest-row имеет `lod_policy: "authored"`, primary `source` и `lods[]` с
`level`, `source`, `content_hash` для уровней 1+. Custom UE importer добавляет
эти payload-файлы как уровни одного UStaticMesh.

Это не `FbxLODGroup`, не один packed FBX и не Empty/Null hierarchy. Каждый
per-level payload имеет собственный semantic hash, hash-skip и recovery в
рамках одной resource-row. Суффикс `.lods` снимается только при вычислении
логического имени перед ASCII-валидацией; generic dots остаются ошибкой. Так
D39 активирует, но не меняет замороженные поля Source Schema v1 из D13/Q5.

---

## 3. Схема FBX custom properties (протокол Blender→UE)

Минимум для v1, чтобы Analyzer мог восстановить Resource/Placement/Composite/Instance без опоры на имена:

```
# на каждом семантическом узле (Empty или Mesh object)
mh_schema        : int     = 1
mh_node_uid      : string  GUID   (уникален на placement)
mh_kind          : string  "composite_root" | "mesh" | "composite_ref" | "group" | "variant_set" | "variant" | "actor"
mh_resource_uid  : string  GUID   (общий для всех placements одного ресурса; для mesh = UID mesh data, не объекта)
mh_resource_name : string  display / package name fallback

# опционально
mh_weight        : float   (kind=variant)
mh_actor_class   : string  (kind=actor)
render.*, collision.*, material.*  — ваша существующая metadata схема
```

Правила экспорт-компилятора Blender-аддона (аналог того, что dag4blend делает в `write_node`, но в FBX):
1. Collection-instance Empty → в FBX пишется Null-node с `mh_kind=composite_ref|mesh`, **геометрия ресурса в FBX не дублируется** для каждого placement (иначе legacy импортер создаст N мешей).
2. Каждый ресурс (Collection) экспортируется один раз в отдельную ветку `MH_RESOURCES/<uid>` (или в отдельный FBX — решается на тестах из §5).
3. UID на mesh data и на Collection хранятся как custom properties data-блока и **дублируются на объект** при экспорте — потому что уверенность в переносе props с mesh data через Blender FBX exporter ниже, чем с объекта.
4. `random.NNN` → Null `variant_set` с детьми `variant`.
5. Rename не трогает UID; аддон при копировании объекта (Ctrl+D) **обязан** выдать новый `mh_node_uid`, но сохранить `mh_resource_uid` (linked duplicate) — это ровно та точка, где Blender сам ничего не гарантирует, нужен handler на `depsgraph_update_post` или проверка перед экспортом (дубли node_uid → ошибка экспорта).

---

## 4. Что из dag4blend НЕ переносить (и почему — по коду)

1. **Name-only identity + `col['name']` override.** Плагину пришлось добавить отдельный оператор `DT_OT_SetColName` только чтобы обойти Blender-суффиксы. С GUID проблема исчезает.
2. **Глобальный filesystem cache** (`pickle` в папке аддона, per-project). В UE есть Asset Registry; в Blender-аддоне — manifest, выгружаемый UE (UID → asset path), а не walk по диску.
3. **Модуль-глобалы `dags_to_import`/`cmp_to_import`** и неограниченная рекурсия `read_cmp` без cycle detection.
4. **Неявная семантика через имена** (`random.`, `node.`, `name:type`). Точная
   `.lods`/`.lodNN` authoring-структура D39 — ограниченный входной convention;
   на диске её смысл выражен manifest `source`/`lods[]`.
5. **Ручная axis conversion** — FBX делает.
6. **Плоские composit-коллекции** (запрет под-коллекций в `cmp_export`) — для авторинга в Blender можно разрешить группировку коллекциями, но экспорт-компилятор всё равно должен сплющить это в однозначную FBX-иерархию.

---

## 5. Тесты, которые нужно прогнать ДО архитектуры (день работы)

| # | Вопрос | Как проверить | Что зависит |
|---|---|---|---|
| T1 | Доходят ли custom props объекта **и mesh data** через Blender FBX → legacy `Import Into Level` до `UStaticMesh` (editor metadata / AssetImportData) | Сцена: 1 куб, props на obj и на data, импорт, `GetMetaData` в Python | Возможен ли UID-мост "Interchange-анализ + legacy-импорт" |
| T2 | Создаёт ли legacy scene import **один** SM для linked duplicates (shared mesh data) с разными transforms/материалами | 3 linked-объекта, 1 с модификатором | Нужен ли собственный дедуп по `mh_resource_uid` в Finalize (скорее да) |
| T3 | Совпадают ли имена/transforms между `UInterchangeFbxTranslator::Translate()` и legacy на реальной сцене (точки, пробелы, unit scale, axis) | Прогнать оба, сравнить дампы | Два парсера или один |
| T4 | Переживают ли custom props Null-ноду (Empty) через legacy import (нужны для composite_ref/group) | Empty с props → есть ли Actor/компонент с metadata | Можно ли строить composite-граф из legacy результата, или только из анализатора |
| T5 | Blender FBX exporter + Collection Instance: что реально выгружается (разворачивает ли инстансы в копии геометрии?) | Экспорт 3 инстансов, посмотреть FBX (`FbxMesh` count) | Обязателен ли собственный экспорт-компилятор в аддоне (§3 п.1) |

Ответ на T5 почти наверняка "разворачивает" — Blender FBX exporter не поддерживает collection instances как references. Это главный аргумент за то, что аддон должен **сам** строить FBX-иерархию из семантической модели, как dag4blend сам пишет `.composit.blk` из Empty-графа, а не полагаться на стандартный экспорт коллекций.

---

## 6. Данные схемы `UMHCompositeAsset` с учётом всего выше

```cpp
UENUM() enum class EMHNodeKind : uint8 { Group, Mesh, CompositeRef, Actor, VariantSet, Variant };

USTRUCT() struct FMHVariant        { FGuid ResourceUID; float Weight = 1.f; };
USTRUCT() struct FMHTransformDist  { FVector2f OffsetX, OffsetY, OffsetZ, RotX, RotY, RotZ, Scale, YScale; bool bEnabled = false; }; // резерв
USTRUCT() struct FMHPlacementPolicy{ int32 PlaceType = -1; bool bPlaceOnCollision = false; bool bIgnoreParentInstSeed = false; };    // резерв

USTRUCT() struct FMHCompositeNode
{
    FGuid                 NodeUID;
    FGuid                 ParentUID;
    FString               DisplayName;
    EMHNodeKind           Kind;
    FTransform            LocalTransform;
    FGuid                 ResourceUID;              // Mesh / CompositeRef / Actor
    FSoftObjectPath       Resource;                 // резолвится через manifest / Asset Registry
    TArray<FMHVariant>    Variants;                 // Kind == VariantSet
    FMHTransformDist      TransformDistribution;    // резерв
    FMHPlacementPolicy    PlacementPolicy;          // резерв
    FMHPropertyBag        Properties;               // resolved render/collision/material + raw unknown keys
};
```

Compiled representation (ISM/HISM/Actor/LevelInstance) — только в `Compiler`, никогда не в этих полях. Это прямое следствие Dagor-модели: `.composit.blk` не знает, как движок его инстансит.
