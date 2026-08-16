# 01 — Bundle Schema v1 (SPEC)

Статус: DRAFT → фиксируется в этапе A (см. 02_mvp_plan.md). Изменения схемы после фиксации —
только через повышение `schema_version` и migration-заметку.

Принцип №1 (проверяется каждым решением): **Identity = адрес, а не имя и не порядок.**
Ни rename, ни перестановка узлов в файле, ни добавление параметра, ни апгрейд движка
не меняют ничего, кроме того, что изменил человек.

## 1. Структура bundle-каталога

```
Building_A.bundle/
├─ export_manifest.json                    # СЛУЖЕБНЫЙ, скрытый от пользователя, пишется ПОСЛЕДНИМ
├─ building_a__3c1a9b2e.composite          # 1 файл = 1 composite definition (JSON внутри)
├─ window_set_a__f53d93af.composite
└─ meshes/
   ├─ wall_a__2db5574c.mesh.fbx            # 1 файл = 1 mesh resource
   └─ window_a__5839a2e1.mesh.fbx
```

Имя файла ресурса: `<sanitized_name>__<uid8>` + расширение (`.composite` / `.mesh.fbx`),
единообразно для мешей и композитов — правила §10.

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
      "source": "window_set_a__f53d93af.composite",
      "content_hash": "xxh3:aa01..."
    }
  ],
  "external_dependencies": [
    { "uid": "e3ba6783-...", "kind": "composite", "name": "lamp_a" }
  ]
}
```

Правила:
- `content_hash` для mesh — по детерминированной бинарной сериализации evaluated mesh
  (точный формат — §9), НЕ по байтам FBX.
- `content_hash` для composite — по канон-форме JSON (точные правила — §8).
- Формат значения hash — `xxh3:` + 16 hex lowercase (§8.3).
- `external_dependencies` — UID'ы, на которые ссылаются composites, но которых нет в этом
  bundle. Сортировка — по uid. `resources` также сортируются по uid — манифест диффабелен.
- Hash — это fast-path фильтр «смотреть ли внутрь», а не источник diff-операций:
  операции (§7) всегда вычисляются структурным сравнением распарсенных данных.

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

## 8. Канонизация и content_hash

Раздел решает задачу: hash обязаны одинаково вычислять две независимые реализации
(Python-аддон и C++-плагин UE). Строковые представления float непереносимы между языками
и версиями рантаймов, поэтому канон-форма не содержит float вообще.

### 8.1 Два представления, одно значение

| Представление | Назначение | Формат |
|---|---|---|
| On-disk `*.composite` | git-дифф, чтение человеком | pretty-printed JSON: отступ 2 пробела, ключи в фиксированном порядке схемы (8.4), узлы отсортированы по `node_uid`, UTF-8 без BOM, LF, завершающий перевод строки |
| Канон-форма | вход хеш-функции | компактный JSON без whitespace, ключи отсортированы побайтово (UTF-8), все квантуемые числа — масштабированные целые (8.2) |

`content_hash` считается **не по байтам файла**, а по канон-форме, которую каждая сторона
строит из распарсенных данных. Переформатирование файла, порядок узлов в массиве и
prettify на hash не влияют. Hash покрывает весь документ (включая `name`): неравенство
hash означает только «смотреть внутрь», конкретные операции определяет структурный diff (§7).

### 8.2 Квантование чисел

Все continuous-величины квантуются на экспорте, **до записи в файл**:

```
q(value, p) = round_half_even(value * 10^p)   -> целое
```

| Класс поля | p | Точность |
|---|---|---|
| `translation_cm` | 3 | 0.001 см |
| `rotation_quat` | 6 | 1e-6 |
| `scale` | 6 | 1e-6 |
| `weight` (variants) | 4 | 1e-4 |
| числа в `properties` | 6 | 1e-6 |

В on-disk файл попадают уже квантованные значения (десятичная запись `q / 10^p`),
в канон-форму — сами целые `q`. Следствия:

- файл, канон-форма и hash всегда согласованы между собой;
- файл, перечитанный и переэкспортированный без изменений, даёт тот же hash —
  идемпотентность, приёмочный тест канон-библиотеки;
- вопрос «изменился ли transform» — сравнение целых, без эпсилон-логики.

`round_half_even` (banker's rounding) выбран как режим, одинаково определённый в Python
(`round`) и C++ (`std::nearbyint` при округлении к ближайшему чётному IEEE-754).
NaN/Inf в любом квантуемом поле — ошибка валидации экспорта (§6).

### 8.3 Хеш-функция и формат записи

- Алгоритм: XXH3-64 по байтам канон-формы.
- Запись в манифесте: `"xxh3:" + 16 hex lowercase`, пример: `xxh3:9f2c01ab34cd56ef`.
- Для mesh-ресурсов вход хеша — бинарная сериализация §9 (не JSON).
- Кросс-реализационные тест-векторы (вход → байты канон-формы → hash) живут в
  `golden/canonical_vectors.json`; обе реализации обязаны их проходить.

### 8.4 Правила канон-формы JSON

- Ключи объектов — в порядке побайтовой сортировки UTF-8. (Фиксированный порядок ключей
  on-disk формата — человекочитаемое соглашение: `schema, schema_version, uid, name, nodes`;
  в узле: `node_uid, parent_uid, kind, display_name, resource_uid, local_transform,
  properties`, прочие known-поля — за ними, unknown — по алфавиту в конце.)
- Без whitespace: `{"a":1,"b":[2,3]}`.
- Строки — UTF-8; экранируются только обязательные символы: `"` как `\"`, `\` как `\\`,
  управляющие < 0x20 как `\u00xx` (hex lowercase). Не-ASCII символы НЕ экранируются.
- Целые — десятичные, без `+`, без ведущих нулей; `-0` нормализуется в `0`.
- Квантованные числа — целые `q` из 8.2. Других чисел в схеме v1 нет: появление
  неквантованного float в канон-форме — ошибка реализации, а не данных.
- `null` сериализуется как `null`; отсутствующее optional-поле не сериализуется вовсе.
  Экспортер обязан писать все known-поля узла явно (включая `parent_uid: null`),
  поэтому неоднозначность absent-vs-null возможна только у unknown-ключей `properties` —
  там правило: ключ со значением `null` и отсутствующий ключ — разные вещи, оба
  транспортируются как есть.
- Массив `nodes` в канон-форме отсортирован по `node_uid` (побайтовое сравнение строк UID).
- Булевы — `true` / `false`. UID — строка UUID lowercase с дефисами.

## 9. Сериализация mesh для content_hash

Вход — evaluated mesh (после модификаторов) каждого объекта коллекции-ресурса.
Выход — байтовый поток → XXH3-64 → `xxh3:...` в манифесте.

### 9.1 Общие правила кодирования

- little-endian, фиксированные ширины;
- counts и индексы — uint32;
- квантованные значения — int64 (`q` по правилу §8.2, `p` указан per-поле ниже);
- строки — uint32 длина в байтах + байты UTF-8;
- поток начинается с тега версии сериализации — строки `mh.meshser:1`.
  Любое изменение перечня или порядка полей = новый тег; смена тега легально
  меняет все mesh-хеши (полный реэкспорт геометрии, диффы честно скажут UPDATE_GEOMETRY).

### 9.2 Порядок объектов в коллекции-ресурсе

Коллекция-ресурс может содержать несколько объектов. Объекты сериализуются в порядке
побайтовой сортировки их `mh_uid` — единственного ключа, устойчивого к rename.
Известное следствие: переназначение `mh_uid` объекта (например, оператором Fix при
дубликатах) может изменить порядок сериализации и, значит, hash — принимается как
редкий ложноположительный UPDATE_GEOMETRY (см. QUESTIONS).

### 9.3 Поля per-object (строго в этом порядке)

1. Локальный трансформ объекта относительно коллекции: матрица 4×4 row-major,
   16 значений q(p=6). Входит в hash, потому что влияет на итоговую геометрию SM.
2. Vertex positions: count, затем per-vertex x, y, z — q(p=4)
   (локальное пространство объекта, метры Blender, после модификаторов).
3. Polygons: count, затем per-polygon: count вершин + vertex-индексы в порядке обхода.
   Порядок полигонов и вершин — как в mesh datablock (для неизменённых данных стабилен).
4. Custom split normals: uint8 флаг наличия; при наличии — per-loop x, y, z — q(p=4).
5. Шейдинг: per-polygon `use_smooth` (uint8), затем sharp edges — count + пары
   vertex-индексов (меньший индекс первым, пары отсортированы лексикографически).
6. UV-слои: count, слои в порядке побайтовой сортировки имён; на слой — имя,
   затем per-loop u, v — q(p=6). Имя слоя — часть контента: rename слоя легитимно
   меняет hash (UE-материалы адресуют UV-каналы).
7. Color attributes: count, в порядке побайтовой сортировки имён; на атрибут — имя,
   domain (строка: `POINT` / `CORNER`), тип (строка, имя типа Blender),
   затем значения по компонентам — q(p=4).
8. Material slot names: count + имена в порядке слотов (порядок слотов — семантика:
   он определяет material index полигонов).

### 9.4 Анти-требования

- Имя объекта и имя mesh datablock НЕ входят в hash никогда — rename не меняет геометрию.
- Прочие generic-атрибуты, vertex groups, shape keys в v1 НЕ хешируются: их изменение
  не детектится как UPDATE_GEOMETRY (расширение перечня = bump тега `mh.meshser`,
  см. QUESTIONS).

## 10. Имена файлов внутри bundle

Схема имени файла ресурса: `<sanitized_name>__<uid8>` + расширение
(`.mesh.fbx` для мешей, `.composite` для композитов — симметрично, чтобы коллизии
display-имён не влияли на файловую систему).

- `uid8` — первые 8 hex-символов UID (первый блок UUID до дефиса), lowercase.
- Санитизация display-имени:
  1. lowercase;
  2. каждый символ вне `[a-z0-9_]` → `_` (без транслитерации);
  3. последовательности `_` схлопываются в один;
  4. пустой результат → `unnamed`;
  5. зарезервированные имена Windows (`con`, `prn`, `aux`, `nul`,
     `com1`–`com9`, `lpt1`–`lpt9`) → префикс `_`.
- Уникальность имени файла гарантирует `uid8`, а не display-имя.
- Коллизия `uid8` двух разных UID внутри одного bundle (вероятность ~10⁻⁹ на пару):
  экспорт падает с требованием перегенерировать UID одного из ресурсов.
  Схему имени НЕ расширяем: детерминированность имени файла ценнее.
- Rename ресурса меняет имя файла (`source` в манифесте), но не UID: для UE это
  RENAME, не REMOVE+CREATE. Старый файл удаляется по протоколу §1 (после манифеста).
