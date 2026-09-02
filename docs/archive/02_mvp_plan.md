> Status: HISTORY · Do not use for implementation · Superseded by docs/16_recipe_model.md

# План разработки: Blender → UE5 Composite Pipeline (MVP)

> **SUPERSEDED BY SOURCE PROTOCOL V4.** Этот план больше не задаёт порядок или
> acceptance-критерии работ. Действующий норматив —
> [`08_source_protocol_v4_plan.md`](08_source_protocol_v4_plan.md), действующие
> срезы S0–S6 — [`09_v4_agent_slices.md`](09_v4_agent_slices.md). Body сохранён
> как история прежнего плана.

> **HISTORICAL EVIDENCE ONLY / SUPERSEDED BY CLEAN SOURCES v2.** Весь план ниже,
> включая MVP criteria, B/C sequencing, manifest watcher, uid8 filenames и
> hash-skip, не является implementation input. Активные документы:
> `05_source_schema_v1.md` (v2 contract; filename historical),
> `04_source_workflows.md`, `06_final_v1_plan.md`,
> `07_ue_import_contract_r3.md`. Blender Export v2 всегда пишет requested
> payload и не строит diff/index; UE startup/watcher сравнивает payloads с
> Ledger. Старый body сохранён только для истории оценок.

Актуальная Blender-модель совпадает с dag4blend по разделению definition и
placement. В сцене **GEOMETRY** definitions представлены sibling Collections:
mesh collection содержит geometry, composite collection — Empty collection
instances. Empty + `instance_collection` = placement/reference; фиксированная
сцена COMPOSITS больше не является условием экспорта.

Значит, авторинг-модель не проектируем — она есть. Проект = замена формата экспорта (.dag/.blk → source files) и целевого движка (daEditor → UE5).

---

## 0. Определение MVP (критерий "готово")

Один вертикальный сценарий, доказуемый на golden-сцене:

1. Художник отдельными кнопками экспортирует выбранные mesh/composite
   Collections в один source-каталог → появляются `*.mesh.fbx`,
   `*.composite` и инкрементальный `export_manifest.json`.
2. В UE watcher (или ручной Import) подхватывает → появляются `SM_*`, `CA_*` uassets.
3. `CA_Building_A` перетаскивается в уровень → `AMHCompositeActor` разворачивается в компоненты StaticMesh с правильными трансформами (включая вложенный композит).
4. В Blender: rename объекта, сдвиг placement, удаление одного узла, правка
   геометрии одного меша → повторный standalone Export соответствующей
   definition → в UE Reimport → **обновляются только затронутые ассеты**, все
   размещённые в уровне актёры пересобираются, ничего лишнего не пересоздано
   (проверка по diff-логу).

Всё, что не нужно для этого сценария — за пределами MVP.

**В MVP не входит** (но схема данных их резервирует):
VariantSet / weights / keyed random (Kind и seed-поля в схеме есть, компилятор их не трогает); seed_policy; Actor/BP-узлы; Break / Build New Composite; Level Instance / PLA / ISM-таргеты (только StaticMeshComponent); orphan review UI (просто список в логе); skeletal (D29). Blender-импортер `.composite` — В MVP (блок 6 этапа B, D21: исходники = экспортированные файлы, .blend вне VCS); материалы — полноценные ресурсы уже в MVP (D22), ownership — через VCS (D21).

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
- Атомарность: вся семантика и manifest вычисляются до первой записи;
  prepared `export_manifest.json.tmp` служит fail-closed marker, изменённые
  payload-файлы пишутся через adjacent `.tmp` + replace, marker продвигается
  в stable manifest только после их успеха (§1.1 схемы).

### B3. Валидация
Циклы composite-ссылок (DFS, показ цепочки), дубликаты UID, битые instance_collection, ресурс без UID, пустые коллекции. Отчёт — в текстовый лог (паттерн dag4blend `log()`).

### B4. Материалы (D22/D23, расширение B2)
- Извлечение из `Material.dagormat`: `shader_class`, `optional`, `sides`, `textures`;
  UID в `Material['mh_uid']`. Без dagormat — заглушка `rendinst_simple`.
- Нормализация путей текстур под `texture_root` (forward slashes), валидация `MH_E_TEXTURE_OUTSIDE_ROOT`, утилита однократного remap старого корня (D27).
- `material_slots` у mesh-ресурсов; секция `materials` и material `content_hash`
  в manifest v2 (§2.1 схемы); отдельного `materials.json` нет.
- Чтение registry.json (D28), если есть: warning на неизвестный shader_class.
- FBX-экспорт — через канон-настройки и cm-контекст-менеджер из reference-скрипта (`axis_forward='X'`, `axis_up='Z'`; mesh-hash считается ДО cm-конверсии — §9).

### Чекпойнт «owner hands-on» (после B11, до B13)

По готовности B10–B11 аддон передаётся владельцу для ручного мини-сценария:
«коллекция из куба + композит из трёх ссылок на неё → Export → правка трансформа →
Export → `diff_bundles` показывает один UPDATE_TRANSFORM, FBX не переэкспортирован».
Цель — проверка UX одной кнопки, не корректности. Замечания по панели/логу —
отдельными issue, B13 не блокируют. PR этапа B открывается draft'ом заранее —
ревью схемных правок ведётся в нём.

### Блок 6 этапа B — Blender-импортер `.composite` (D21; отдельный PR, не блокирует старт C)
- **B14.** `core/import_composite.py` + оператор Import Composite: референс `cmp_import.py`, но dependency-обход вместо модуль-глобалов, резолв ссылок по UID.
- **B15.** Round-trip приёмка: export golden → import в чистый .blend → re-export → дифф пуст. Допустимое исключение: ложный UPDATE_GEOMETRY multi-object ресурсов, если внутренние uid не переживут FBX — проверить `use_custom_props` в обе стороны, зафиксировать примечанием в §9.2.

Приёмка этапа B: экспорт golden-сцены и всех мутаций; JSON-диффы между экспортами мутаций совпадают с ожидаемыми из A2 (это проверяется скриптом, UE ещё не нужен); негативные сцены дают ожидаемые `expected_errors`.

---

## 3. Этап C — UE-плагин, часть 1: ассеты и импорт (≈ 3–4 недели)

Плагин `MimirComposite` (editor-модуль + тонкий runtime-модуль для AMHCompositeActor).

Таргет: **UE 5.7.4**, код совместим с 5.8 (без deprecated-API 5.7; сборочная
проверка на 5.8 — в CI-матрицу, когда появится CI).

### C1. Классы данных
- `UMHCompositeAsset`: CompositeUID, `TArray<FMHCompositeNode>`, AssetImportData, SourceJsonSnapshot.
- `FMHCompositeNode`: NodeUID, ParentUID, DisplayName, Kind, LocalTransform, ResourceUID, `FSoftObjectPath Resource`, PropertyBag(raw). Поля Variants/SeedPolicy — объявлены, не используются.
- `UMHImportLedger` (терминология D26; бывш. UMHSourceBundle): таблица ResourceUID → {SoftObjectPath, applied content_hash, source file}, дата/версия последнего импорта. Applied-хеши — база трёхстороннего сравнения материалов (D25).
- `UAssetDefinition_*` для обоих (цвет, категория, read-only generic editor).

### C2. Импорт
- `UMHCompositeFactory : UFactory, FReimportHandler` для `.composite`.
- `UMHManifestImporter` (терминология D26; бывш. UMHBundleImporter — subsystem, вызывается фабрикой manifest'а или кнопкой): parse manifest → diff с UMHImportLedger (по content_hash) → топосортировка (детект циклов) → **план**: [import mesh FBX ×N, create/update MI, import composites в порядке зависимостей] → выполнение → обновление Ledger'а.
- Целевые пути ассетов — вычисляются по D27 (content_root + относительный путь источника + префикс SM_/MI_/T_/CA_); смена каталога источника при том же UID = MOVE ассета.
- Material Builder: трёхстороннее сравнение base/theirs/ours (D25) → UPDATE / LOCAL_EDIT / CONFLICT (интерактивный keep/overwrite в Preview; headless — политика проекта, default overwrite). Master по `<master_root>/<shader_class>`; registry.json генерируется из master_root (D28, кнопка Refresh + автогенерация при startup-скане); отсутствующий Master — ошибка материала в отчёте, не блокирующая остальной импорт.
- Finalize-реестр — порт правил из `reference/studio_scripts/ue5_postprocess_materials.py`: decal (основной сигнал `mh_p_role="decal"`; legacy-суффикс `_decal/_decals` — fallback с warning'ом «проставьте mh_p_role»), Max Lumen Mesh Cards = 32 (настройка), UCX-чистка, texture suffix постобработка (§12 схемы).
- Geometry backend: `IGeometryImportBackend` с одной реализацией `FLegacyFbxBackend` — legacy static mesh import "один FBX → один SM" (UFbxFactory / AssetTools), + перенос ваших текущих post-import Python-правил как C++/Python шаг Finalize (Nanite, collision, decal shadows и т.д. — прямой порт существующих скриптов).
- Резолв `resource_uid → SoftObjectPath` при импорте композита через UMHSourceBundle; неразрешённый UID → warning-узел, импорт не падает.
- Preview в MVP = **текстовый diff-отчёт** в Message Log/Output (CREATE/UPDATE/RENAME/REMOVE/UNCHANGED по каждому ресурсу и узлу). Окно с тремя колонками — после MVP; но отчёт генерится из того же плана, что исполняется (принцип единого FResolvedImportPlan соблюдён с первого дня).

Приёмка: импорт golden-bundle создаёт правильные ассеты; каждая мутация → реимпорт → diff-отчёт совпадает с ожидаемым из A2; повторный импорт без изменений → всё UNCHANGED, ноль пересозданных ассетов.

### C3. Синхронизация (D26): watcher + startup-скан
- Watcher: подписка IDirectoryWatcher на stable manifest rename под source_root,
  debounce; каталог с `export_manifest.json.tmp` считается export-in-progress и
  не импортируется (§1.1 схемы).
- Startup-скан: сначала пропускает каталоги с pending marker, затем
  `OnAssetRegistryFilesLoaded` → манифесты vs Ledger, сравнение ТОЛЬКО по
  content_hash (не mtime); режим prompt/silent — настройка, default prompt.
- CI-commandlet (`-run=MHImportManifests`) — post-MVP.
- Первый коммит этапа C — автоматизация UE-половины axis-теста R1 (по сниппету из B3).

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
7. CI-commandlet `-run=MHImportManifests` (D26).
8. Export Selection — dependency closure композита для передачи третьей стороне, флаг with-textures (D21).
9. `FInterchangeGeometryBackend` — NodeContainer-pipeline как геометрический backend, приёмка по golden-сравнению с legacy (D24).

Примечание: Blender-импортер `.composite` из этого списка ИЗЪЯТ — по D21 он
обязательная часть системы (блок 6 этапа B, второй PR).

Долгосрочные направления с критериями пересмотра — `docs/ROADMAP.md`.
