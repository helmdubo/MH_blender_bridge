# План разработки: Blender → UE5 Composite Pipeline (MVP)

Исходное состояние авторинга уже совпадает с dag4blend-моделью, и это главный актив:
- **Сцена GEOMETRY**: каждая коллекция = один mesh-ресурс (будущий FBX → UStaticMesh).
- **Сцена COMPOSITS**: Empty + `instance_collection` = placement, коллекция = composite definition.

Значит, авторинг-модель не проектируем — она есть. Проект = замена формата экспорта (.dag/.blk → bundle) и целевого движка (daEditor → UE5).

---

## 0. Определение MVP (критерий "готово")

Один вертикальный сценарий, доказуемый на golden-сцене:

1. Художник в Blender жмёт **Export Bundle** → на диске появляется bundle-каталог.
2. В UE watcher (или ручной Import) подхватывает → появляются `SM_*`, `CA_*` uassets.
3. `CA_Building_A` перетаскивается в уровень → `AMHCompositeActor` разворачивается в компоненты StaticMesh с правильными трансформами (включая вложенный композит).
4. В Blender: rename объекта, сдвиг placement, удаление одного узла, правка геометрии одного меша → **Export** → в UE Reimport → **обновляются только затронутые ассеты**, все размещённые в уровне актёры пересобираются, ничего лишнего не пересоздано (проверка по diff-логу).

Всё, что не нужно для этого сценария — за пределами MVP.

**В MVP не входит** (но схема данных их резервирует):
VariantSet / weights / keyed random (Kind и seed-поля в схеме есть, компилятор их не трогает); seed_policy; Actor/BP-узлы; Break / Build New Composite; Level Instance / PLA / ISM-таргеты (только StaticMeshComponent); материалы beyond "перенос текущих post-import скриптов"; orphan review UI (просто список в логе); Blender-импортер `.composite`; ownership/fork между .blend-файлами (один bundle = один .blend).

---

## 1. Этап A — Bundle Schema v1 + Golden Scene (≈ 3–5 дней, без кода UE)

Самый дешёвый и самый важный этап. Результат — два артефакта:

### A1. Документ Bundle Schema v1
- Принцип в шапке: **Identity = адрес, а не имя и не порядок.** UID'ы: BundleUID, ResourceUID, CompositeDefinitionUID, NodeUID (Placement/Occurrence — резерв, в MVP не используются, но описаны).
- `*.composite` (JSON внутри): schema_version, uid, name, flat node table (`node_uid`, `parent_uid`, `kind ∈ {group, mesh, composite_ref}`, `resource_uid`, `local_transform` **в UE-конвенции**, `properties{}` как raw bag).
- `export_manifest.json` (скрытый commit-marker): bundle uid, exporter version, resources[] {uid, kind, name, source file, content_hash}, external_dependencies[].
- Правила: transform в см, Z-up, кватернион; порядок нод в файле не несёт смысла; unknown keys в properties сохраняются (принцип `broken_properties` из dag4blend); резервные поля variants/seed_policy/actor_resource_uid описаны, но optional.

### A2. Golden Scene (.blend)
Минимальный набор, покрывающий все рёбра:
```
GEOMETRY:  wall_a, wall_b, window_a (ассиметричный меш!), decal_leak
COMPOSITS: CA_WindowSet  (3 × window_a placements, один со scale+rotation)
           CA_Building   (wall_a, wall_b, decal_leak, 2 × ref CA_WindowSet,
                          Empty-group с детьми, вложенность 2 уровня)
```
+ сценарии-мутации (сохранённые версии сцены): rename объекта; rename коллекции; linked duplicate; Make Single User; удаление узла; правка геометрии wall_a; перенос узла к другому родителю.

Приёмка этапа: schema-документ ревьюится, golden-сцена собрана, для каждой мутации на бумаге записан ожидаемый diff (CREATE/UPDATE/RENAME/REMOVE/UNCHANGED). Это спецификация тестов этапов B и C.

---

## 2. Этап B — Blender-аддон "mh4blend" (≈ 2–3 недели)

Форкается скелет dag4blend — там уже решены скучные вещи. Конкретно переиспользуем:

| Из dag4blend | Как используем |
|---|---|
| `settings.py` (prefs, projects, PropertyGroup-структура) | скелет настроек: путь bundle-выхода, путь registry.json |
| `helpers/getters.py, texts.py (log), popup/` | как есть |
| `helpers/props.py` (`fix_type`, типизированные props) | база для чтения/валидации metadata `mh_*` |
| `colprops` (тип/имя на коллекции) | паттерн UI для назначения kind ресурсу; вместо `col['name']`-override — UID |
| `cmp_export.py::write_node` (обход Empty-графа) | тот же обход, но сериализация в node table JSON вместо blk-текста |
| `cmp_panels` (N-панель структура) | каркас панели Export/Validate |
| сцены GEOMETRY/COMPOSITS/`upd_scenes` | уже совпадает с вашей структурой — оставить |

**Не берём**: name-based identity, `cache_rw` (заменяет registry.json из UE — в MVP файл может быть пустым/опциональным), глобалы импорта, axis-математику (`apply_matrix`/`get_matrix` — пишем свою: Blender→UE конвертация одна и тестируется отдельно).

### B1. UID-подсистема (самое тонкое место этапа)
- `mh_uid` на: Object (NodeUID), Mesh datablock (ResourceUID), Collection (ResourceUID/CompositeUID).
- Назначение lazy: при экспорте/валидации всем без UID выдаётся новый.
- **Дубликаты NodeUID** (Ctrl+D копирует custom props): детект перед экспортом — все коллизии object-UID → новый UID тому, у кого позже creation-time... нельзя определить → правило: экспорт падает с ошибкой и кнопкой "Fix: reassign duplicates" (переназначить у всех, кроме одного). Для Mesh datablock дубликат UID при Make Single User → тот же механизм.
- Тест: все мутации golden-сцены дают ожидаемое поведение UID.

### B2. Export Compiler
- Обход COMPOSITS: Empty-граф → flat node table; `instance_collection` → resource_uid; вложенный composite-collection → `composite_ref`.
- Обход GEOMETRY: каждая коллекция → один FBX (`name__uid8.mesh.fbx`): выделение объектов коллекции, `bpy.ops.export_scene.fbx(use_selection=True, ...)` с зафиксированными параметрами (units, axes, smoothing, tangents — те же, что вы используете сейчас в ручном экспорте).
- Инкрементальность с первого дня: content_hash по evaluated mesh (детерминированная сериализация verts/loops/UV/attrs + material slot names) → неизменённые FBX не переэкспортируются.
- Атомарность: экспорт во временный каталог → hashes → manifest пишется последним → атомарная замена.

### B3. Валидация
Циклы composite-ссылок (DFS, показ цепочки), дубликаты UID, битые instance_collection, ресурс без UID, пустые коллекции. Отчёт — в текстовый лог (паттерн dag4blend `log()`).

Приёмка этапа B: экспорт golden-сцены и всех мутаций; JSON-диффы между экспортами мутаций совпадают с ожидаемыми из A2 (это проверяется скриптом, UE ещё не нужен).

---

## 3. Этап C — UE-плагин, часть 1: ассеты и импорт (≈ 3–4 недели)

Плагин `MimirComposite` (editor-модуль + тонкий runtime-модуль для AMHCompositeActor).

### C1. Классы данных
- `UMHCompositeAsset`: CompositeUID, `TArray<FMHCompositeNode>`, AssetImportData, SourceJsonSnapshot.
- `FMHCompositeNode`: NodeUID, ParentUID, DisplayName, Kind, LocalTransform, ResourceUID, `FSoftObjectPath Resource`, PropertyBag(raw). Поля Variants/SeedPolicy — объявлены, не используются.
- `UMHSourceBundle`: BundleUID, таблица ResourceUID → {SoftObjectPath, content_hash, source file}, дата/версия последнего импорта.
- `UAssetDefinition_*` для обоих (цвет, категория, read-only generic editor).

### C2. Импорт
- `UMHCompositeFactory : UFactory, FReimportHandler` для `.composite`.
- `UMHBundleImporter` (subsystem, вызывается фабрикой manifest'а или кнопкой): parse manifest → diff с UMHSourceBundle (по content_hash) → топосортировка (детект циклов) → **план**: [import mesh FBX ×N, create/update MI, import composites в порядке зависимостей] → выполнение → обновление UMHSourceBundle.
- Geometry backend: `IGeometryImportBackend` с одной реализацией `FLegacyFbxBackend` — legacy static mesh import "один FBX → один SM" (UFbxFactory / AssetTools), + перенос ваших текущих post-import Python-правил как C++/Python шаг Finalize (Nanite, collision, decal shadows и т.д. — прямой порт существующих скриптов).
- Резолв `resource_uid → SoftObjectPath` при импорте композита через UMHSourceBundle; неразрешённый UID → warning-узел, импорт не падает.
- Preview в MVP = **текстовый diff-отчёт** в Message Log/Output (CREATE/UPDATE/RENAME/REMOVE/UNCHANGED по каждому ресурсу и узлу). Окно с тремя колонками — после MVP; но отчёт генерится из того же плана, что исполняется (принцип единого FResolvedImportPlan соблюдён с первого дня).

Приёмка: импорт golden-bundle создаёт правильные ассеты; каждая мутация → реимпорт → diff-отчёт совпадает с ожидаемым из A2; повторный импорт без изменений → всё UNCHANGED, ноль пересозданных ассетов.

### C3. Watcher
Auto-reimport директории bundle (Editor Preferences API либо своя подписка на IDirectoryWatcher по manifest-файлу). Полдня работы, огромный UX-выигрыш.

---

## 4. Этап D — UE-плагин, часть 2: актёр и компилятор (≈ 2–3 недели)

- `AMHCompositeActor`: CompositeRef (soft), CompiledFingerprint, (Seed — поле есть, в MVP константа).
- `Compile()`: рекурсивный разворот definition (composite_ref → инлайн-разворот в компоненты; в MVP всё — UStaticMeshComponent под одним актёром, без дочерних актёров/LI). Стек CompositeUID против циклов + error-заглушка. Компоненты: `bSelectable=false`, generated, транзакции только по входам компиляции.
- `UActorFactoryMHComposite`: drag&drop из Content Browser.
- Recompile-цепочка: делегат OnReimported на UMHCompositeAsset → editor subsystem → все загруженные AMHCompositeActor с этим CompositeRef (и с composite-ссылками на него по графу зависимостей вглубь) → Recompile; `PostLoad`-проверка fingerprint для незагруженных ячеек.
- Read-only поведение: выделение проваливается на актёра; `PostEditComponentMove` → откат + throttled-notification (без Override — по решению).

Приёмка (финальная приёмка MVP): сценарий из §0 целиком, на golden-сцене, воспроизводимо.

---

## 5. Порядок и зависимости

```
A (schema+scene) ──► B (аддон) ──► C (импорт) ──► D (актёр+компилятор)
      3–5 дн            2–3 нед        3–4 нед          2–3 нед
```
Итого ~2–2.5 месяца одним разработчиком, знающим обе стороны. B и C можно частично параллелить с двумя людьми (контракт = schema из A).

Первая неделя этапа B должна закрыть два оставшихся технических риска:
1. **Axis/handedness тест**: асимметричный меш + несимметричный placement → мировая позиция контрольной вершины в Blender == в UE. Пока не сходится — дальше не идти.
2. **Стоимость экспорта**: время экспорта GEOMETRY-сцены на 50–100 коллекций с per-collection FBX. Если неприемлемо даже с hash-скипом — рассмотреть один multi-object FBX c пост-сплитом на стороне UE (запасной план, ломает гранулярность, поэтому только по факту замера).

## 6. Что добавляется сразу после MVP (порядок по ценности)
1. Preview-окно (три колонки + Reason на свойство) — план уже есть, нужен только UI.
2. VariantSet + keyed random (schema готова; hash-функция и реестр каналов по фидбэку ревьювера).
3. ISM-таргет для повторяющихся мешей внутри композита.
4. Break / Build New Composite (порты `nodes_split`/`nodes_to_composite`).
5. Actor/BP-узлы + registry placeable-классов.
6. Level Instance таргет для крупных детерминированных поддеревьев.
7. Blender-импортер `.composite` (замыкание круга, порт `cmp_import`).
