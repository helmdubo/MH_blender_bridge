# 05 — Source Schema v1 (NORMATIVE FINAL v1)

Статус: **единственный нормативный замороженный on-disk контракт Source Schema
v1**. Документ учитывает решения `[composite-graph-source]`,
`[texture-path-canonicalization]`, `[uid-source-resolution]` и
`[material-source-files]`. Противоречащие ему pre-freeze описания bundle,
inline `materials[]`, `external_dependencies[]` и directory-local-only resolve
являются историческими.

Source Schema v1 заморожена. Любое изменение формы JSON,
required/optional-полей, канонизации, path semantics, hash input или
машинного кода, меняющее совместимость, требует `schema_version: 2` и явной
migration note. Директивы аудитора ссылаются на решения по slug; числовые D-ID
назначает репозиторий.

Post-freeze D38 `[fbx-export-materials-toggle]` — совместимое operational/UX
amendment: одна вкладка `MH` и Boolean `Export Materials` лишь оркестрируют уже
описанные standalone writers. Ни одна JSON-форма, canonical byte, hash input или
диагностическая семантика этого документа не изменена.

**Approved schema-significant amendment — 2026-08-18.** D40
`[combined-lod-fbx]` и `AMENDMENT_combined_lod_fbx.md` немедленно заменяют только
per-file LOD-части этого frozen v1 документа. До формального объявления Source
Protocol v2 это approved migration contract: новый writer пишет один FBX на
mesh-ресурс, `lods[]` больше не эмитится, а geometry stream имеет tag
`mh.meshser:2`. Остальные v1 JSON-формы сохраняют силу. Старые выгрузки с
`lods[]` мигрируются реэкспортом и после миграции блокируются
`MH_E_DEPRECATED_LOD_ROWS`; тихая интерпретация старой формы запрещена.

## 1. Инварианты

1. Identity ресурса и узла задаётся полным lowercase UUID с дефисами. Имя,
   filename, каталог и порядок в JSON не являются identity.
2. `.composite` задаёт граф. Resolver находит payload по ResourceUID. FBX только
   наполняет geometry definition и никогда не является источником composite
   nodes, `parent_uid`, placement transforms или node properties.
3. `export_manifest.json` — инкрементальная опись ресурсов, принадлежащих одному
   каталогу, а не bundle, snapshot сцены или список dependency closure.
4. Зависимости не хранятся денормализованным массивом: composite dependencies
   вычисляются из `nodes[].resource_uid`, material dependencies — из
   `material_slots[].material_uid`.
5. Один ResourceUID имеет ровно один owning manifest под `source_root`. Resolve
   считается завершённым только результатом
   `(payload_path, owning_manifest_path, manifest_row)`.
6. Материал — самостоятельный `.material` payload и обычная строка
   `kind: "material"` в `resources[]`.
7. Текстуры только упоминаются путём. Экспорт не копирует, не перемещает и не
   модифицирует texture files.

## 2. Project Source Root

`source_root` — один абсолютный каталог на проект, заданный в настройках, но
**не записываемый** в manifest, composite или material. Он служит:

- границей поиска manifests и resource payloads;
- базой registry paths;
- базой канонизации internal texture paths;
- базой зеркалирования структуры в Content Browser.

Resource payload и owning manifest обязаны находиться под `source_root`.
Resolver не ищет resource payloads вне корня. Исключение относится только к
texture paths: в transitional-режиме внешний texture file допустим как
абсолютная ссылка (§5.3).

## 3. `export_manifest.json`

### 3.1 Полная форма

```json
{
  "schema": "mh.export_manifest",
  "schema_version": 1,
  "exporter_version": "0.4.0",
  "resources": [
    {
      "uid": "2db5574c-3aca-43cc-9ab5-8242403e18cd",
      "kind": "static_mesh",
      "name": "wall_a",
      "source": "meshes/wall_a__2db5574c.mesh.fbx",
      "content_hash": "xxh3:9f2c01ab34cd56ef",
      "material_slots": [
        {
          "slot_name": "m_stucco_concrete",
          "material_uid": "7d995e54-d084-4466-a613-a1cd8f3248b2"
        }
      ],
      "properties": {
        "role": "wall"
      },
      "lod_policy": "authored"
    },
    {
      "uid": "7d995e54-d084-4466-a613-a1cd8f3248b2",
      "kind": "material",
      "name": "m_stucco_concrete",
      "source": "materials/m_stucco_concrete__7d995e54.material",
      "content_hash": "xxh3:67c9db59a8b33f0d"
    },
    {
      "uid": "f53d93af-94c3-472f-98d0-ff36eb93c417",
      "kind": "composite",
      "name": "window_set_a",
      "source": "composites/window_set_a__f53d93af.composite",
      "content_hash": "xxh3:aa010fa05a09dabc",
      "properties": {
        "role": "facade_module"
      }
    }
  ]
}
```

Top-level required fields:

| Поле | Тип | Правило |
|---|---|---|
| `schema` | string | Ровно `mh.export_manifest`. |
| `schema_version` | integer | Ровно `1`. |
| `exporter_version` | string | Версия writer; не участвует в identity. |
| `resources` | array | Строки отсортированы по `uid` побайтово. |

Других top-level полей v1 нет. В частности, запрещены `bundle_uid`,
`bundle_name`, `source`, `materials` и `external_dependencies`.

### 3.2 Общая resource row

Required для каждого kind:

| Поле | Тип | Правило |
|---|---|---|
| `uid` | UUID string | Уникален во всём `source_root`, не только в файле. |
| `kind` | enum | `static_mesh`, `composite` или `material`. |
| `name` | string | Display/resource name; не identity. |
| `source` | string | Относительно каталога owning manifest, forward slashes. |
| `content_hash` | string | `xxh3:` + 16 lowercase hex. |

`source` обязан быть непустым нормализованным относительным путём без drive,
leading slash, `.` или `..`; после разрешения он остаётся внутри каталога
owning manifest и внутри `source_root`. Один source path не может принадлежать
двум UID. Kind существующего UID не меняется.

Kind определяет точный lowercase suffix `source` и имя при первом создании:

| Kind | Suffix | First-create basename |
|---|---|---|
| `static_mesh` | `.mesh.fbx` | `<sanitized_name>__<uid8>.mesh.fbx` |
| `composite` | `.composite` | `<sanitized_name>__<uid8>.composite` |
| `material` | `.material` | `<sanitized_name>__<uid8>.material` |

Reader проверяет suffix по kind и обязательный basename-фрагмент
`__<uid8><suffix>` по полному row UID, но не требует, чтобы устаревшая
filename-часть `sanitized_name` совпадала с текущим row `name`. Полный UUID
остаётся identity; совпадение первых восьми hex у двух ResourceUID под одним
`source_root` блокируется `MH_E_UID8_COLLISION`.

Manifest row и payload согласованы: UID/name внутри `.composite` или
`.material` равны row UID/name. Для FBX identity задаёт manifest row, потому
что FBX не является семантическим UID-контейнером.

### 3.3 Kind-specific поля

`static_mesh` допускает:

- optional `material_slots`, default `[]`: массив
  `{ "slot_name": string, "material_uid": UUID }`; порядок — первое
  вхождение после детерминированной сортировки ObjectUID, затем slot index;
  один MaterialUID повторно не добавляется; один `slot_name` не может указывать
  на разные UID;
- optional `properties`, default `{}`: asset-level JSON bag;
- optional `lod_policy`: `authored | generated | nanite`, default `generated`;
- поле `lods` deprecated и новым writer не эмитится. После миграции любое его
  присутствие — `MH_E_DEPRECATED_LOD_ROWS`. `lod_policy: authored` означает,
  что все уровни находятся внутри единственного FBX из общего `source` row.

`composite` допускает optional `properties`, default `{}`: asset-level bag.
Node/placement properties живут только в `.composite`; они не наследуются из
resource bag и не дублируют его.

`material` не имеет kind-specific полей в manifest; в частности, manifest-row
kind `material` **не допускает `properties`**. Asset-level semantic properties
материала живут в `.material.params`; `shader_class`, `params` и `textures`
принадлежат только `.material` payload. Это уточнение не добавляет поле в v1.

Для допускающих её rows (`static_mesh`, `composite`) `properties` — единственная
открытая resource extension bag. Прочие неизвестные поля row не являются v1
extension mechanism.

### 3.4 Hash inputs

- `static_mesh`: XXH3-64 потока `mh.meshser:2` evaluated geometry всех LOD;
  не FBX bytes. Объекты идут по `(lod_level asc, ObjectUID asc)`, перед данными
  каждого объекта в поток пишется `lod_level` как little-endian `uint32`;
- `composite`: XXH3-64 канон-формы всего `mh.composite` document;
- `material`: XXH3-64 канон-формы semantic payload
  `{shader_class, params, textures}`. UID/name/schema исключены, поэтому rename
  остаётся `RENAME`, а не `UPDATE_PROPERTIES`.

`content_hash` — semantic fast path, а не доказательство byte equality. В
частности, при material rename writer обновляет `name` в существующем payload,
хотя material `content_hash` не изменился.

### 3.5 Combined-LOD FBX amendment

Dagor-authoring остаётся `<base>.lods` с direct children `<base>.lod00`,
`<base>.lod01`, …, но это только Blender-side convention. Writer экспортирует
объекты всех уровней одним вызовом FBX exporter в один `source`. На каждом
экспортируемом mesh-узле записывается integer custom property `mh_lod_level`;
отсутствие свойства означает уровень 0 только для single-LOD ресурса. Имена
узлов и suffix `.lodNN` внутри FBX не являются семантикой.

Номера уровней обязаны быть плотными от 0 (`MH_E_LOD_LEVELS_SPARSE`). Material
slots уровней 1+ — подмножество slots уровня 0
(`MH_E_LOD_SLOT_NOT_IN_BASE`). `UCX_*` и `SOCKET_*` принадлежат уровню 0;
обнаруженные на уровне 1+ игнорируются с `MH_W_LOD_AUX_NODE_IGNORED`. Несколько
render mesh-узлов на одном уровне разрешены. Любая правка любого уровня меняет
общий geometry hash и приводит к полной перезаписи единственного FBX.

## 4. `*.composite` — `mh.composite` v1

Форма сохранена без изменений:

```json
{
  "schema": "mh.composite",
  "schema_version": 1,
  "uid": "f53d93af-94c3-472f-98d0-ff36eb93c417",
  "name": "window_set_a",
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
    },
    {
      "node_uid": "a0ccf18c-2e7a-4270-8cf2-36505a060e3d",
      "parent_uid": null,
      "kind": "composite_ref",
      "display_name": "lamp",
      "resource_uid": "e3ba6783-bc26-4787-a608-3133cad0d0eb",
      "local_transform": {
        "translation_cm": [20.0, 0.0, 0.0],
        "rotation_quat": [0.0, 0.0, 0.0, 1.0],
        "scale": [1.0, 1.0, 1.0]
      },
      "properties": {
        "role": "lighting"
      }
    }
  ]
}
```

Top-level required: `schema`, `schema_version`, `uid`, `name`, `nodes`.
`schema` равна `mh.composite`, version равна `1`. Nodes — flat table;
иерархия задаётся `parent_uid`. Порядок массива семантики не несёт; writer
сортирует по `node_uid`.
Других top-level полей `.composite` v1 нет; unknown key даёт
`MH_E_INVALID_COMPOSITE`.

Required для каждого node: `node_uid`, `parent_uid`, `kind`, `display_name`,
`local_transform`, `properties`. `node_uid` уникален внутри composite.
`parent_uid` — `null` или UID узла того же файла; dangling parent и parent cycle
запрещены.
Unknown node fields также запрещены. Единственная открытая extension bag узла —
`properties`; дополнительно разрешены только перечисленные ниже kind-specific
fields. Reader не сохраняет неизвестный top-level key «на будущее».

Kind rules:

- `group`: `resource_uid` отсутствует;
- `mesh`: required `resource_uid` static-mesh definition;
- `composite_ref`: required `resource_uid` composite definition;
- `variant_set`, `variant`, `actor` и их ранее зарезервированные поля остаются
  зарезервированными v1 spellings, но не входят в поддерживаемый authoring/import
  slice и не должны интерпретироваться эвристически.

Зарезервированная форма variant selection (optional только для соответствующего
reserved kind) остаётся:

```json
{
  "kind": "variant_set",
  "variants": [
    {
      "resource_uid": "5839a2e1-7118-41a7-a226-0edbcc6941da",
      "weight": 1.0
    }
  ],
  "seed_policy": "inherit",
  "seed_salt": 0
}
```

Зарезервированная actor-форма:

```json
{
  "kind": "actor",
  "actor_resource_uid": "c38bf56d-39a8-42c0-a4e1-565fb1fd2c83",
  "cached_soft_class_path": "/Game/Gameplay/BP_PhysicsDoor"
}
```

Эти fragments дополняют, а не заменяют общие required node fields. Их runtime
семантика остаётся post-v1-slice. Пока отдельный contract не реализован,
Blender export/import и UE import блокируют `variant_set`, `variant` и `actor`
кодом `MH_E_UNSUPPORTED_NODE_KIND`; reader не превращает их в mesh по имени.

`local_transform` содержит ровно три required поля: `translation_cm` (3 числа,
UE centimeters), `rotation_quat` (4 числа x,y,z,w, normalized и
sign-canonicalized), `scale` (3 числа, каждый строго больше нуля). Конвенция:
Z-up, left-handed. `properties` — открытый placement-level JSON bag;
unknown keys и `null` транспортируются как данные.

Composite references вычисляют dependency edges только по
`nodes[].resource_uid`. Никаких имён/узлов из FBX для этого не читается.

## 5. `*.material` — `mh.material` v1

### 5.1 Полная форма

Filename при первом создании:
`<sanitized_name>__<uid8>.material`.

```json
{
  "schema": "mh.material",
  "schema_version": 1,
  "uid": "7d995e54-d084-4466-a613-a1cd8f3248b2",
  "name": "m_stucco_concrete",
  "shader_class": "rendinst_perlin_layered",
  "params": {
    "mask_gamma": [0.1, 1.0, 1.0, 1.0],
    "micro_detail_layer": 0,
    "sides": 0
  },
  "textures": {
    "tex0": "manmade_common/textures/whitewash_plain_a_tex_d.tif",
    "tex2": "C:/DagorLibrary/textures/whitewash_plain_a_tex_n.tif"
  }
}
```

Все семь полей required:

| Поле | Тип | Правило |
|---|---|---|
| `schema` | string | Ровно `mh.material`. |
| `schema_version` | integer | Ровно `1`. |
| `uid` | UUID string | Совпадает с owning manifest row. |
| `name` | string | Текущее display name; совпадает с manifest row. |
| `shader_class` | non-empty string | Dagormat shader либо fallback `rendinst_simple`. |
| `params` | object | JSON-compatible semantic parameters; может быть `{}`. |
| `textures` | object | Непустые slots `tex0`…`tex15` → string; может быть `{}`. |

Других top-level полей `.material` v1 нет. Неизвестный top-level key —
`MH_E_INVALID_MATERIAL_VALUE`; расширяемый passthrough живёт только внутри
`params`. Поэтому hash exclusion для UID/name/schema не превращает неизвестные
данные в unhashed semantics.

Пустой texture slot отсутствует. `params` сохраняет незнакомые dagormat values
как passthrough. Node tree не является альтернативным metadata source. Материал
без dagormat либо с пустым shader сериализуется как `rendinst_simple` с пустыми
`params` и `textures`.

### 5.2 Filename и rename

Правила первого имени resource payload:
`<sanitized_name>__<uid8><extension>`, где `uid8` — первые восемь hex полного
UID. Уникальность обеспечивает UID, а не display name. Соответствие
`sanitized_name` текущему `name` **не требуется и не проверяется**.

После первого material export cascade находит существующий UID, и existing
location всегда выигрывает: повторный export обновляет тот же payload и тот же
owning manifest. Автоматически переименовывать файл запрещено. Rename меняет
`name` внутри `.material` и manifest row, но старое display name в filename
легально. Ручная транзакционная утилита `Rename file to match` — только ROADMAP.

### 5.3 Texture path canonicalization

Перед записью Blender path `//...` разворачивается относительно сохранённого
`.blend`. `//` при несохранённом `.blend` — `MH_E_INVALID_MATERIAL_VALUE`.
Затем каждый непустой texture path нормализуется:

1. Обычный relative authored path сначала разрешается относительно
   `source_root`; абсолютный path используется как есть.
2. Если нормализованный абсолютный файл находится под `source_root`, на диск
   пишется путь относительно `source_root` с forward slashes.
3. Если файл вне `source_root`, на диск пишется нормализованный абсолютный путь
   с forward slashes (`C:/...` или `/...`). Это относится и к authored relative
   path, который после схлопывания `..` вышел за root.
4. Поле `external_path` не существует. Internal/external — вычисляемое свойство:
   path является internal **тогда и только тогда**, когда строка относительная.
   Абсолютная authored-строка, указывающая внутрь root, всегда переводится в
   единственную относительную форму.

Нормализация лексическая и не требует существования файла: переменные среды и
`~` не разворачиваются, разделители становятся `/`, сегменты `.`/`..`
схлопываются без обхода symlink. Для Windows drive letter приводится к upper
case, регистр остальных компонентов сохраняется; UNC сохраняет форму
`//server/share/...`. Проверка containment под `source_root` case-insensitive
для Windows drive/UNC и case-sensitive для POSIX. Trailing slash удаляется,
кроме самого filesystem root. Эти правила одинаковы в Python и UE readers.
Валидная on-disk relative строка уже нормализована, не содержит `.`/`..` и при
разрешении относительно `source_root` остаётся внутри root. Reader не исправляет
нарушение молча: malformed relative path даёт `MH_E_INVALID_MATERIAL_VALUE`.

Для одной internal texture канон-строка не зависит от абсолютного расположения
`source_root` на машине. Текстуры не копируются.

Настройка `texture_policy` не входит в on-disk JSON:

- `transitional` (default): absolute external path допустим и даёт
  `MH_W_TEXTURE_OUTSIDE_ROOT`;
- `strict`: тот же факт даёт `MH_E_TEXTURE_OUTSIDE_ROOT` и блокирует операцию.

Warning/error — диагностика операции, не часть `.material` и не hash input.
Импорт повторно вычисляет internal/external из строки и выдаёт ту же диагностику
по текущей policy, не переписывая файл.

## 6. Resolver и `mh.registry` v1

### 6.1 Registry hint

Registry ускоряет первичный candidate lookup, но не является authority:

```json
{
  "schema": "mh.registry",
  "schema_version": 1,
  "resources": [
    {
      "uid": "7d995e54-d084-4466-a613-a1cd8f3248b2",
      "kind": "material",
      "name": "m_stucco_concrete",
      "source_path": "common/materials/m_stucco_concrete__7d995e54.material",
      "manifest_path": "common/export_manifest.json"
    }
  ],
  "shader_classes": [
    "rendinst_simple",
    "rendinst_perlin_layered"
  ]
}
```

Resource hint required fields: `uid`, `kind`, `name`, `source_path`.
`source_path` — payload path относительно `source_root`. Optional
`manifest_path` — путь owning-manifest candidate относительно `source_root`.
Оба path — normalized relative forward-slash paths без `.`/`..`.
Дополнительные registry sections D28 допустимы.

Registry row, включая optional `manifest_path`, лишь hint: она подтверждается
полным scan всех `export_manifest.json` под `source_root`. Stale/invalid row
даёт warning и fallback к scan. Registry никогда не отменяет проверку unique
owner и не может самостоятельно завершить resolve.

### 6.2 Полный scan и результат

Scan читает только файлы с basename `export_manifest.json`; payload files по
расширению не угадываются. Дисковый cache v1 отсутствует. Для каждого UID:

1. найти все manifest rows;
2. потребовать ровно один owning manifest;
3. проверить row schema/kind/source и разрешить source относительно владельца;
4. проверить существование и тип payload; для `.composite`/`.material` также
   проверить внутренние UID/name;
5. вернуть honest result:

```text
uid -> {
  payload_path,
  owning_manifest_path,
  manifest_row
}
```

Ноль owners = unresolved. Два owners одного UID =
`MH_E_AMBIGUOUS_RESOURCE_OWNER`; выбор первого по порядку файлов запрещён.
Несовпадение expected kind также блокирует resolve. Один ресурс, встреченный N
раз в dependency graph, резолвится один раз и материализуется в одну definition
Collection.

### 6.3 Recursive import

В v1 Composite Import всегда recursive и всегда импортирует доступные FBX;
checkboxes `Recursive`/`With FBX` отсутствуют. Resolver обходит волну
`composite_ref → composite payload → nodes`, затем material UID из
`static_mesh.material_slots`. Shared resources дедуплицируются по UID.

Точное Blender host-mapping: definition Collection хранит `mh_uid=ResourceUID`
и `mh_kind=static_mesh|composite`; node Empty хранит `mh_uid=NodeUID` и
`mh_kind=group|mesh|composite_ref`. У `group` отсутствуют `mh_resource_uid` и
`instance_collection`. У `mesh`/`composite_ref` required
`mh_resource_uid=ResourceUID`, а target Collection имеет соответственно
`mh_kind=static_mesh`/`mh_kind=composite`.

Ненайденная ссылка при Blender import не уничтожает структуру: placement Empty
хранит `mh_uid=NodeUID`, `mh_resource_uid=ResourceUID`, `mh_kind` и
`mh_unresolved=true`; target definition/placeholder Collection хранит
`mh_uid=ResourceUID`, ожидаемый `mh_kind` и `mh_unresolved=true`. Лог получает
`MH_W_UNRESOLVED_RESOURCE`. `Resolve Missing` повторяет cascade, наполняет те же
Empty/Collection, выставляет `mh_unresolved=false` и не пересобирает composite
или identity.

При composite cycle Blender import обрывает только back-edge, создаёт
placeholder, выдаёт `MH_W_COMPOSITE_CYCLE` и продолжает остальной граф. Blender
export и UE import блокируют соответствующую операцию кодом
`MH_E_COMPOSITE_CYCLE`. Node-parent cycle внутри одного файла всегда
`MH_E_PARENT_CYCLE`.

Missing material при Blender import даёт `MH_W_MATERIAL_NOT_FOUND`, mesh
geometry продолжает импортироваться. Missing material при FBX Export с
`Export Materials=OFF` либо после неуспешной material-операции даёт warning со
списком UID и быстрым действием `Export materials…`, но не блокирует geometry
payload. При ON новая material resource создаётся по D38; существующая
обновляется in place. Composite Export material payload не размещает. Missing
static-mesh/composite dependency при export — `MH_E_UNRESOLVED_EXTERNAL`.
При UE import unresolved material блокирует применение только затронутого
material resource с `MH_E_UNRESOLVED_EXTERNAL`; остальные независимые resource
operations batch продолжаются. Тем самым `MH_E_*` по-прежнему блокирует свою
операцию, но не обязан отменять весь batch.

Зависимости для export validation и Analyzer каждый раз вычисляются из payload
и manifest rows; отдельного persisted dependency list нет.

## 7. Диагностики v1

Нормативный инвариант: `MH_E_*` блокирует ту операцию, к которой относится;
`MH_W_*` сообщает проблему, но её не блокирует. Коды называют обнаруженный
факт, а реакцию (`placeholder`, fallback, skip) описывает message/report.

| Код | Контекст | Условие |
|---|---|---|
| `MH_E_AMBIGUOUS_RESOURCE_OWNER` | resolve/import/export | UID объявлен двумя owning manifests под root. |
| `MH_E_UNRESOLVED_EXTERNAL` | Blender export; UE import | Export: required mesh/composite не резолвится; UE: affected resource dependency, включая material, не применяется. |
| `MH_E_COMPOSITE_CYCLE` | Blender export; UE import | Composite dependency cycle. |
| `MH_E_UNSUPPORTED_NODE_KIND` | Blender/UE import/export | Reserved node kind ещё не имеет runtime contract. |
| `MH_W_COMPOSITE_CYCLE` | Blender import | Back-edge заменён placeholder; остальной граф импортируется. |
| `MH_W_UNRESOLVED_RESOURCE` | Blender import | UID не найден; identity сохранена placeholder. |
| `MH_W_REGISTRY_STALE` | resolve | Registry hint не подтверждён scan; используется scan result. |
| `MH_W_REGISTRY_INVALID` | resolve/export | Registry не читается/не соответствует schema; используется scan. |
| `MH_W_MATERIAL_NOT_FOUND` | Blender import/export | Material UID пока не резолвится; geometry не блокируется. |
| `MH_W_MATERIAL_PAYLOAD_FALLBACK` | Blender import | Dagormat RNA не представляет payload lossless; JSON Custom Property остаётся authority. |
| `MH_W_TEXTURE_OUTSIDE_ROOT` | transitional export/import | Texture path абсолютный, то есть external. |
| `MH_E_TEXTURE_OUTSIDE_ROOT` | strict export/import | Texture path абсолютный, то есть external. |
| `MH_E_INVALID_MATERIAL_VALUE` | material export/import | Payload/path нельзя представить по v1. |
| `MH_E_UNKNOWN_SCHEMA_VERSION` | reader | Schema version не поддерживается. |
| `MH_E_INVALID_LOD_HIERARCHY` | Blender export | Dagor authoring container/child shape не соответствует `<base>.lods` → direct `<base>.lodNN`. |
| `MH_E_LOD_LEVELS_SPARSE` | Blender export; UE import | Фактические LOD levels не образуют плотный диапазон от 0. |
| `MH_E_LOD_SLOT_NOT_IN_BASE` | Blender export; UE import | Material slot уровня 1+ отсутствует в LOD0. |
| `MH_E_DEPRECATED_LOD_ROWS` | manifest reader/writer | Обнаружено отменённое per-file поле `lods`; требуется миграция/re-export. |
| `MH_E_LOD_PASSPORT_MISMATCH` | UE FBX import | Passport `lod_levels` не совпадает с фактическими `mh_lod_level` mesh nodes. |
| `MH_W_LOD_AUX_NODE_IGNORED` | Blender export; UE import | `UCX_*`/`SOCKET_*` обнаружен на уровне 1+ и игнорируется. |

Коды целостности UUID, resource names, node table, transforms, material slots и
source paths из frozen composite/validation contract сохраняются. Для нового
source workflow malformed/escaping `source` даёт
`MH_E_INVALID_RESOURCE_SOURCE`; pending, malformed или изменившийся manifest
snapshot — `MH_E_INVALID_EXPORT_MANIFEST`. Расширение машинного реестра после
freeze требует versioned migration note.

## 8. Канонизация и имена

JSON payloads пишутся UTF-8 без BOM, LF, завершающий LF, indent 2. Known keys
имеют порядок примеров; `nodes` и `resources` сортируются по UID.

Hash canonical form: compact JSON без whitespace; object keys сортируются по
UTF-8 bytes после Unicode NFC; строки — NFC UTF-8; `null` и bool сохраняются;
массивы сохраняют порядок, кроме явно UID-сортируемых таблиц. NFC-collision
object keys — ошибка.

Continuous numbers квантуются round-half-even до on-disk записи:

| Поля | p |
|---|---:|
| `translation_cm` | 3 |
| `rotation_quat`, `scale` | 6 |
| variant `weight` | 4 |
| `properties`, material `params` | 6 |

В canonical form decimal `q / 10^p` заменяется integer `q`.
`schema_version` и `seed_salt` остаются точными integers. NaN/Inf запрещены.
XXH3-64 записывается как `xxh3:` и 16 lowercase hex.

Каждый текущий resource/composite/material `name` при каждом export/import
непуст и соответствует `[A-Za-z0-9_ -]`; нарушение даёт
`MH_E_NON_ASCII_RESOURCE_NAME`. Node `display_name` и JSON properties могут быть
Unicode NFC. Sanitized filename вычисляется только при первом создании:
lowercase, всё вне `[a-z0-9_]` → `_`, runs underscore схлопываются, Windows
reserved base name получает prefix `_`. Filename не участвует в identity и не
обязан меняться вслед за последующим валидным rename.

## 9. Транзакции и стабильность

### 9.1 Standalone export

Единица export-транзакции — один FBX mesh-ресурса (включая все его LOD),
composite или material и upsert одной resource row в его owning manifest.

1. До первой записи вычислить/валидировать payload, canonical hash, row,
   computed dependencies и полный новый manifest.
2. Создать атомарно `export_manifest.json.tmp` с полным prepared manifest.
   Пока marker существует, stable manifest каталога не читается.
3. Изменённый payload записать рядом как `.tmp`, затем atomic replace. При
   неизменённой semantic и on-disk metadata payload не трогать.
4. Только после успеха payload atomic replace marker в
   `export_manifest.json`. Это commit event.

Upsert сохраняет unrelated rows и пользовательские файлы. Standalone export
не удаляет orphan rows/payloads автоматически. Failure оставляет marker
fail-closed. Обычный resolver/import и writers других UID не читают его как
owner и завершаются `MH_E_INVALID_EXPORT_MANIFEST`.

Единственное исключение — явный recovery тем же standalone writer для UID из
marker. До записи он обязан:

1. прочитать stable manifest, если он есть, и marker;
2. доказать, что кроме допустимого обновления `exporter_version` marker
   отличается от stable ровно одним upsert этого UID, сохраняя все unrelated
   rows в точной канонической форме; при отсутствии stable marker может
   содержать только этот первый UID;
3. подтвердить тот же kind, нормализованный source/owner и отсутствие второго
   owning manifest полным scan: собственный marker проверяется этим recovery,
   но не считается owner; любой другой pending marker под root блокирует scan;
   для существующего stable row source менять нельзя;
4. заново вычислить payload и row из текущего Blender source, построить новый
   prepared manifest поверх **stable** unrelated rows и atomic-replace marker;
5. безусловно заново atomic-replace payload (старое/new crash-состояние не
   считается hash-skip proof), затем commit'ить новый marker в stable manifest.

Если любое доказательство не проходит, marker сохраняется, операция блокируется
`MH_E_INVALID_EXPORT_MANIFEST`; автоматического выбора или удаления нет.
Повторный crash оставляет тот же fail-closed recovery protocol.

UI FBX Export с `Export Materials=ON` собирает уникальные MaterialUID выбранной
Collection hierarchy и запускает для каждого тот же standalone material writer:
unique owner обновляется in place, UID без owner использует каталог выбранного
FBX output. В обычной операции FBX/mesh-row commit'ится первым, после чего идут
material upserts: material failure становится warning и не откатывает geometry.
Если до запуска уже существует доказанный pending material marker выбранной
Collection, сначала выполняется обязательный recovery этого MaterialUID, потому
что §9.1 запрещает любой другой write под root до снятия marker.
Это последовательность независимых resource-транзакций, а не новый bundle или
cross-manifest atomic commit; успешно завершённые upserts не откатываются из-за
последующего независимого upsert.

При `Export Materials=OFF` FBX writer не вызывает material writer и не меняет
material payload/rows. Отдельный Material Export принимает Material + Directory;
если UID уже имеет единственного owner, выбранный каталог не перемещает его:
update идёт in place. Composite Export не создаёт `.material`. Во всех режимах
texture files не входят в export-транзакции и не копируются.

### 9.2 Resolver/import snapshot

Операция получает стабильный snapshot полного manifest set под `source_root`:
фиксируются перечень paths и bytes каждого manifest. Наличие pending marker,
изменение/появление/исчезновение manifest либо изменение registry/payload до
apply делает snapshot нестабильным и отменяет операцию.

После чтения dependency payloads и непосредственно перед commit/apply перечень
и bytes проверяются повторно. Blender mutation выполняется транзакционно:
preflight предшествует изменению сцены; при runtime failure или нестабильном
snapshot внесённые этой операцией изменения откатываются. Source files import
не изменяет.

## 10. Legacy, migration и golden gates

Production reader для pre-freeze `materials[]`, `mh.bundle_manifest` или
других промежуточных форм **не создаётся**. Эти формы не являются v1 legacy
API: внешних consumers до freeze не было. Golden artifacts регенерируются.
Локальные тестовые выгрузки при необходимости преобразуются одноразовым
внешним migration script, который не входит в runtime codec.

Обязательные final-v1 golden gates:

- **M8 external_resource:** `libB/export_manifest.json` владеет composite
  `lamp_set` и static mesh `lamp_mesh`; `lamp_set.composite` ссылается на
  `lamp_mesh`. Root composite из `libA` ссылается на UID `lamp_set`. При полном
  root resolver возвращает owner/payload из `libB`, recursive import создаёт по
  одной definition Collection и импортирует FBX. В negative-варианте без
  `libB` Blender import оставляет composite UID-placeholder с
  `MH_W_UNRESOLVED_RESOURCE`, а export validation даёт
  `MH_E_UNRESOLVED_EXTERNAL`.
- **M9 shared_material:** `common/export_manifest.json` владеет одним
  `.material`; два static meshes из разных manifests ссылаются на его UID.
  Resolve/import материализует материал один раз. Изменение только `.material`
  даёт `UPDATE_PROPERTIES` одного material resource и ноль mesh operations.
  FBX Export/ON обновляет unique owner in place, новый material создаёт рядом с
  FBX, а OFF оставляет все material bytes/rows неизменными.

В этом schema-doc commit фиксируются формы и ожидаемые исходы M8/M9. Golden
файлы, canonical bytes и expected hashes регенерируются в G1/G4 и должны быть
воспроизводимы независимыми Python и UE readers до приёмки соответствующих
implementation gates; production legacy-reader ради старых fixtures не
добавляется.
