# 10 — MH Source Protocol v5: random composites and placement seeds (FREEZE CANDIDATE)

Статус: **кандидат owner freeze V5-S0**. Owner merge среза V5-S0 означает
ратификацию этого документа и делает его единственным активным нормативом для
срезов `11_v5_agent_slices.md`. До такого merge запрещены любые production-code
изменения v5. Неразрешённые места перечислены в §13 и `QUESTIONS.md`; для них
действует STOP, а не подразумеваемая семантика.

Поколение v5 несовместимо меняет ТОЛЬКО `.composite` и добавляет новый resource
kind `.placement`. Контракты `.material`, `.mesh.fbx`, текстур, Project Resource
Index и applied state перенесены из 08; поле версии в них не добавляется.

Номера `S1`…`S7` внутри дословно перенесённых v4-абзацев — provenance прежней
реализации по 09, а не порядок v5. Активные gates всегда пишутся `V5-S*` и
определены только в 11.

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

Дополнение v5: `.composite` получает обязательный discriminator `"v": 5`,
random-узлы и parent-local T/R/S; `<name>.placement` хранит типизированный
placement profile версии 1. Seed принадлежит только размещению композита в UE.

## 2. Identity

```text
ResourceKey = Kind + LogicalName
static_mesh: garage_a          <- garage_a.mesh.fbx
material:    m_stucco          <- m_stucco.material
composite:   garage_type_a     <- garage_type_a.composite
placement_profile: vehicle_scatter <- vehicle_scatter.placement
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
  `.material`, `.composite`, `.placement`; для текстур — одиночного `<img-ext>` из
  allowlist §5); `foo.bar.mesh.fbx` невалиден.
- Одинаковый stem у разных kind — разные ResourceKey; одинаковый stem одного
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

**Аддитивная проекция v5.** Файл `.placement` классифицируется кандидатом kind
`placement_profile`; `.composite` без `"v": 5` имеет `invalid_payload` и не
даёт dependency edges. После ратификации binding из §6.3 добавляется закрытая
роль `composite→placement_profile: "placement_profile"`. Все остальные таблицы,
precedence rules, `mh.project_index:4`, шесть Asset Registry tags и rebuild-
контракт остаются дословно v4: новое поколение composite не является миграцией
SQLite и не создаёт вторую authority.

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
- **Sockets и collision — наблюдаемый результат (решение OPEN-V4-21).**
  Имя сокета = имя узла БЕЗ префикса `SOCKET_` (маркер — транспорт, не
  identity); пустой остаток — `MH_E_INVALID_NODE_MARKERS`; дубликаты
  имён сокетов внутри ресурса — `MH_E_INVALID_RESOURCE_SOURCE`.
  Трансформ сокета — global evaluated transform узла в пространстве
  ресурса. Каждый collision-узел (`UCX_*`/`*_cls_*`) → РОВНО ОДИН
  convex element = выпуклая оболочка его transformed control points;
  никакой декомпозиции, V-HACD и примитив-фиттинга — невыпуклое
  хуллится (стандарт UCX; художник декомпозирует сам); дегенеративная
  геометрия (<4 некомпланарных точек) — `MH_E_INVALID_RESOURCE_SOURCE`.
  Несколько узлов одного режима — каждый своим элементом. Per-shape
  `CollisionEnabled` по таблице выше; `CollisionTraceFlag =
  CTF_UseDefault`; авто-генерации коллизии при отсутствии узлов НЕТ —
  BodySetup остаётся пустым.
- **Transport-level отказы FBX (решение OPEN-V4-22).** Единый код
  `MH_E_FBX_TRANSPORT_FAILED` (регистрируется в S5; заменяет
  незарегистрированный probe-код `MH_E_GEOMETRY_SOURCE_MISMATCH` во
  всех call sites): отказ SDK init/чтения, corrupt scene, несовпадение
  axis/units после конверсии, неудачная triangulation, невалидные
  geometry layer indices, отказ axis-probe. Блок ресурса; slot-рёбра из
  неразобранного FBX не извлекаются (unknown ≠ empty, §3).
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
- Managed-текстура, чей canonical logical name равен `tex_n` либо оканчивается
  на `_tex_n`,
  импортируется как normal map с `sRGB = OFF` и UE compression setting
  `BC7` (`TC_BC7`). Эти параметры являются частью применённой import-policy:
  равный raw hash с другими значениями не считается `NO_CHANGE` и
  принудительно исправляется следующим import/reimport.
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
  срабатывает детект локальной правки). Blender-модель v4 использует
  property group mh4blend (`class|library` + поля грамматики) для явных
  authoring-overrides. Для class-формы writer автоматически читает уже
  заполненное семантическое содержимое `dagormat`: `shader_class`,
  `textures.tex0`–`tex15`, `optional` и `sides`; точечные значения mh4blend
  имеют приоритет. `sides=0|1` выводится явным `twosided=false|true`, потому
  что отсутствие поля означает default мастера; `sides=2` и любой optional
  тип вне number/vector4 блокируются
  `MH_E_MATERIAL_NOT_ROUNDTRIPPABLE` без потери данных. Из texture path
  writer берёт только extensionless logical stem и
  резолвит его по общему texture ResourceKey. Синтетический
  `tex16support`, dag4blend `is_proxy`/`proxy_path`, legacy-поля и UI-state
  не являются содержимым v4 и не сериализуются; proxymat-концепция
  superseded library-формой. Blender UI предоставляет внутри общей панели
  `MH Source Tools` блок `Misc` с двумя раздельными операциями: Copy All
  Textures копирует все непустые Dagor slots текущего blend по правилу
  `<external>/assets/<tail> -> <source_root>/assets/<tail>` (ровно один
  сегмент `assets`, полный preflight, sibling staging/read-back, locks,
  rollback набора при обычной ошибке); Remap All Texture Paths ничего не
  копирует, требует существования всех project-targets и затем меняет пути
  с read-back/rollback. Copy не меняет Blender paths, Remap не читает внешний
  файл; обе операции идемпотентны. FBX/Material Export не запускает их
  неявно.
- **Publish Material = полная перезапись source-файла без сравнения** (№11):
  extract MI → canonical JSON → sibling tmp → read-back → atomic replace →
  index upsert. Материал без source: диалог «папка + имя» (Adopt). Blender
  симметрично: экспорт обновляет все файлы материалов при включённой опции.
  Three-way diff/merge/`MH_E_MATERIAL_CONFLICT` — УДАЛЕНЫ из scope.
- Импорт: source всегда побеждает; локально изменённый MI перед перезаписью
  получает warning `MH_W_MANAGED_ASSET_LOCALLY_MODIFIED` (не блок).

## 6. Композиты v5: random, parent-local T/R/S и placement seed

`<name>.composite` — несовместимый JSON v5 без информации о материалах.
Корень всегда начинает поколение полем `"v": 5`. Версия относится ТОЛЬКО к
`.composite`: `.material`, `.mesh.fbx`, текстуры, Project Resource Index и
applied state не получают version-поля. Существующие v4-композиты временные:
миграции и dual-read нет, owner удаляет их и переэкспортирует.

Минимальный документ:

```json
{
  "v": 5,
  "nodes": []
}
```

Файл без поля `v` блокируется кодом
`MH_E_COMPOSITE_LEGACY_GENERATION` и сообщением: «файл прежнего поколения:
удалите и переэкспортируйте». Любое `v`, не равное целому `5`, блокируется
`MH_E_UNKNOWN_SCHEMA_VERSION`. Reader никогда не пытается угадать поколение
по остальным полям.

### 6.1 Закрытая грамматика Composite v5

Корень — объект РОВНО с полями `v` и `nodes` в этом каноническом порядке.
`nodes` — массив, может быть пустым. Kinds обычного узла:
`mesh|actor|composite|group|random|marker`.

- `kind` обязателен. `resource` обязателен для `mesh|actor|composite|marker`,
  запрещён для `group|random`. Resource token каноничен по §2.
- `name` опционален: непустая display-only строка, identity не несёт.
- `transform` опционален и содержит parent-local T/R/S: `translation_cm`
  (ровно три числа), `rotation_quat` (ровно четыре числа `[x,y,z,w]`),
  `scale` (ровно три числа). Дефолты identity:
  `[0,0,0] / [0,0,0,1] / [1,1,1]`.
- `children` опционален у любого обычного узла и является массивом узлов.
  Порядок `nodes` и `children` значим и сохраняется.
- `options` обязателен и разрешён ТОЛЬКО у `random`. Это непустой
  упорядоченный массив объектов РОВНО с полями `kind`, `resource` где
  применимо и `weight`. Kind option принадлежит
  `mesh|actor|composite|marker|empty`. У `mesh|actor|composite|marker` resource
  обязателен; у `empty` запрещён. `weight` обязателен, конечен и ≥ 0;
  хотя бы один option имеет weight > 0. Option не имеет `transform`,
  `children`, `options` или `name`: трансформ принадлежит random-узлу.
  Порядок options значим, zero-weight option входит в source closure, но
  никогда не выбирается.
- При resolution выбранный `mesh|actor|composite` option материализуется в
  transform random-узла; `empty` не создаёт leaf. `children` random-узла затем
  обходятся всегда и наследуют тот же random-node transform независимо от
  выбранного option. Selected composite разворачивается рекурсивно в этой
  parent-local basis.
- **`marker` (owner, документ 13 редакция 2 §6.4).** Именованная
  неисполняемая точка: `resource` — канонический токен, не внешний ресурс.
  Обычный marker сохраняет имя и transform в производном `Plan.Nodes`,
  выбранный marker-option — имя и world transform random-узла по пути
  `options[i]`; выбор фиксируется обычным `Decisions`. Ни один marker не
  создаёт `Leaves`, UE actor, Blueprint, ресурсный файл, registry binding
  или dependency edge. Его обычные `children` обходятся как у group.
  Это явная поправка протокола внутри V5-S6.1: необходима для gameObj,
  старые документы не используют marker и остаются байт-идентичными.
  Существующая исполняемая семантика MH `actor` не переопределяется.
- Все числа конечны; `MH_E_NAN_INF_VALUE` и `MH_E_INVALID_SCALE` сохраняют
  семантику v4. Кватернион writer нормализует в float32 и приводит к
  каноническому знаку v4; reader применяет тот же norm-admission v4.
- Duplicate JSON key, неизвестное поле, неверный тип/арность, нарушение
  resource/options/weight-правил и option-transform блокируют
  `MH_E_COMPOSITE_GRAMMAR`. Отдельное семейство random-grammar кодов НЕ
  вводится: это та же закрытая грамматика одного ресурса, а один стабильный
  код не заставляет Python и C++ расходиться на порядке проверок. Семантические
  отказы resolve/cycle/transform сохраняют отдельные доменные коды.
- Неизвестный kind — `MH_E_UNSUPPORTED_NODE_KIND`. Нерезолвящийся
  mesh/composite/actor option или обычный узел —
  `MH_E_UNRESOLVED_COMPOSITE_REFERENCE`. Cycle detection обходит ВСЕ
  composite-опции ВСЕХ random-узлов, не только выбранные seed'ом;
  самовключение и предок — `MH_E_COMPOSITE_CYCLE`.

Канонические байты сохраняют режим v4 §5: UTF-8, LF, финальный LF, отступ 2,
float32-shortest, целые без дробной части, identity-поля опускаются.
Порядок полей корня `v → nodes`; узла
`kind → resource → name → transform → profile → placement → appearance_seed_boundary
→ options → children`; transform
`translation_cm → rotation_quat → scale`; option `kind → resource → weight`.
Узлы и options не сортируются.

`profile` сохраняет прежнее место после transform. Порядок двух новых
опциональных носителей из документа 13 R2 §7 фиксируется после него
(закрытие OPEN-V5-21 owner'ом); пропуск profile в документе 12 — редакционный
остаток, не удаление действующего поля. Неизменённые документы дают прежние
байты; новые поля не меняют формулу ResolvedSignature или RNG. Их source bytes
участвуют в raw/closure hash обычным образом (§13.3).

### 6.2 Parent-local transform contract и shear

Трансформ каждого узла — parent-local:

```text
World(node) = World(parent) × Local(node)
World(top_level) = World(AMHCompositeActor) × Local(node)
```

Group — настоящий родительский transform. Его перенос, вращение и scale
математически влияют на всех потомков. Полные матрицы в `.composite` не
хранятся. Blender writer/reader выполняют зеркальную конверсию между Blender и
UE-конвенциями из §4; UE-компилятор потребляет JSON T/R/S без изменения
единиц/осей. Канонические числа и float32-shortest — как v4.

Shear и любая матрица, не представимая без потерь контрактным T/R/S,
отклоняются `MH_E_UNREPRESENTABLE_TRANSFORM` без decompose-аппроксимации,
snapping или repair на трёх границах:

1. Dagor import — lossless-conversion error с полным путём и именами узлов,
   создавших непредставимую композицию;
2. Blender Export Composite — preflight до staging с именами parent/child;
3. UE compile/Build/Break/cook — до мутации уровня или создания компонентов.

В частности, non-uniform parent scale × rotated child не может тихо менять
форму. Численный predicate admission для float host-матриц задан §13.5
(решение `OPEN-V5-5`): реконструкция host-декомпозиции, `8 ULP` в float32.

### 6.3 Placement profile `.placement`

`<name>.placement` — новый ResourceKey kind `placement_profile`. Это
типизированный JSON-ресурс, не текстовый препроцессор:

```json
{
  "v": 1,
  "kind": "placement_profile",
  "offset_cm": [[0, 10], [0, 10], [0, 0]],
  "rotation_deg": [[0, 0], [0, 0], [0, 180]],
  "uniform_scale": [1, 0.1],
  "vertical_scale": [1, 0.2]
}
```

Каждая пара имеет форму `[base, deviation]` и задаёт закрытый диапазон
`[base-deviation, base+deviation]`. Offset и rotation содержат по три пары
X/Y/Z; uniform и vertical — по одной паре. Все числа конечны, deviation ≥ 0.
Корень содержит обязательные `v` и `kind`; четыре parameter fields опциональны.
Отсутствующее поле означает отсутствие соответствующей variation и не
потребляет draw. `kind` обязан буквально равняться `placement_profile`, `v` —
целому 1; другая версия даёт `MH_E_UNKNOWN_SCHEMA_VERSION`.
Writer использует канонические байты §5 с порядком полей
`v → kind → offset_cm → rotation_deg → uniform_scale → vertical_scale`.
Неизвестное поле, duplicate key, неверный тип/арность или диапазон дают
`MH_E_PLACEMENT_GRAMMAR`.

Dagor `include` конвертируется importer'ом в такой профиль и typed reference.
Произвольный textual include/preprocessor в MH запрещён. То, что нельзя
перенести без потерь, — lossless-conversion error; importer не сохраняет
непонятный текст как скрытую authority.

Имя поля ссылки, порядок применения к Local T/R/S и политика scale-диапазона
заданы §13.2 (решение `OPEN-V5-2`).

**Blender-carrier и конвертация `include` (решение OPEN-V5-9).**

- Authority ссылки в Blender — typed поле `profile` в PropertyGroup
  `mh4blend` того же объекта (строка `[a-z0-9_]+` либо пустая = профиля
  нет), плюс НЕавторитетное зеркало `mh_composite_profile` по правилу
  зеркал §6.4. Никаких скрытых ключей и отдельных датаблоков.
- Имя профиля из Dagor `include` берётся **дословно** как stem файла
  include без расширения. Если stem уже каноничен (`[a-z0-9_]+`) — это и
  есть logical name. Неканоничный stem — fail-closed
  `MH_E_NONCANONICAL_RESOURCE_NAME` с путём include. Нормализация,
  lowercase, sanitize и генерация имён ЗАПРЕЩЕНЫ: имя — identity, а
  тихое переименование запрещено везде (§2).
- Содержимое include конвертер **материализует** в `<name>.placement`
  рядом с производимым `.composite` (структура папок свободна, §2, а
  identity — только имя). Запись — штатная атомарная публикация.
- Коллизия имени: профиль с таким именем уже существует → сравнить
  канонические байты. Идентичны — переиспользовать без записи (типичный
  случай: один include в нескольких композитах). Отличаются —
  fail-closed `MH_E_AMBIGUOUS_RESOURCE_NAME` с обоими путями; перезапись
  чужого профиля запрещена, победитель не выбирается.
- Непереносимый параметр внутри include — lossless-conversion error, как
  и прежде; скрытый «сырой текст на будущее» не сохраняется.

### 6.4 Blender authoring и Dagor import

В Blender seed не существует ни в каком виде: ни в PropertyGroup, ни в custom
properties, ни в UI, ни в `TECH`. Blender готовит source asset и всегда
загружает ВСЕ варианты ВСЕХ random-узлов; выбор делает только UE resolver.

Допустимы строго четыре служебные сцены: `COMPOSITE`, `MESH`,
`ACTOR_PLACEHOLDERS`, `TECH`. `TECH` содержит только preview helpers и никогда
не является source authority.

Random-node — Empty с `kind=random` в PropertyGroup `mh4blend`. Его дети-
options — Empty с `instance_collection` на чистую resource Collection. На
option-Empty находятся typed `weight` и `mh_option_index`; resource Collection
о weight/index не знает. Option transform display-only и экспортом игнорируется.
Export сортирует options по `mh_option_index`. Отсутствующий/не-int index,
дубликат индекса или невалидный weight блокирует export; duplicate index имеет
код `MH_E_DUPLICATE_RANDOM_OPTION_INDEX`. Автоматическая перенумерация и
сортировка по Outliner/имени запрещены.

**Ключи и зеркало (поправка owner по референсу dag4blend).** Authority —
typed PropertyGroup `mh4blend` на объекте: `kind`, `weight`, `option_index`.
Дополнительно import/export пишут на тот же Empty НЕавторитетное зеркало в
обычные ID custom properties с закреплёнными именами —
`mh_composite_kind` (`"random"` на random-узле), `mh_random_weight` (float),
`mh_random_option_index` (int). Зеркало существует ровно для внешних
инструментов и диагностики: reader/writer MH читают ТОЛЬКО typed-данные, при
расхождении typed побеждает, правка зеркала художником ничего не меняет. Это
тот же приём, что receipts↔Asset Registry tags (§7): одна истина, одна её
проекция.

**Незаполненный typed `kind` выводится из графа сцены, а не из зеркала.**
Правило «зеркало не authority» касается ИМЕННО зеркала и не требует ручного
клеймения каждого размещения. Если `mh4blend.kind` не задан, writer выводит
его из того, чем объект фактически является: instance ресурсной Collection
даёт kind этой Collection, а обычный Empty без instance — `group` (у опции —
`empty`). Это чтение графа сцены — того же источника, который и так решает
resource identity, — а `mh_composite_kind` не читается никогда. Заданный typed
`kind` всегда побеждает вывод; расхождение с Collection остаётся
`MH_E_RESOURCE_KIND_MISMATCH`. `random` НЕ выводится: он существует только как
явное typed-намерение. Без этого правила сцены, собранные руками до v5,
переставали экспортироваться — регрессия `04b4ceb`.

Панель Options предоставляет Add / Remove / Up / Down / weight и меняет typed
option data.

**Наблюдаемая семантика load modes и reuse/refresh (решение OPEN-V5-10).**

Три режима отличаются ТОЛЬКО объёмом импортируемой геометрии; дерево
размещений строится полностью всегда — иначе теряется смысл «Blender
загружает все варианты».

- `full-LOD` — mesh-определение содержит ровно то, что даёт §4.1: все
  authored LOD, коллизии, сокеты, материалы.
- `LOD0` — импортируется только render-mesh уровня lod00; коллизии,
  сокеты и группы импортируются (это структура, она дёшева), материалы
  резолвятся; старшие LOD опускаются.
- `structure-only` — геометрия не импортируется вовсе: определения
  создаются пустыми штампованными Collection, размещения их инстансят,
  дерево видно по Empty. Материалы не загружаются.

**Определение, созданное в `LOD0` или `structure-only`, НЕПОЛНОЕ** и
штампуется `mh_incomplete_import = True`. Из такого определения экспорт
mesh-ресурса fail-closed запрещён (иначе неполный импорт затёр бы
source); `reuse` его не принимает; composite-export не затрагивается —
он публикует размещения, а не геометрию.

- `reuse` (по умолчанию): перед созданием определения ресурса ищется
  существующая managed Collection с штампами (kind, logical name). Полная
  и managed — переиспользуется КАК ЕСТЬ, её содержимое не трогается.
  Неполная (`mh_incomplete_import`), placeholder или unmanaged с тем же
  именем — fail-closed; молчаливое дополнение/замена запрещены.
- `refresh` (явный опт-ин): определение переимпортируется, даже если
  существует. **Datablock Collection сохраняется in-place** — внешние
  пользователи (размещения в других композитах, ссылки сцен) переживают
  операцию; заменяется только содержимое.
- Рекурсивный импорт — одна undo-транзакция и одна rollback-граница:
  при ошибке ЛЮБОГО определения откатывается вся дельта. Частичного
  refresh не существует. Смешанное замыкание (часть определений есть,
  часть нет) — нормальный случай: существующие переиспользуются или
  обновляются по режиму, отсутствующие создаются, и всё это в одной
  транзакции.

**Контракт конвертации Dagor→MH.** Источников два, оба обязательны.
(1) Прямой разбор `*.composit.blk` — авторитетный путь.
(2) Конвертация УЖЕ импортированной dag4blend-сцены — рабочий путь художника,
у которого композит уже открыт. Для (2) действуют правила:

- random-узел распознаётся по ЛЮБОМУ из двух признаков: узел-Empty, чья
  `instance_collection` является helper-коллекцией `random.*` с
  ent-Empty внутри, ЛИБО явный маркер `type:t == "random"` (его ставит
  overlay-патч dag4blend; на непропатченной установке его нет, поэтому
  опираться только на маркер нельзя);
- вес берётся из `dagorprops['weight:r']`, при отсутствии — из ID custom
  property `weight:r` (зеркало overlay-патча), при отсутствии обоих — ровно
  `1` (Dagor не пишет значение по умолчанию);
- **options поднимаются из helper-коллекции в дети random-Empty.** dag4blend
  держит варианты в отдельной коллекции внутри служебной сцены и инстансит
  её; у нас `TECH` никогда не является source authority (выше), поэтому
  конвертер переносит варианты в дети узла и назначает `option_index` по
  порядку их появления в исходном `.blk`/коллекции;
- `type:t` варианта (`composit`/`rendinst`/`prefab`/`gameobj`) отображается в
  наш `kind` (`composite`/`mesh`/`mesh`/`actor`); неизвестный или
  отсутствующий тип — fail-closed, угадывание по имени запрещено. Импорт рекурсивен по всем options,
поддерживает structure-only / LOD0 / full-LOD и явные reuse/refresh definitions.
Неразрешённый option следует v4 Blender placeholder policy, но остаётся в
source closure; ambiguous same-kind resource блокирует импорт.

Обычный или option actor token Blender хранит lossless в
`ACTOR_PLACEHOLDERS` и не сверяет с UE registry. Отсутствующий mesh/composite в
Blender даёт `MH_W_UNRESOLVED_PLACEMENT` и видимый placeholder с сохранением
token; ambiguous same-kind остаётся блоком. UE source validation обязана
резолвить каждый обычный reference и все options через index/ActorClassRegistry;
неразрешённый reference блокирует composite и dependents. Если уже применённая
dependency позже исчезла, derived preview показывает per-leaf placeholder до
восстановления same-name resource и dependency notify/Rebuild.

### 6.5 Source closure, resolved plan и export

Существуют два разных замыкания, смешивать их запрещено:

- **Source closure** — root плюс все обычные зависимости и ВСЕ options ВСЕХ
  random-узлов рекурсивно. Используется для validation, cycle detection,
  Include-All export, cook dependency admission и Find Broken References.
  Source closure никогда не строится из seed.
- **Resolved plan** — один результат resolver'а для конкретного
  (root composite, placement Seed, closure). Он содержит только выбранные
  зависимости/leaves для preview, Break, runtime и cook flattening. Thumbnail
  потребителем plan НЕ является (§13.12).

Blender export-команды:

1. `Export Composite` — только выбранный root;
2. `Export Composite + Composite Closure` — root, полное nested-composite
   closure и все referenced placement profiles; mesh/material payloads не
   добавляет;
3. `Export Composite Include All Stuff` — то же плюс meshes и materials;
   обход всех random options обязателен. Textures в публикуемый набор НЕ
   входят ни в одной команде (§13.10).

Batch сначала строит и валидирует полный source closure без записи. Существующий
незагруженный managed source переиспользуется байт-в-байт без перезаписи;
отсутствующий или unmanaged dependency блокирует preflight. Исключение из
ПУБЛИКУЕМОГО набора не является исключением из ЗАМЫКАНИЯ (§13.11): mesh,
material и texture зависимости, которые команда не публикует, обязаны
разрешаться в существующий canonical managed source, иначе весь батч блокируется
до staging. Затем ВСЁ замыкание
пишется в staging и read-back проверяется до первой публикации. Publish order
(решение `OPEN-V5-2`, §13.2): placement profiles → materials → meshes → leaf
composites → parents → root LAST. Профили идут первыми как листья зависимостей
композитов; batch без профилей просто начинает со следующего шага. Этот порядок
и есть гарантия консистентности при наблюдении извне: любой ПРЕФИКС публикации —
уже замкнутое множество, поэтому watcher, среагировавший на одиночное событие
в середине батча, никогда не видит ресурс с неопубликованными зависимостями.

Self-publish token — механизм ВНУТРИ UE и только для собственных записей UE в
source tree (`Adopt`, `Delete resource`): `FMHProjectResourceIndex::
RegisterSelfPublishAfterReplace` регистрирует `(path, raw_hash, generation)`,
и событие помечается `SELF_PUBLISHED` лишь при совпадении хеша. Blender — ВНЕШНИЙ
publisher: он не выпускает токенов, watcher его события не подавляет, и
cross-process транспорт не вводится (§13.9).
После первого успешного replace общий rollback не обещается: ошибка сообщает
точный published/unpublished set как `MH_E_PARTIAL_PUBLISH`; откат выполняет
VCS, теневой transaction manifest запрещён.

### 6.6 `mh.random_stream:1` и `FMHResolvedCompositePlan`

Один кросс-hostовый RNG имеет tag `mh.random_stream:1`. Python reference V5-S1
и C++ V5-S2 обязаны быть бит-идентичны. Запрещены `FMath::Rand`,
`FRandomStream` и порядок hash/map/set контейнеров как источник результата.

Один int32 `Seed` принадлежит `AMHCompositeActor` и рекурсивно определяет все
вложенные random-узлы. Seed 0 валиден при явном значении; auto-seed при создании
всегда non-zero. Перемещение/вращение/scale актора Seed не меняет. Duplicate по
умолчанию создаёт новый seed; явная опция Keep Seed копирует старый.
`InstanceSeed` не существует. Blender seed не хранит.

Draw-order фиксирован:

```text
option selection
offset X, Y, Z
rotation X, Y, Z
uniform scale
vertical scale
depth-first child traversal in source order
```

Отсутствующий profile-параметр draw не потребляет. Порядок options значим.
Cycle/source-closure validation всегда предшествует resolution и обходит все
options.

Resolver возвращает ровно один immutable `FMHResolvedCompositePlan`:

- decisions: NodePath, selected option index, raw draw/sample и веса;
- leaves: kind, resource, world matrix и provenance;
- SelectedDependencies;
- ResolvedSignature = hash(closure hash + Seed + selected indices + samples +
  resolver version).

Editor preview, Show Resolved Choices, Show Decision Trace, Break,
`AMHRuntimeCompositeActor`, PIE, packaged runtime и cook обязаны потреблять этот
же plan. Custom asset-thumbnail в этот список не входит и остаётся отключённым
(§13.12). Random-selection внутри component spawning запрещён. Level Instance
random не резолвит никогда; в будущем он допустим только как optional backend
уже выбранного plan-варианта.

Битовый алгоритм/инициализация stream, отображение int32 Seed в state, draw →
`[0,1)`/weighted interval, stable NodePath encoding, closure-hash serialization,
signature hash/tag и resolver-version token ЗАДАНЫ в §§13.1 и 13.3
(решения `OPEN-V5-1`/`OPEN-V5-3`); Dagor probe выполнена, owner выбрал вариант
B — совместимость поведенческая, байты `mh.random_stream:1` окончательны.
Потоки **выводятся из пути** (поправка owner, §13.8): единого сквозного
stream нет — каждый узел, которому нужна случайность, открывает собственный
поток от `mix(placement Seed, hash(NodePath))`. Depth-first обход в порядке
источника сохраняется, но определяет теперь только ПОРЯДОК ЗАПИСЕЙ в плане
(decisions/leaves, а значит и подпись), а не позиции в потоке.

### 6.7 UE editor, runtime и cook

`AMHCompositeActor` — persisted level-placement: ссылка на
`UMHCompositeAsset`, actor transform, int32 `Seed`, `bAutoSeed` и read-only
derived `ResolvedSignature`. Компоненты и decision trace derived/transient.
Dependency notify пересобирает plan и preview без пересохранения уровня.

V5-S5 добавляет Reseed / Randomize Selected / Copy Seed / Paste Seed / Lock Seed /
Keep Seed on Duplicate / Show Resolved Choices / Show Decision Trace.
Build Composite сериализует parent-local T/R/S. Edit Composite публикует source
и пересобирает все placements. Break потребляет resolved plan текущего Seed и
полностью материализует его leaves: selected mesh leaves становятся
StaticMeshActor, selected gameplay leaves — акторами, nested composites/groups
растворяются; source и asset не меняются. Любая непредставимая итоговая
матрица блокирует всю операцию
`MH_E_UNREPRESENTABLE_TRANSFORM` до мутаций. Файловые side-effects и undo
имеют строгую границу: UE transaction закрывается до Publish, Undo не
восстанавливает source bytes, VCS — единственный rollback source.

Основной runtime path — `AMHRuntimeCompositeActor`, использующий тот же plan:
Editor = PIE = packaged по decision trace и ResolvedSignature. Только после
этого V5-S7 строит cook flattening: каждый placed actor резолвится по своему Seed;
static leaves материализуются в ISM/HISM/StaticMeshActor, gameplay leaves — в
самостоятельные actors, groups/nested composites растворяются, wrapper
удаляется. World Partition/OFPA validation и cook smoke обязательны.
Level-Instance cache (V5-S8) запаркован и ничего не блокирует.

### 6.8 Первый end-to-end golden GAZ-53

Freeze fixture состоит из трёх v5 composite-файлов:

```text
gaz53_b_random_cmp.composite
  -> composite:gaz53_b_body_cmp
  -> composite:gaz53_body_bc_random_cmp
       -> one random node, three ordered mesh options, weight 1 each
```

Обязательные проверки по мере срезов:

- Dagor → Blender → MH → export сохраняет порядок и веса; implicit Dagor
  weight=1 материализуется в явный canonical weight;
- source closure содержит все три option resources, включая невыбранные;
- parent 100 + child local 25 даёт world 125, движение parent двигает child;
- shear отклоняется без TRS approximation;
- Seed set `{0, 1, 2, 42, 123, 1024, 2147483647}` получает фиксированные
  choices/traces/signatures;
- Python reference = UE Automation = PIE = packaged;
- placements независимы: seed 100 == seed 100, seed 100 != seed 200;
  перемещение placement не меняет result.

V5-S0 фиксирует topology/source fixtures. Owner передал исходные
`*.composit.blk` (§13.6), поэтому synthetic option tokens заменены реальными и
GAZ-фикстура является content authority; guard —
`test_gaz_protocol_fixture_contains_no_synthetic_option_tokens`. Expected seed
traces/signatures заполнены V5-S1 после ратификации `OPEN-V5-1`/`OPEN-V5-3`;
подстановка случайных ожидаемых значений запрещена.

## 7. Applied state в ассетах (поправка №9)

- `UMHStaticMeshImportData : UAssetImportData` на UStaticMesh (решение
  OPEN-V4-20; поля v3-черновика упразднены): LogicalName,
  SourceRelativePath, SourceHash (raw, §3), ImporterVersion (int32 —
  монотонная константа кода импортёра; отличие receipt от текущей →
  REIMPORT даже при равном raw hash; bump — только owner-подтверждённым
  изменением build-семантики), bLocallyModified (bool, §9).
  `SchemaVersion`, `RecipeHash`, `AppliedAssetHash`,
  `LastSuccessfulTransaction` НЕ существуют: канонического fingerprint'а
  собранного UStaticMesh нет (binary kind, V4-18), а транзакций нет —
  сам receipt и есть запись последнего успеха, commit строго после save
  package (ниже). Штатные source-file записи (path/timestamp/MD5) —
  бесплатно от базового класса. Это receipt, не identity.
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
- `UMHCompositeAsset` — applied state зеркалом §5 (решение OPEN-V4-10), но
  canonical payload теперь строго v5 по §6:
  `SourceHash` (raw, §3) и `AppliedHash` — hash канонического JSON,
  извлечённого из применённого ассета тем же extractor'ом, что Publish
  Composite; детект локальной правки — как у материалов (re-extract vs
  `AppliedHash`; non-roundtrippable extract = локальная правка, warning).
  Аналога `AppliedParent` у композитов нет.
- `AMHCompositeActor.ResolvedSignature` — derived результат resolver'а §6.6,
  а не identity/source receipt/Asset Registry tag. Он пересчитывается из
  closure+Seed и не меняет шесть тегов managed asset.
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

Carrier/generated-path/applied receipt для `placement_profile` задан §13.4
(решение `OPEN-V5-4`) и §13.4.1: отдельного UAsset, седьмого tag и generated
path нет, значения инлайнятся в `UMHCompositeAsset` как applied state, а
freshness закрывает приватный `AppliedSourceHash`.

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

`placement_profile` generated path не имеет вовсе (§13.4): он инлайнится в
композит и `.uasset` не порождает.

## 9. Политика UE-редактирования

StaticMesh — source-generated: правки в Static Mesh Editor не
экспортируются и перезаписываются следующим reimport. Детект локальной
правки (решение OPEN-V4-20) — БЕЗ fingerprint'а: editor-hook
(`OnObjectModified`/`PostEditChange`) на managed-мешах ставит
persisted-флаг `bLocallyModified` в receipt;
`MH_W_MANAGED_STATIC_MESH_LOCALLY_MODIFIED` (dev: warning; strict/CI:
error) выдаётся в точках, где ассет и так загружен: при импорте (перед
перезаписью; успешный apply + save сбрасывает флаг) и в явных
Verify-командах S6. Обычный scan мешей не загружает и локальные правки
не видит — принято by design; правки при выключенном плагине хуком не
ловятся — тот же best-effort класс, что V4-19. Явный пользовательский
Reimport всегда пересобирает полностью, игнорируя NO_CHANGE.
MI и Composite двусторонние только через явный Publish.

## 10. Архитектура импортёра

Stock Interchange НЕ используется как основа. Сохраняется seam:

```text
Autodesk FBX SDK -> IMHGeometryTranslator -> FMHSceneIR
  -> FMHStaticMeshBuildPlan -> FMHStaticMeshBuilder -> UStaticMesh
  -> UMHStaticMeshImportData
```

Порядок импорта: textures → materials → static meshes → placement profiles →
leaf composites → parents → root. Внутри composite source closure обходятся
все random options; resolved plan не участвует в import-order.
Resolver: `IMHSourceResolver::Resolve(FMHResourceKey)`.

## 11. Миграция

**Миграции и dual-read нет.** Все существующие `.composite` считаются
временными. V5-S2 физически удаляет v4 document-world parser/writer/compiler
paths, v4 composite goldens и тесты (включая решение OPEN-V4-24) и заменяет их
v5. Owner удаляет старые source-файлы и переэкспортирует. Файл без `"v"` не
чинится и не интерпретируется: `MH_E_COMPOSITE_LEGACY_GENERATION` с сообщением
из §6. Материалы, meshes, textures, индекс и их applied state не мигрируют.

## 12. Судьба существующих нормативов

| Документ/область | Судьба в v5 |
|---|---|
| 08 §§1–5, 7–10 | перенесены сюда; identity, index, mesh, material, texture, applied-state и generated-path контракты выживают, кроме явно перечисленных аддитивных `.placement` пунктов |
| 08 §§6, 6.1 и OPEN-V4-24 | superseded целиком: document-world, structural-only group и старые Build/Break assumptions физически удаляются в V5-S2 |
| 08 §11 | заменён §11 этого документа: v4 composite не мигрирует и не dual-read'ится |
| 09 S0–S6 | историческая карта завершённой реализации v4; не является порядком работ v5 |
| ADR_V4_mh_asset_io / 09 S7 | отдельный parked backlog; v5 S0–S7 не блокирует и сам не меняет |
| 00–07, ADR_V2/V3, оба AMENDMENT, RISK_RESULTS, ROADMAP | сохраняют прежние v4 supersede-баннеры; новая authority — этот документ, их surviving statements уже перенесены сюда |
| QUESTIONS | OPEN-V4-24 остаётся историей отменённого document-world решения; OPEN-V4-1 перенесён в OPEN-V5-7; активные дыры — только `OPEN-V5-*` |
| C0/C1 audit reports и receipts v4 | исторические квитанции, не норматив и не acceptance v5 |

## 13. Решения owner по вопросам v5

Семь вопросов freeze (`OPEN-V5-1…7`) решены в §§13.1–13.7; ожидание GAZ-oracle
закрыто (§13.6). Позднее добавлены §13.4.1 (`OPEN-V5-8`), §13.8 (path-derived
seeds, поправка owner) и §§13.9–13.11 (`OPEN-V5-11…13`, подняты V5-S4);
§13.12 (`OPEN-V5-14`, поднят V5-S5); §6.3/§6.4 несут решения
`OPEN-V5-9`/`OPEN-V5-10`. Активных STOP-гейтов по v5 нет.

### 13.1 `mh.random_stream:1` — битовый контракт (OPEN-V5-1)

Baseline фиксируется СЕЙЧАС, чтобы V5-S1 строил и тестировал реальный
reference; probe может его заменить только явным owner-решением (вариант A).

- State: `uint64`, вся арифметика — по модулю 2^64.
- Инициализация из int32 Seed: `state = splitmix64(uint64(uint32(seed)))`
  (bit-cast signed→unsigned, затем один шаг ниже).
- `next_u64()` — splitmix64:
  `state += 0x9E3779B97F4A7C15; z = state;`
  `z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9;`
  `z = (z ^ (z >> 27)) * 0x94D049BB133111EB;`
  `return z ^ (z >> 31);`
- `next_u32() = uint32(next_u64() >> 32)` (старшие биты).
- `next_unit() = next_u32() * 2^-32` — float64 в `[0, 1)`.
- Weighted selection: `total` — последовательная float64-сумма весов В ПОРЯДКЕ
  опций; `target = next_unit() * total`; идём по кумулятивной сумме в том же
  порядке и берём ПЕРВУЮ опцию с `cumulative > target` (строго больше).
  Нулевые веса не увеличивают сумму и не выбираются никогда; `next_unit() < 1`
  гарантирует, что последняя положительная опция всегда ловит остаток.
- Random-узел ВСЕГДА потребляет ровно один selection-draw, даже при единственной
  положительной опции: позиция потока не зависит от содержимого данных.
- Отсутствующий параметр профиля draw НЕ потребляет (подтверждение прежнего
  решения); порядок draw — как в §6.6.
- Sample диапазона: `value = base + (next_unit() * 2 - 1) * deviation` во
  float64, затем округление до float32 при записи в план.
- **Dagor parity probe ВЫПОЛНЕНА (V5-S1); owner выбрал ВАРИАНТ B —
  поведенческая совместимость, собственный RNG.** Проба показала: Dagor
  использует 32-битный LCG с 15-битной выборкой (`objgenPrng.h`), выбор
  расходится с baseline на 5 seed'ах из 7, а привязка осей трансформа вообще
  требует наблюдения живого рантайма, который не запускался (`not_run`).
  Bit-for-bit паритет отклонён по трём причинам: (1) Dagor не участвует в
  shipping-пути — композиты конвертируются один раз, а не со-симулируются;
  (2) placement seed назначается заново в UE при создании размещения, готовых
  «запечённых» даговских seed'ов, которые обязаны воспроизвестись, не
  существует; (3) паритет, выведенный из чтения исходников без живого
  рантайма, недоказуем — а заявлять недоказанное запрещено. Цена варианта A
  была бы реальной: 15 бит выборки — грубое разрешение для непрерывных
  диапазонов offset/rotation/scale. Совместимость с Dagor остаётся
  ПОВЕДЕНЧЕСКОЙ: weighted options, наследование seed вложенными композитами,
  per-placement seed, порядок draw. Тег `mh.random_stream:1` остаётся, его
  байты — окончательные; формулировки вида «тот же seed, что в Dagor» в
  документации, UI и квитанциях запрещены. Source-derived векторы пробы
  сохраняются как историческая улика, шаблон runtime-наблюдения гейтом больше
  не является.

### 13.2 Binding и применение `.placement` (OPEN-V5-2)

- Поле ссылки — `"profile": "<name>"`, допустимо на УЗЛЕ любого kind
  (`mesh|actor|composite|group|random`); на опции random-узла — запрещено
  (опция не имеет собственного трансформа, §6.1).
- Применение — покомпонентно НАД authored Local T/R/S, без матричного
  умножения (домен остаётся T/R/S, shear не возникает):
  `translation = t_authored + offset_sample`;
  `rotation = r_authored * q_sample`, где
  `q_sample = qZ * qY * qX` из `rotation_deg` (углы X/Y/Z, UE-конвенция);
  `scale = s_authored * (u, u, u * v)`, где `u` — `uniform_scale` sample,
  `v` — `vertical_scale` sample (вертикаль — ось Z UE).
- Невалидно ДО сэмплинга (fail-closed при парсе профиля,
  `MH_E_PLACEMENT_PROFILE_GRAMMAR`, регистрируется в V5-S2): `deviation < 0`;
  нефинитные числа; `base - deviation <= 0` у любого scale-диапазона (правило
  «scale ≤ 0 запрещён» переносится из v4).
- Порядок публикации батча: **профили публикуются первыми**, до материалов —
  они листья зависимостей композитов; далее прежний порядок §6.5.

### 13.3 NodePath, closure hash и `ResolvedSignature` (OPEN-V5-3)

- NodePath: сегменты `nodes[i]`, `children[j]`, `options[k]`, соединённые `/`;
  пересечение границы вложенного композита обозначается `>` и логическим
  именем: `root_cmp:nodes[1]/options[2]>variant_cmp:nodes[0]`.
- `closure_hash` = BLAKE3-160 конкатенации raw payload hashes ВСЕХ ресурсов
  source closure (все опции всех random-узлов, §6.5), отсортированных по
  `ResourceKey.ToString()`; форма — `blake3-160:<40 hex>`.
- Прообраз подписи — канонический JSON (та же машинерия §5) фиксированной
  структуры: `{"v":1,"resolver":"mh.random_resolver:1","seed":<int32>,`
  `"closure":"<closure_hash>","decisions":[{"path","option","total","draw"}...],`
  `"leaves":[{"kind","resource","trs"}...]}`, элементы — в порядке резолва,
  числа — float32 shortest round-trip.
- Display-only `name`, любые файловые пути и абсолютные локации в прообраз НЕ
  входят: подпись — функция identity и геометрии, не презентации.
- `ResolvedSignature` = `blake3-160:<40 hex>` от прообраза.

### 13.4 UE carrier профиля (OPEN-V5-4)

Отдельного UAsset у `placement_profile` НЕТ; седьмой Asset Registry tag и
generated path не вводятся. Значения профиля **инлайнятся в
`UMHCompositeAsset`** при импорте композита, который на них ссылается — это
applied state внутри ассета (§7), поэтому cook и runtime никогда не читают
source tree. Индекс хранит ребро `composite→placement_profile: "profile"`
(закрытая роль добавляется к §3), поэтому правка `.placement` помечает
dependent-композиты `stale` и вызывает их реимпорт обычным порядком. Kind
`placement_profile` участвует в scan/resolve как обычный source-ресурс
(duplicate/ambiguous — стандартная политика §2), но GeneratedAssets-строки не
имеет.

### 13.4.1 Durable freshness инлайненного профиля (решение OPEN-V5-8)

Аудит V5-S2 нашёл реальную дыру в §13.4: после рестарта индекс не отличает
«в ассете лежит профиль H1, а в source уже H2» от «в ассете H2 и в source H2» —
файл `.composite` в обоих случаях один и тот же, а шесть тегов hash
применённых inline-байтов профиля не несут. Принят **вариант A**.

- В `UMHCompositeAsset` каждый инлайненный `FMHPlacementProfile` получает
  приватное поле `AppliedSourceHash` — raw hash (§3) того `.placement`, из
  которого профиль был применён. Поле editor-only и НЕ участвует ни в wire
  JSON `.composite`, ни в каноническом extract, ни в `MH.AppliedHash`, ни в
  шести тегах, ни в SQLite. Форматы и грамматика не меняются.
- **Индекс остаётся чистой проекцией.** `GeneratedAssets.status` профильной
  свежести НЕ отражает: у такого композита source-файл не менялся, тег
  совпадает, статус честно `applied`. Ослабление §3 (вариант C) отвергнуто —
  это несущая аксиома, и «немного истории» в кэше её ломает.
- Свежесть проверяет ИМПОРТЁР, а не индекс: для ключей, у которых есть хотя бы
  одно ребро `composite→placement_profile` (наличие ребра индекс знает без
  загрузки) И план дал бы `NO_CHANGE`, ассет загружается, и каждый инлайненный
  `AppliedSourceHash` сверяется с raw hash текущего `.placement`; расхождение
  повышает `NO_CHANGE → REIMPORT`. Композиты без профилей не загружаются
  никогда. Это тот же приём, что `ImporterVersion` и `bLocallyModified`
  (§13-решения V4-20): часть staleness принадлежит импортёру, а не проекции.
- Вариант B (новый/переопределённый hash-домен) отвергнут: он вынудил бы
  тег композита перестать равняться raw hash кандидата, что задевает правило
  `applied/stale` §3 И метрику orphan-rebound §2 (там сравниваются ровно
  candidate raw hash и receipt `SourceHash`; у профильного композита они
  перестали бы совпадать никогда, давая ложную дивергенцию на каждом
  rebind). Радиус поражения больше выигрыша.
- **Честное следствие, которое нельзя «чинить» позже:** index-производные
  представления (Find Broken References, Show Duplicates, отчёт скана) НЕ
  показывают профильную устарелость — её показывает план импорта. Попытка
  вынести её в индекс = вернуть вариант B/C.
- Ратифицированы как постоянные усиления, найденные тем же аудитом: exact-byte
  ревалидация всех profile payloads перед ПЕРВОЙ мутацией UAsset (профиль не
  мог измениться между resolve и apply); симметричный триггер
  added/removed/changed при восстановлении `missing|invalid|ambiguous →
  unique`; source-only `.placement` сам по себе НЕ порождает phantom
  `CREATE`-действие GeneratedAssets.

### 13.5 Predicate представимости TRS/shear (OPEN-V5-5)

Единый предикат для Python и C++ (реализация уже принята в v4 S6.1 и
переносится дословно): матрица `M` допустима ⟺ её host-декомпозиция в T/R/S,
собранная обратно в `M'`, совпадает с `M` поэлементно в float32 с допуском
`8 ULP` относительно `max(1, |M[i][j]|, |M'[i][j]|)`; любое нефинитное значение
— отказ. Отрицательные scale/отражения ДОПУСТИМЫ, если проходят этот тест
(`FTransform` их представляет). Вопрос «какая из эквивалентных декомпозиций
каноническая» не возникает: используется штатная декомпозиция хоста
(`matrix_world.decompose()` / `FTransform(FMatrix)`), а принимается результат
только при совпадении реконструкции. Отказ — `MH_E_UNREPRESENTABLE_TRANSFORM`
с именем объекта/узла; epsilon-починка и приближение запрещены.

### 13.6 GAZ-53 oracle (OPEN-V5-6)

Единственный вопрос, где решение зависит от owner-данных, а не от контракта.
Topology-фикстура V5-S0 принимается как есть; её option-токены помечены
synthetic и в acceptance parity не участвуют. Owner передаёт три исходных
`*.composit.blk` (root/body/random) в `reference/dagor_fixtures/gaz53/`; после
этого V5-S1 строит oracle из них, заменяет synthetic токены реальными
(`gaz53_bread_b_cmp`, `gaz53_wooden_b_cmp`, `gaz53_wooden_c_cmp` — это
composite-опции, а не mesh) и фиксирует seed-векторы.

**Закрыто.** Owner положил три файла в `reference/dagor_fixtures/gaz53/`
(`gaz53_b_body_cmp`, `gaz53_b_random_cmp`, `gaz53_body_bc_random_cmp`);
synthetic-токены в `golden/v5/gaz53/` заменены реальными, отсутствие
synthetic-остатков охраняется
`test_gaz_protocol_fixture_contains_no_synthetic_option_tokens`, а
Dagor→MH round-trip трёх документов проверен V5-S3. Ожиданий owner-данных по
v5 больше нет.

### 13.7 Filesystem aliases (OPEN-V5-7, бывший OPEN-V4-1)

Вопрос закрывается физической канонизацией: `source_root` и КАЖДЫЙ сканируемый
путь приводятся к физической форме (разрешение symlink/junction по всей цепочке)
до любой проверки принадлежности; путь, физическая форма которого лежит вне
физического `source_root`, отклоняется fail-closed. Явно настроенный alias
самого `source_root` тем самым допустим автоматически — после канонизации обе
стороны совпадают. Diagnostic/report output остаётся только под `Saved/Mimir`
с той же физической проверкой. Отдельного кода не вводится: нарушение —
`MH_E_INVALID_RESOURCE_SOURCE` с обеими формами пути в сообщении.

### 13.8 Path-derived seeds поддеревьев (поправка owner)

Единый сквозной stream заменяется потоками, выведенными из пути. Причина —
крупные итеративно правящиеся сцены: при сквозном потоке правка одного узла
сдвигает позиции всех последующих и перетасовывает несвязанные ветки
(«добавил ящик — перерандомился весь дом»). Dagor этой болезнью болеет тоже;
мы её лечим.

- `NodePath` — канонический путь §13.3 (`nodes[i]/children[j]/options[k]` c
  `>` на границе вложенного композита), считая от корневого композита
  размещения.
- `path_hash64` — первые 8 байт BLAKE3-256 от UTF-8 байтов `NodePath`,
  little-endian.
- `placement_state` — начальное состояние из int32 `Seed` ровно по §13.1.
- Начальное состояние потока узла = один шаг splitmix64 от
  `placement_state XOR path_hash64`. Дальше `next_u64/next_u32/next_unit` и
  порядок draw ВНУТРИ узла — без изменений (§13.1, §6.6).
- Каждый random-узел и каждый узел с `profile` открывает СВОЙ поток. Узлы
  взаимно независимы: правка содержимого узла или добавление узлов в другую
  ветку не меняют результат соседей.
- Следствие: даговская корреляция соседних трансформов (одинаковая выборка у
  сиблингов с одинаковыми диапазонами) отсутствует — узлы с разными путями
  всегда независимы. Это осознанный отказ копировать артефакт pass-by-value.
- Честное ограничение: путь содержит ИНДЕКС, поэтому вставка узла ПЕРЕД
  существующим сиблингом сдвигает индексы и меняет результат сдвинутых
  поддеревьев. Ключевание по `name` невозможно — оно display-only и
  необязательно (§6.1). Локальность правок это ограничение не отменяет:
  страдают только сиблинги правее вставки, а не всё дерево.
- Плата: golden-векторы V5-S1 подлежат регенерации; тег `mh.random_stream:1`
  сохраняется (алгоритм потока не менялся, изменилась его инициализация на
  узле), а изменение фиксируется версией резолвера
  `mh.random_resolver:2` в прообразе подписи §13.3.

### 13.9 Blender как внешний publisher (OPEN-V5-11)

Вопрос поставлен верно, но предпосылка §6.5 была ошибкой формулировки, а не
дырой контракта: фраза «каждый replace получает batch self-publish token»
писалась про батч ВНУТРИ UE и была ошибочно распространена на Blender-экспорт.

- Self-publish token существует и уже реализован: `FSelfPublishToken` —
  `(AbsolutePath, RawHash, Generation)`, регистрируется через
  `FMHProjectResourceIndex::RegisterSelfPublishAfterReplace`, потребляется
  однократно и **только при совпадении raw hash** отсканированного кандидата.
  Он обслуживает записи самого UE в source tree — сегодня это `Adopt`
  (`MHSourceImporter`) и `Delete resource` (`MHCompositeLevelSubsystem`).
- Механизм по построению инертен для чужого писателя: UE не регистрирует токен
  на байты, которые записал Blender, поэтому совпадения быть не может и события
  Blender не подавляются никогда. Ничего передавать между процессами не нужно —
  и не будет: **Blender токенов не выпускает.**
- Cross-process транспорт (marker-файл, lock, sentinel в Source Root) ЗАПРЕЩЁН.
  Он fail-open по своей природе: остаток после падения Blender глушил бы watcher
  бесконечно, и UE молча жил бы на устаревших ассетах. Это ровно тот класс
  тихого поведения, который протокол запрещает.
- Консистентность при наблюдении в середине батча обеспечивает publish order,
  а не подавление: порядок — зависимостный, листья первыми, значит любой префикс
  публикации замкнут (§6.5).
- Честное ограничение: watcher может переимпортировать промежуточное состояние
  и затем ещё раз финальное. Это ИЗБЫТОЧНАЯ РАБОТА, а не порча. Coalescing —
  вопрос производительности UE-стороны (окно склейки на существующем watcher),
  никогда не подавление: истечение окна может только отложить событие, но не
  отменить его. В v5-срезы это не входит.

**Следствие для V5-S4: первый replace разблокирован**, Blender не обязан
реализовывать ничего по этому пункту.

### 13.10 Textures не имеют Blender-authority и фазы публикации (OPEN-V5-12)

Отвечать нечего, потому что публиковать нечего: у текстуры нет authored
содержимого на стороне Blender. Composite, material и mesh Blender ПОРОЖДАЕТ;
текстура — это готовый файл художника, уже лежащий в Source Root.

- Формулировка «опционально textures» в §6.5 — черновой артефакт v4-редакции.
  **Отменяется.** Дельта третьей команды над второй — meshes и materials.
- Текстуры участвуют в батче ТОЛЬКО как preflight-зависимости. Каждый texture
  token каждого материала замыкания обязан разрешиться в ровно один
  существующий canonical файл тем же `resolve_texture_reference`:
  отсутствие — `MH_E_UNRESOLVED_TEXTURE_REFERENCE`, множественность —
  `MH_E_AMBIGUOUS_RESOURCE_NAME`, неканоничное имя/расширение —
  `MH_E_NONCANONICAL_RESOURCE_NAME`, вне корня — `MH_E_TEXTURE_OUTSIDE_ROOT`.
  Всё это ДО staging.
- Батч НИКОГДА не копирует внешнее изображение в Source Root. Копирование —
  это скрытое решение об identity (какое имя, какое расширение, перезаписывать
  ли), а такие решения в протоколе всегда явные. Внесение текстуры в проект
  остаётся отдельными командами `mh.copy_all_textures_to_project` /
  `mh.remap_all_textures_to_project`, выполняемыми художником до экспорта.
- Новых кодов и новой фазы publish order не вводится.

### 13.11 Исключённые зависимости остаются в замыкании (OPEN-V5-13)

Ратифицируется ровно временная реализация исполнителя; §6.5 это уже
подразумевал («отсутствующий или unmanaged dependency блокирует preflight»),
но формулировка «mesh/material payloads не добавляет» допускала прочтение
«mesh/material вообще вне замыкания», поэтому фиксируется явно.

- `Export Composite` и `Export Composite + Composite Closure` строят ПОЛНОЕ
  source closure, включая mesh, material и texture зависимости. Не публиковать
  их — не значит не проверять.
- Каждая такая зависимость обязана разрешаться в существующий canonical managed
  source. Иначе — `MH_E_RESOURCE_NOT_FOUND` (уже зарегистрированный код,
  «no source payload for <key>»), unmanaged — `MH_E_INVALID_RESOURCE_SOURCE`,
  неоднозначность — `MH_E_AMBIGUOUS_RESOURCE_NAME`. Блокируется весь батч, до
  staging, без частично записанных файлов.
- Диагностика обязана называть конкретный ResourceKey, ссылающийся на него
  композит и команду, которая закрыла бы дыру
  (`Export Composite Include All Stuff`). Автоматического повышения команды до
  Include-All нет: выбор объёма публикации остаётся за художником.
- Причина запрета мягкого режима: композит с висячей зависимостью — это
  заведомо сломанное дерево источников. UE на нём всё равно fail-closed, только
  на импорте, дальше от места ошибки и после записи файлов.

### 13.12 Thumbnail не является потребителем resolved plan (OPEN-V5-14)

Вопрос поднят верно и вскрыл мою ошибку: «thumbnail» попал в списки
потребителей plan и в acceptance V5-S5, хотя custom renderer был отключён по
отдельному owner-запросу после shutdown crash. Owner подтвердил: **thumbnails
сейчас не нужны.**

- `MHCompositeThumbnailRenderer` остаётся отсутствующим, регистрация не
  возвращается, Automation-тест `Mimir.V5.Composite.ThumbnailRenderingDisabled`
  не меняется. Content Browser рисует стандартный ассетный thumbnail.
- Слово `thumbnail` убрано из §6.5, §6.6 и из acceptance V5-S5 и сводного
  end-to-end. Принцип «все потребители КОНКРЕТНОГО результата получают один
  `FMHResolvedCompositePlan`» не ослаблен: thumbnail из него ИСКЛЮЧЁН, а не
  освобождён от него.
- Причина исключения важнее самого отключения и фиксируется, чтобы вопрос не
  вернулся в неправильной форме: **у композит-ассета нет канонического
  внешнего вида.** Резолюция — свойство РАЗМЕЩЕНИЯ (actor + Seed), а не
  ассета; один ассет законно имеет много размещений с разными seed, trace и
  signature. Поэтому asset-thumbnail не «трудно засидить» — он некорректно
  поставлен как потребитель plan, и никакой неявный seed 0, авто-seed или
  выбор по текущему выделению эту некорректность не лечит, а прячет.
- Если thumbnail когда-нибудь понадобится, допустима ровно одна форма:
  ОТОБРАЖАТЕЛЬНАЯ конвенция над явно документированным фиксированным seed,
  объявленная не протокольным артефактом. Она НИКОГДА не входит в
  `ResolvedSignature`, closure hash, applied state или cook, и acceptance
  никогда не сравнивает её «на parity» с размещением. Вводится это отдельным
  срезом и отдельным owner-решением, не внутри V5-S5.

**Следствие для V5-S5: thumbnail-часть снята со среза целиком**, а не
отложена внутри него. Остальной scope V5-S5 вопросом не затронут.

### 13.13 Прямой dag4blend-адаптер (owner, документ 13 R2)

Документ [13 R2](13_v5_s6_1_dag4blend_bridge.md) заменяет обязательную
материализацию маршрута сценовой конвертации из §6.4 прямым read-only
экспортом. Строгий BLK-reader не меняется; две формы сцены Blender дают один
Composite DTO и используют один canonical writer / source-closure publisher.
Диспетчер отвергает частичную MH identity и смешанную MH/dag4blend authority.

Прямой экспорт не создаёт MH-двойников, не меняет `instance_collection`,
parent, имена, custom properties, активную сцену и выделение, не ставит
`mh_resource_*` на даговские определения. Публикация заканчивается файловой
квитанцией; материализатор и Blender-finalizer не вызываются даже при
частичном сбое. Optional Convert остаётся отдельной явной командой;
перепривязка в ней разрешена только отдельным opt-in.

Три export-команды §6.5 принимают обе формы сцены. Исключённые зависимости
проверяются против Source Root; неизменённые опубликованные payload'ы
переиспользуются по содержимому источника, не по меткам сцены. Все options,
включая zero-weight, входят в closure; недостижимые датаблоки — нет.

Сценовый адаптер имеет **частичную совместимость**, не lossless-конверсию
исходного BLK. Потерянные dag4blend корневые controls не восстанавливаются;
их отсутствие не означает известный ноль. Отчёт отдельно перечисляет
сохранённое, невосстановимое и заблокированное. Inline p2 без typed profile
по-прежнему блокируется с NodePath и именами параметров (OPEN-V5-15).
Prefab по умолчанию блокируется; явный Allow Prefab as Mesh (Lossy) требует
warning. Наблюдаемые label/require/colors опускаются с
`MH_W_DAGOR_CONSTRUCT_DROPPED`, а не исполняются.

`gameObj` сценового адаптера кодируется только как `marker` (§6.1).
`placement` — provenance-only объект с обязательным закрытым `mode`, без
`trace_start_above_cm`; `appearance_seed_boundary` — bool-носитель с omission
false, без потребителя до S6.3. Прижатия в UE и обратной записи в Dagor
не будет. Старые положения документа 12 о снапе, провайдере, warnings на
каждое размещение и gameObj registry отменены owner'ом, не являются scope.
