# 01 — Bundle Schema v1 (SPEC)

Статус: DRAFT → фиксируется в этапе A (см. 02_mvp_plan.md). Изменения схемы после фиксации —
только через повышение `schema_version` и migration-заметку.

Принцип №1 (проверяется каждым решением): **Identity = адрес, а не имя и не порядок.**
Ни rename, ни перестановка узлов в файле, ни добавление параметра, ни апгрейд движка
не меняют ничего, кроме того, что изменил человек.

## 1. Структура bundle-каталога

```
Building_A.bundle/
├─ export_manifest.json          # СЛУЖЕБНЫЙ, скрытый от пользователя, пишется ПОСЛЕДНИМ
├─ building_a.composite          # 1 файл = 1 composite definition (JSON внутри)
├─ window_set_a.composite
└─ meshes/
   ├─ wall_a__2db5574c.mesh.fbx  # 1 файл = 1 mesh resource; суффикс = первые 8 hex UID
   └─ window_a__5839a2e1.mesh.fbx
```

Атомарность экспорта: экспорт во временный каталог → хеши → манифест последним →
атомарная замена published-каталога. Отсутствие валидного манифеста = UE не импортирует.
Неизменённые mesh.fbx (по content_hash) не переэкспортируются и не перезаписываются.

## 2. export_manifest.json

```json
{
  "schema": "mh.bundle_manifest",
  "schema_version": 1,
  "exporter_version": "0.1.0",
  "bundle_uid": "11db2600-1a7d-4808-bfa7-0d7b5c71a78c",
  "bundle_name": "Building_A",
  "source": { "blend_file": "Building_A.blend" },
  "resources": [
    {
      "uid": "2db5574c-3aca-43cc-9ab5-8242403e18cd",
      "kind": "static_mesh",
      "name": "wall_a",
      "source": "meshes/wall_a__2db5574c.mesh.fbx",
      "content_hash": "xxh3:9f2c..."
    },
    {
      "uid": "f53d93af-94c3-472f-98d0-ff36eb93c417",
      "kind": "composite",
      "name": "window_set_a",
      "source": "window_set_a.composite",
      "content_hash": "xxh3:aa01..."
    }
  ],
  "external_dependencies": [
    { "uid": "e3ba6783-...", "kind": "composite", "name": "lamp_a" }
  ]
}
```

Правила:
- `content_hash` для mesh — по детерминированной сериализации evaluated mesh
  (verts, loops, UV, атрибуты, material slot names в стабильном порядке), НЕ по байтам FBX.
- `content_hash` для composite — по канонизированному JSON (сортировка узлов по node_uid,
  сортировка ключей, без whitespace).
- `external_dependencies` — UID'ы, на которые ссылаются composites, но которых нет в этом bundle.

## 3. Файл `*.composite`

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
      "resource_uid": "5839a2e1-....",
      "local_transform": {
        "translation_cm": [0.0, 45.0, 210.0],
        "rotation_quat":  [0.0, 0.0, 0.0, 1.0],
        "scale":          [1.0, 1.0, 1.0]
      },
      "properties": { }
    },
    {
      "node_uid": "a0ccf18c-....",
      "parent_uid": null,
      "kind": "composite_ref",
      "display_name": "lamp",
      "resource_uid": "e3ba6783-....",
      "local_transform": { "...": "..." }
    }
  ]
}
```

Правила:
- **Flat node table**, иерархия через `parent_uid` (null = корень). Порядок узлов в массиве
  семантики НЕ несёт; экспортер сортирует по node_uid для стабильных диффов.
- `kind` в v1: `group | mesh | composite_ref`. Зарезервированы (валидны в схеме, MVP-компилятор
  выдаёт warning-заглушку): `variant_set | variant | actor`.
- Трансформы — **уже в UE-конвенции**: сантиметры, Z-up, left-handed, кватернион (x,y,z,w).
  Конвертацию выполняет Blender-аддон (Decision D12).
- `properties` — открытый bag `строка → значение`. Неизвестные ключи сохраняются и
  транспортируются (принцип broken_properties из dag4blend). Known-ключи v1:
  `role: string` (например "decal"), резерв: `render.*`, `collision.*`.

### Зарезервированные поля (v1 схема, post-MVP реализация)

```json
{
  "kind": "variant_set",
  "variants": [
    { "resource_uid": "...", "weight": 1.0 },
    { "resource_uid": "...", "weight": 1.0 }
  ],
  "seed_policy": "inherit",
  "seed_salt": 0
}
```
```json
{
  "kind": "actor",
  "actor_resource_uid": "c38bf56d-....",
  "cached_soft_class_path": "/Game/Gameplay/BP_PhysicsDoor"
}
```

## 4. UID-правила (Blender-аддон)

| Событие в Blender | Поведение UID |
|---|---|
| Переименование объекта/меша/коллекции | все UID сохраняются |
| Новый placement того же Mesh datablock | новый node_uid, старый resource_uid |
| Linked duplicate (Alt+D) | общий resource_uid, новый node_uid |
| Make Single User | НОВЫЙ resource_uid |
| Ctrl+D (копия объекта) | Blender копирует props → дубликат node_uid → **экспорт падает**, Fix-кнопка переназначает |
| Collection Instance на composite | новый node_uid, тот же composite uid |
| Перенос узла к другому родителю | тот же node_uid, меняется parent_uid |

Хранение: `obj['mh_uid']` (node), `mesh['mh_uid']` (resource), `collection['mh_uid']`
(resource/composite). Назначение — lazy при Validate/Export. UUID4, lowercase, дефисы.

## 5. Random (спецификация зафиксирована, реализация post-MVP)

```
NodePathUID  = Hash(node_uid_0, node_uid_1, ..., node_uid_N)   # путь от корня definition
OccurrenceUID = Hash(PlacementUID, NodePathUID)
RandomValue   = Hash(RngSchemaVersion, PlacementUID, OccurrenceUID, RandomChannelID, UserSeed)
```
- Hash: xxHash64 (или xxh3) по байтовой конкатенации; НЕ HashCombine UE (нестабилен между версиями).
- `RandomChannelID` — закрытый реестр: `variant_selection, offset_x, offset_y, offset_z,
  rotation_yaw, rotation_pitch, rotation_roll, scale_uniform, scale_y`. Расширение реестра —
  через schema_version заметку. Свободные строки запрещены.
- Инварианты (тестируются): добавление нового канала не меняет значения других каналов;
  перестановка узлов в JSON не меняет ничего; rename не меняет ничего; reroll меняет только
  UserSeed; `seed_policy=independent` отвязывает поддерево от PlacementUID (использует seed_salt).
- Выбор варианта по весам: накопительные диапазоны в порядке сортировки variants по
  resource_uid (стабильный порядок); добавление варианта добавляет диапазон в конец.

## 6. Валидация (Blender Export / UE Import)

Blender (блокирует экспорт): дубликаты node_uid / resource_uid; циклы composite-ссылок
(DFS, вывод цепочки A→B→A); Empty с instance_collection без mh_uid на коллекции;
пустая коллекция-ресурс; composite-коллекция с под-коллекциями (плоскость как в dag4blend).

UE (Analyzer, блокирует импорт затронутых композитов, не всего bundle): цикл в объединённом
графе (импортируемые + существующие ассеты) через топосортировку; неизвестный schema_version;
дубликат UID с другим bundle-владельцем.

UE (Compiler, никогда не крэшит): стек CompositeUID → error-заглушка + Message Log;
лимит глубины 64; неразрешённый resource_uid → warning-заглушка.

## 7. Диффы (контракт reimport)

Операции, которые UE-Analyzer обязан различать и печатать в отчёте:
`CREATE / UPDATE_GEOMETRY / UPDATE_TRANSFORM / UPDATE_PROPERTIES / RENAME / REPARENT /
REMOVE / UNCHANGED / EXTERNAL_UNRESOLVED`.
Повторный импорт неизменённого bundle → 100% UNCHANGED, ноль пересозданных ассетов —
это приёмочный тест всего пайплайна.
