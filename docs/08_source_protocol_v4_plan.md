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
  raw hash» (точная семантика — §3/OPEN-V4-15), но alias не создаёт.
  Re-bind сироты — обычный импорт в тот же generated path (§3/OPEN-V4-16);
  ЛЮБОЕ отличие raw hash кандидата от receipt-`SourceHash` сироты —
  `MH_W_ORPHAN_REBOUND_CONTENT_DIVERGED` (warning, не блок; равный hash —
  молчаливо).
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
- **Индекс — чистая проекция (решение OPEN-V4-12).** Каждая строка
  выводима заново из (скан source tree + Asset Registry tags); никаких
  tombstones, history и event-логов. Meta-таблица несёт tag
  `mh.project_index:4`; несовпадение tag, коррупция или любая аномалия
  при открытии → файл удаляется и индекс перестраивается полностью; кэш
  никогда не мигрируется и не интерпретируется в подозрительном
  состоянии. `generation` — монотонный int64 одной завершённой
  scan/upsert транзакции; строки несут `last_seen_generation`; full scan
  удаляет строки, не подтверждённые своей generation. Acceptance
  «удаление .sqlite → идентичный индекс» = равенство НОРМАЛИЗОВАННОГО
  логического дампа (упорядоченные typed rows пяти таблиц БЕЗ volatile
  полей: generation, row id, wall-clock) и равенство resolver outcomes
  по каждому key; байтовое равенство .sqlite НЕ требуется; in-memory
  состояние (self-publish tokens) в дамп не входит.
- **Словари состояний (решение OPEN-V4-12).**
  `ResourceCandidates.parse_status ∈ {ok, noncanonical, unreadable,
  invalid_payload}`: `noncanonical` — имя файла нарушает §2 (строка
  ключуется путём, kind/name NULL, в резолв не участвует, видна
  диагностике); `unreadable` — IO-ошибка (без raw_hash);
  `invalid_payload` — байты прочитаны, payload не проходит свою
  грамматику (для static_mesh до S5 парсера нет — читаемый каноничный
  FBX считается `ok`, что зафиксировано как interim). `ResourceKeys`
  существуют для union: {ключи с ≥1 кандидатом с выводимым ключом} ∪
  {dependency targets} ∪ {ключи, заявленные managed-ассетами};
  `resolution_status` — чистая функция: ≥2 кандидатов у ключа →
  `ambiguous` (ВСЕГДА, независимо от parse_status — ambiguous
  побеждает invalid, победитель не выбирается); ровно 1 и не-ok →
  `invalid`; ровно 1 ok → `unique`; 0 кандидатов при живом референсе →
  `missing` (строка живёт ровно пока жив референс); 0 и без референсов
  → строки нет. `GeneratedAssets.status ∈ {applied, stale, orphan, source_blocked,
  invalid_receipt, duplicate_claim}` — derived, функция ТОТАЛЬНА
  (решение OPEN-V4-17), прецеденс: `duplicate_claim` (два UE ассета
  заявляют один ключ — оба; import/plan ключа fail-closed новым
  `MH_E_AMBIGUOUS_GENERATED_ASSET`, регистрируется в S4) →
  `invalid_receipt` (malformed/неполные MH-теги или kind без carrier)
  → source-состояние, взаимоисключающее по ключу: кандидатов нет →
  `orphan`; ключ `ambiguous` ИЛИ `invalid` → `source_blocked` (ассет и
  receipt здоровы, источник нездоров; hash-сравнение НЕ выполняется —
  авторитетного кандидата нет; импорт заблокирован source-диагностикой
  ключа; при выздоровлении источника строка перевычисляется в
  applied/stale обычным образом); ключ `unique` и receipt `SourceHash`
  == candidate raw_hash → `applied`; `unique` и hash отличается →
  `stale`. Плохие managed-строки НИКОГДА не блокируют rebuild целиком —
  только свой ключ.
- **Dependencies (решение OPEN-V4-14).** Только ResourceKey →
  ResourceKey рёбра; закрытые роли: `material→texture: "texture"`,
  `composite→static_mesh: "placement_mesh"`,
  `composite→composite: "placement_composite"`, и с S5 —
  `static_mesh→material: "slot"` (до S5 slot-рёбер нет — принятый
  interim: dependents материалов не включают меши). Registry-tokens
  (actor class, material class/library parents) рёбрами НЕ являются.
  Рёбра извлекаются ПО КАНДИДАТУ и хранятся с provenance
  (`owner_path`); множество рёбер ключа = union по его кандидатам;
  unreadable/invalid кандидат рёбер не даёт (unknown ≠ empty).
  Import-blocked(key) = status ≠ unique ИЛИ существует ребро на
  import-blocked target — транзитивно; циклы невозможны (композитные
  отвергнуты грамматикой, межвидовой порядок ацикличен). Diagnostics
  хранит ТОЛЬКО derived-перевычислимые строки (входят в
  rebuild-identity); сессионные события (SELF_PUBLISHED, ход скана) —
  только Message Log/лог, в БД не пишутся.
- **Граница S4/S6 и self-publish (решение OPEN-V4-15).** S4 НЕ владеет
  watcher/debounce/PIE-очередью (это S6); S4 даёт: full scan (явный
  API + перевод `MHAnalyzeSources` на индекс), batched
  `UpsertPaths(paths)` — будущая точка входа watcher'а S6, и
  интеграцию Publish (S2/S3 вызывают upsert сразу после atomic
  replace). Self-publish token = in-memory кортеж {каноничный
  абсолютный путь, raw_hash, generation}; регистрируется публикатором
  СТРОГО после успешного atomic replace и до собственного upsert;
  первое совпадение (path, raw_hash) при upsert/scan классифицируется
  `SELF_PUBLISHED` и потребляет token (single-shot). Failed publish —
  токена нет; crash/restart — токены пропадают by design: корректность
  от них не зависит (после рестарта receipts дают NO_CHANGE), token —
  лишь подавление внутрисессионного шума. Probable rename считается на
  границе одной generation: `Disappeared` (ключи, потерявшие всех
  кандидатов) × `Appeared` (ключи с первым кандидатом), пара — только
  при same kind И биективном совпадении raw_hash внутри батча;
  many-to-many по одному hash → пары НЕТ и warning НЕТ.
  `MH_W_PROBABLE_RESOURCE_RENAME` — derived-диагностика, живёт пока
  условие наблюдаемо (orphan + появившийся ключ с совпадающим hash),
  alias не создаёт.
- **Orphan rebind (решение OPEN-V4-16).** Отдельной rebind-операции не
  существует: импорт ключа K целится в детерминированный путь §8; если
  там уже живёт managed-ассет (сирота K) — обычный полный in-place
  импорт переиспользует этот UObject, это и есть rebind. Метрика
  дивергенции БИНАРНАЯ и в одном домене: candidate `raw_hash` против
  receipt-`SourceHash` сироты (`AppliedHash` не сравнивается с raw
  никогда). Равенство → молчаливый rebind; ЛЮБОЕ отличие →
  `MH_W_ORPHAN_REBOUND_CONTENT_DIVERGED` в момент импорта (warning, не
  блок). Категория предупреждения — СЕССИОННОЕ СОБЫТИЕ импорта, как
  `SELF_PUBLISHED` (решение OPEN-V4-19): выдаётся в Message Log и отчёт
  выполняемой операции по наблюдённому в живом индексе переходу
  `orphan → rebind`; в таблицу Diagnostics НЕ пишется и в
  rebuild-identity НЕ входит. Обычный `stale` (правка файла на месте)
  rebind-предупреждением не является никогда. Потеря события при
  «orphan возник → SQLite удалён → rebuild → импорт» принята by design:
  чистая проекция не хранит историю, а при совпадении hash перед таким
  импортом всё равно стоит derived-предупреждение
  `MH_W_PROBABLE_RESOURCE_RENAME`. Rebind-импорт выполняется как обычный
  REIMPORT без дополнительных подтверждений — source побеждает. Слово
  «сильно» из прежней формулировки §2 упразднено — порогов и метрик
  подобия нет.

## 4. FBX-контракт v4 (поправки №2, №3)

FBX — односторонний транспорт Blender → UE и **обычный FBX без MH-метаданных**:
паспорт/descriptor/`mh_*` custom properties УДАЛЕНЫ. Односторонность —
свойство протокола: UE никогда не пишет FBX; при этом Blender имеет право
ЧИТАТЬ опубликованные `.mesh.fbx` как рабочую копию (§4.1). Вся
классификация — на стороне кастомного UE-импортёра по FBX SDK узлам:

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
- **Маркеры имён взаимоисключающи (решение OPEN-V4-11).** Узел несёт не
  более ОДНОГО маркера классификации, согласованного с типом узла.
  Fail-closed `MH_E_INVALID_NODE_MARKERS` (регистрируется в S3; проверяют
  Blender export и импорт §4.1, зеркалирует UE-импортёр S5): сочетание
  префикса `UCX_` с любым суффиксом `_cls_*` — включая семантически
  совпадающее `UCX_*_cls_both`; mesh-узел с префиксом `SOCKET_`;
  null-узел с `UCX_` или `_cls_*`; null-узел с терминальным `_lodNN`;
  `SOCKET_`-узел с детьми. Никакого precedence и никакого repair.
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
  `.material` — `MH_E_UNRESOLVED_MATERIAL_REFERENCE` (регистрируется в S3;
  общий код Blender-импорта §4.1 и UE-импортёра S5), блок ресурса. Никаких
  post-import скриптов.

### 4.1 Blender-импорт mesh FBX (рабочая копия из source)

Source tree — единственная истина пайплайна; `.blend` — рабочая копия.
Blender читает опубликованные `.mesh.fbx` (adoption, recovery, повседневное
редактирование), разворачивая их обратно в авторские структуры обратной
таблицей §4. Это НЕ двусторонность: UE FBX не пишет никогда, а
Blender-импорт ничего не пишет в source tree.

Оператор-обёртка над штатным импортёром; один вызов = один undo-шаг; сбой
любой стадии = полный откат дельты датаблоков — частично импортированного
состояния не существует. Стадии:

0. **Parse & preflight** (без мутаций сцены): собственный
   `parse_fbx`-ридер читает граф узлов, типы, иерархию и имена материалов
   по слотам; валидация тем же общим классификатором, что у экспортёра и
   UE: каноничный stem, mismatched `_lodNN` →
   `MH_E_INVALID_LOD_HIERARCHY`, конструкции вне диалекта (анимация,
   арматуры, NURBS) — reject. Предусловия занятости: целевая коллекция
   не существует И каждое имя узла свободно в `bpy.data.objects`, иначе
   `MH_E_IMPORT_TARGET_OCCUPIED` (регистрируется в S3) со списком
   конфликтов. Гарантия: Blender-авторенейм `*.001` в успешном импорте
   невозможен ни для одного датаблока, который переживает оператор.
1. **Материализация зависимостей до геометрии**: для каждого имени слота
   существующий Blender-материал `<name>` переиспользуется (материалы
   общие между мешами); отсутствующий строится ридером S2 из
   `<name>.material`; нет и файла — `MH_E_UNRESOLVED_MATERIAL_REFERENCE`,
   отказ.
2. **Геометрия**: штатный импортёр как чёрный ящик с пиноваными
   настройками, зеркальными экспорту (cm, Forward=X/Up=Z, custom
   normals, `use_image_search=False`), в staging-коллекцию; дельта
   objects/materials/images отслеживается. Реализация пинует конкретный
   импортёр (стабильный Python `io_scene_fbx`); переход на C++/ufbx —
   только отдельным решением owner, не дефолтом пользователя.
3. **Резолв по истине парса**: соответствие «узел ↔ объект» строится от
   парса стадии 0 (после preflight имена совпадают буквально); каждому
   слоту меша назначается канонический материал ПО ИНДЕКСУ слота из
   парса; плейсхолдерные материалы/изображения из дельты удаляются.
   Имена узлов не переписываются: совпадающий `_lodNN` — легальный
   authoring (OPEN-V4-3). Никаких эвристик по искажённым именам
   результата.
4. **Реструктуризация и отчёт**: сборка `<name>.lods`/`.lodNN`,
   aux-раскладка `UCX_`/`SOCKET_`/групп по конвенциям экспорта, снос
   staging; отчёт: LOD-уровни, материалы reused/created, предупреждения.

Non-goals (постоянные, честные потери транспорта): модификаторы, shape
keys, node trees материалов (восстанавливается только то, что строит
ридер S2) и прочие Blender-only данные из FBX не восстанавливаются.
Художник, которому нужны модификаторы, хранит личный `.blend`; пайплайн
от него не зависит.

Архитектурный seam: стадии 0/1/3/4 не зависят от декодера геометрии.
Замена стадии 2 собственным нативным декодером диалекта (без
плейсхолдеров вовсе) ратифицирована `ADR_V4_mh_asset_io.md` и выполняется
только срезом S7 (после S6); до S7 действует пиновка штатного импортёра
выше.

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
- **Закрытая грамматика (решение OPEN-V4-10).** Корень — объект ровно с
  одним полем `nodes` (массив, может быть пустым). Узел: `kind`
  обязателен (`mesh|actor|composite|group`); `resource` обязателен для
  mesh/actor/composite (canonical `[a-z0-9_]+`) и ЗАПРЕЩЁН для group;
  `name` опционален (непустая строка, display-only, identity не несёт);
  `transform` опционален — объект с опциональными `translation_cm`
  (ровно 3 числа), `rotation_quat` (ровно 4 числа `[x,y,z,w]`), `scale`
  (ровно 3 числа), дефолты — identity `[0,0,0] / [0,0,0,1] / [1,1,1]`;
  `children` опционален у ЛЮБОГО kind (массив узлов). Порядок
  `nodes`/`children` ЗНАЧИМ и сохраняется (это порядок компиляции);
  никакой сортировки узлов. Числа конечны (`MH_E_NAN_INF_VALUE`);
  компонента `scale` ≤ 0 — `MH_E_INVALID_SCALE` (правило v2 переносится);
  неизвестный `kind` — `MH_E_UNSUPPORTED_NODE_KIND`; duplicate JSON key
  (парсер обязан детектировать), неизвестное поле, неверный тип/арность,
  `resource` у group или его отсутствие у прочих —
  `MH_E_COMPOSITE_GRAMMAR` (регистрируется в S3). Reader ничего не
  игнорирует. Самовключение/предок — `MH_E_COMPOSITE_CYCLE`;
  нерезолвящийся `resource` (mesh/composite — ресурсы по §2, actor — имя
  в `ActorClassRegistry`) — `MH_E_UNRESOLVED_COMPOSITE_REFERENCE`; все
  блокируют ресурс и dependents.
- **Кватернион**: writer пишет нормализованный (float32) в каноническом
  знаке (`w > 0`; при `w == 0` — первый ненулевой компонент > 0); reader
  принимает любой знак, но `|‖q‖ − 1| > 1e-3` — `MH_E_COMPOSITE_GRAMMAR`.
  Publish каноникализирует норму и знак.
- **Канонические байты — режим §5**: UTF-8, LF, финальный LF, отступ 2;
  порядок полей узла `kind → resource → name → transform → children`,
  внутри transform — `translation_cm → rotation_quat → scale`; float —
  float32 shortest round-trip, целые без дробной части; identity-дефолты,
  пустые `transform`/`children` и отсутствующий `name` ОПУСКАЮТСЯ.
  Общее canonical-ядро с материалами; golden-векторы — общие файлы для
  pytest и UE Automation.
- **Transform contract (решение OPEN-V4-10).** Каноническое пространство
  хранения — конвенция UE: сантиметры, оси и handedness UE, кватернион
  `[x,y,z,w]` в смысле `FQuat`. Определяющее свойство вместо прозы о
  матрицах: composite-transform Blender-объекта ОБЯЗАН равняться мировому
  трансформу, который UE вычисляет для того же объекта, пришедшего через
  mesh-FBX транспорт §4 (cm, `axis_forward=X`, `axis_up=Z`).
  UE-компилятор потребляет значения без конверсии; Blender writer/reader
  выполняют зеркальную конверсию (Blender → JSON → Blender = identity в
  пределах float32). Обязательный parity-гейт: одно и то же размещение
  через FBX-узел и через composite даёт совпадающий мировой трансформ в
  UE (допуск float32). Прежний `core/transforms.py` authority НЕ является
  и переписывается под это свойство. Квантования нет: детерминизм — из
  float32 + shortest-записи.
- **Blender и actor tokens**: реестра акторов в Blender нет — Blender
  хранит и пишет `resource` актора lossless, не валидируя; валидация —
  только UE через `ActorClassRegistry`. Source-wins warning
  (`MH_W_MANAGED_ASSET_LOCALLY_MODIFIED`) относится ТОЛЬКО к managed
  `UMHCompositeAsset`; Blender-сцена — рабочая копия без applied state и
  без source-wins warnings.
- **Unresolved placement в Blender-авторинге (поправка owner).** При
  импорте композита в Blender ОТСУТСТВУЮЩИЙ mesh/composite ресурс узла —
  не блок, а `MH_W_UNRESOLVED_PLACEMENT` (регистрируется в S3.1) плюс
  видимый placeholder: Empty display-cube, красный object color, без
  контента; `resource`-token сохраняется lossless (custom property
  placement'а), Export пишет узел без изменений. Художник чинит
  остальное, не будучи заложником битого узла; строгость остаётся у
  потребителя — UE-сторона по-прежнему блокирует
  (`MH_E_UNRESOLVED_COMPOSITE_REFERENCE`). Послабление касается ТОЛЬКО
  отсутствующего ресурса: ambiguous same-kind duplicates остаются жёстким
  блоком по §2 и в Blender; mesh-FBX импорт §4.1 не меняется (unresolved
  material — по-прежнему E).

## 7. Applied state в ассетах (поправка №9)

- `UMHStaticMeshImportData : UAssetImportData` на UStaticMesh: SchemaVersion,
  LogicalName, SourceRelativePath, SourceRawHash, RecipeHash,
  AppliedAssetHash, LastSuccessfulTransaction. Штатные source-file записи
  (path/timestamp/MD5) — бесплатно от базового класса. Это receipt, не
  identity.
- `UMHMaterialSourceData : UAssetUserData` на MI: LogicalName,
  SourceRelativePath, SourceHash (raw, §3), AppliedHash (canonical
  extract, §5 / решение OPEN-V4-7), AppliedParent (решение OPEN-V4-9).
  `AppliedParent` — tagged logical token `class:<токен>` или
  `library:<имя>` (та же форма `tag:name`, что у
  `FMHResourceKey::ToString`; алфавит токена `[a-z0-9_]+`). Поле —
  receipt-only: import/publish/extract никогда не резолвят по нему;
  резолв — всегда source JSON + текущие registry-настройки, а extract
  делает live reverse-lookup фактического parent'а MI относительно
  текущих master/library roots. UE object path в поле не хранится.
  Список Asset Registry tags ниже не расширяется — `AppliedParent` в
  теги не выносится.
- `UMHCompositeAsset` — applied state зеркалом §5 (решение OPEN-V4-10):
  `SourceHash` (raw, §3) и `AppliedHash` — hash канонического JSON,
  извлечённого из применённого ассета тем же extractor'ом, что Publish
  Composite; детект локальной правки — как у материалов (re-extract vs
  `AppliedHash`; non-roundtrippable extract = локальная правка, warning).
  Аналога `AppliedParent` у композитов нет.
- Asset Registry tags — РОВНО ШЕСТЬ (поправка owner, решение OPEN-V4-13):
  `MH.Kind`, `MH.LogicalName`, `MH.SourcePath`, `MH.SourceHash` (raw,
  форма §3), `MH.AppliedHash`, `MH.Managed` — индекс строит
  GeneratedAssets только из них, не загружая UObject'ы. Прежняя фиксация
  «ровно пять» предшествовала удалению Ledger: без raw hash в тегах
  change detector был бы вынужден грузить каждый managed-ассет на каждом
  скане. Receipts внутри ассетов остаются authority, теги — их проекция;
  расхождение тега и receipt, обнаруженное при реальной загрузке, —
  `invalid_receipt` диагностика (§3).
- `UMHTextureSourceData : UAssetUserData` на managed UTexture (вводится в
  S4, решение OPEN-V4-13): LogicalName, SourceRelativePath, SourceHash
  (raw) + те же шесть тегов; прикрепляется существующим texture-import
  путём — с S4 текстуры managed. StaticMesh-receipts появляются только в
  S5; до этого mesh-строк в GeneratedAssets нет.
- **`AppliedHash` по kind (решение OPEN-V4-18).** Для kinds с
  канонической текстовой формой (material, composite) `AppliedHash` —
  hash канонического extract (V4-7/V4-10), домен отличен от raw. Для
  БИНАРНЫХ kinds (texture; static_mesh с S5) канонического extract не
  существует: applied state бинарного kind — это применённые
  source-байты, поэтому `MH.AppliedHash == MH.SourceHash` ПО
  ОПРЕДЕЛЕНИЮ (нормативное тождество, не эвристика; отдельное
  receipt-поле не добавляется — тег публикуется из `SourceHash`, тег
  никогда не пуст). Валидатор GeneratedAssets для бинарных kinds
  проверяет это тождество; расхождение — `invalid_receipt`. Запрет
  V4-16 «AppliedHash не сравнивается с raw» остаётся для канонических
  kinds, где домены разные; у бинарных домен один и тождество
  тривиально.
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
