# 09 — Срезы реализации Source Protocol v4 для внешнего агента

Аудитория: **исполнитель без контекста сессии**. Перед любым срезом прочитай
`docs/08_source_protocol_v4_plan.md` (единственный активный норматив) и этот
файл целиком. История решений — в superseded-доках, читать их нужно только
чтобы ставить баннеры (срез S0) и удалять код (S1).

## Инварианты для всех срезов

1. **08 — единственная истина.** Конфликт кода/старых доков с 08 решается в
   пользу 08. Новые нормативные решения в срезах не изобретаются: чего нет в
   08 — вопрос owner'у, не импровизация.
2. Fail-closed: неоднозначность/невалидность блокирует ресурс и dependents с
   машинным `MH_E_*`; предупреждения `MH_W_*` не блокируют. Все новые коды
   регистрируются в `addon/mh4blend/core/canonical.py::ERROR_CODES` и в
   golden-списке `tests/test_canonical.py` (обнови счётчики E/W).
3. Атомарные записи: sibling tmp → (read-back где указано) → `os.replace`.
   Writer никогда не делает diff; сравнивает только reader (UE).
4. Никаких UUID нигде. Никакого dual-read старых форматов.
5. Python: существующий стиль addon/ (bpy-free core, `_bpy`-тесты гейтятся
   `pytest.importorskip("bpy")`). UE: editor-only модули, engine не форкается,
   стиль существующего `ue/MimirComposite`.
6. Каждый срез: код + тесты + правка затронутых доков + короткая квитанция в
   `docs/receipts/<slice>.md` (что сделано, что прогнано, что осталось).
7. Не трогай `reference/`, `golden/` вне явных указаний среза.

---

## S0 — Documentation overhaul (этап обновления всей документации)

Цель: вся документация репозитория соответствует 08.

Действия:
1. В каждый superseded-документ (список — 08 §12: 00–07, ADR_V2, ADR_V3,
   AMENDMENT_combined_lod, AMENDMENT_node_hierarchy, RISK_RESULTS, ROADMAP,
   C0/C1 отчёты не трогать — они исторические квитанции) добавить шапку-баннер:
   статус superseded/частично superseded, ссылка на 08, перечень выживших
   разделов (для 07 и обоих AMENDMENT — по таблице 08 §12).
2. `QUESTIONS.md`: все UID/passport/round-trip вопросы пометить
   `SUPERSEDED BY 08`; оставить открытыми только не-UID (например
   filesystem aliases). Добавить новый открытый вопрос, если найдёшь реальную
   дыру 08 — с fail-closed временным правилом.
3. `README.md` и `KICKOFF_PROMPT.md`: переписать краткое описание проекта под
   формулу 08 §1.
4. Ничего не удалять: история сохраняется, нормативность снимается баннерами.

Acceptance: grep по докам не находит нормативных утверждений про UID/паспорт/
UE→FBX без баннера superseded; README отражает v4.

## S1 — Purge UID/passport из production-кода

Цель: кодовая база не содержит UID-механики и passport-транспорта
(миграция не нужна — 08 §11).

Blender (`addon/mh4blend`):
1. `scene/export_fbx.py`: удалить ensure_uid/PROP_UID/repair
   (`_repair_duplicate_node_uids`), passport стамping
   (`_temporary_passport_properties`, `read_fbx_passport` consensus,
   `_assert_existing_target_uid` заменить проверкой «target существует и не
   каталог»), `_temporary_lod_level_properties` (свойства больше не пишутся).
   Вместо lod-свойств — временный rename export context: каждый mesh-объект
   `.lodNN`-коллекции получает суффикс `_lodNN` в имени узла, если его нет
   (08 §4); восстановление имён в `finally`.
   СОХРАНИТЬ: сбор groups/aux/geometry, транспорт пустышек, замыкание по
   родителям `MH_E_PARENT_OUTSIDE_RESOURCE`, атомарную запись, centimeter
   export context, `_dagor_lod_structure` (валидация .lods).
2. Удалить `core/uid.py`, `core/fbx_passport.py`, `core/meshser.py`,
   `scene/mesh_extract.py` hash-вызовы; вычистить импорты. Экспорт больше не
   вычисляет никаких хэшей.
3. Материалы/композиты: удалить uid-поля из моделей/кодеков (полная замена
   форматов — S2/S3; здесь только снять UID-зависимости, чтобы пакет
   собирался и тесты шли).
4. Тесты: удалить/переписать uid/passport/meshser-тесты; сохранить и починить
   гейты иерархии (parent-закрытие, транспорт null-узлов — из
   `tests/test_export_fbx_bpy.py`), убрав uid-ассерты. Коды
   `MH_W_NODE_UID_REASSIGNED` и все `MH_E_DUPLICATE_*UID*` удалить из
   реестра и golden-списка.

UE (`ue/MimirComposite`): удалить passport-ридер/валидацию, UID-resolver
ветки; `IMHSourceResolver` переводится на `FMHResourceKey {Kind, LogicalName}`
(канонический алфавит и правило составного расширения — 08 §2); Ledger-код
оставить компилируемым, но пометить deprecated (замена — S4).

Acceptance: grep `mh_uid|passport|resource_uid|ensure_uid` по production-коду
пуст (кроме исторических доков); полный python-suite зелёный; guarded UE build
зелёный; экспорт коллекции пишет валидный FBX без MH custom properties, с
иерархией и суффиксами `_lodNN`.

## S2 — Material format v4 + registries + Publish

По 08 §5. Blender: writer/reader лаконичного JSON (`class|library`,
`textures`, `params`, БЕЗ schema/version/mode/static bools); экспорт
обновляет все затронутые `.material` при включённой опции. UE: parent
registry `<master_root>/<class>` и library registry `<library_root>/<name>`
(без префиксов), MI-импортёр in-place, `UMHMaterialSourceData`,
`Publish Material` = extract → canonical JSON → tmp → read-back → atomic
replace (`ничего не сравнивая`), Adopt-диалог (папка+имя) для MI без source.
Импорт: source побеждает; `MH_W_MANAGED_ASSET_LOCALLY_MODIFIED` при локальной
правке. Незарегистрированный class/несериализуемый параметр —
`MH_E_MATERIAL_NOT_ROUNDTRIPPABLE` на publish.

Acceptance: `.material → MI → правка → Publish → повторный импорт` даёт
NO_CHANGE; golden-векторы канонического JSON — ОБЩИЕ файлы-фикстуры для
pytest и UE Automation (байт-идентичность Blender- и UE-writer'ов, 08 §5);
закрытая грамматика с `MH_E_MATERIAL_GRAMMAR`; overrides у library-parent
на Publish дают `MH_E_MATERIAL_NOT_ROUNDTRIPPABLE` (решения
OPEN-V4-6/7/8); текстуры резолвятся как
extensionless logical names по ResourceKey `texture:<name>` сканом
source-дерева (08 §5, решение OPEN-V4-2; Project Index как кэш появляется
в S4); коды `MH_E_NONCANONICAL_TEXTURE_REFERENCE` и
`MH_E_UNRESOLVED_TEXTURE_REFERENCE` зарегистрированы в реестре и
golden-списке.

## S3 — Composite format v4 + Blender mesh-импорт

Ядро среза — общий модуль «импорт mesh-ресурса» по 08 §4.1 (стадии 0–4:
parse/preflight собственным ридером, материализация зависимостей через
reader S2, штатный импортёр пиноваными настройками в staging, резолв
слотов по индексам из парса, реструктуризация `.lods`/aux, полный откат
дельты при сбое) и оператор `Import Mesh FBX`. Композитный
Blender-импортёр строит размещения ПОВЕРХ этого модуля. Новые коды:
`MH_E_IMPORT_TARGET_OCCUPIED`, `MH_E_UNRESOLVED_MATERIAL_REFERENCE`
(общий с S5).

Композиты — по 08 §6. Кодек JSON-дерева (kinds mesh/actor/composite/group,
transform translation_cm/rotation_quat/scale, `name` опционален, БЕЗ
материалов и uid); Blender writer/importer; UE `UMHCompositeAsset` +
компилятор в components/actor; `ActorClassRegistry` в настройках (name →
SoftClassPath), unresolved actor/mesh/composite — блок ресурса; запрет
самовключения и включения предков; Publish Composite — семантика S2
(полная перезапись, read-back); импорт — source побеждает + warning.

Попутный cleanup из ревью S2: удалить мёртвые коды
`MH_W_TEXTURE_BASENAME_AMBIGUOUS`, `MH_W_TEXTURE_NOT_FOUND`,
`MH_W_TEXTURE_OUTSIDE_ROOT`, `MH_W_UNKNOWN_SHADER_CLASS`,
`MH_W_MATERIAL_NOT_FOUND` (использований
нет ни в Python, ни в UE; семантика отменена v4) с обновлением golden
counts; перевести
`MH_E_*`-raises в `_dagor_lod_structure` на `MHValidationError`.

Коды S3 (решения OPEN-V4-10/11): `MH_E_COMPOSITE_GRAMMAR`,
`MH_E_COMPOSITE_CYCLE`, `MH_E_UNRESOLVED_COMPOSITE_REFERENCE`,
`MH_E_INVALID_NODE_MARKERS`. Legacy `MH_W_COMPOSITE_CYCLE`,
`MH_W_UNRESOLVED_RESOURCE` и `MH_E_INVALID_COMPOSITE` удаляются, если
после реализации не остаётся call sites (обновить golden counts).
`core/transforms.py` переписывается под transform contract 08 §6.

Acceptance: пример из 08 §6 круговой (`.composite → UE → publish →
реимпорт` эквивалентен); composite golden-векторы канонических байтов —
общие файлы для pytest и UE Automation; parity-гейт transform: одно и то
же размещение через FBX-узел и через composite даёт совпадающий мировой
трансформ в UE (допуск float32); цикл/предок — fail-closed; композит НЕ
содержит и НЕ принимает информации о материалах. Mesh-импорт: `export → Import Mesh
FBX → export` даёт FBX с той же классификацией узлов и теми же слотами
(байт-идентичность FBX не требуется); импорт в сцену с уже существующими
материалами не создаёт ни одного дубликата `*.001` (bpy-гейт); отказ на
любом preflight-предусловии оставляет дельту датаблоков пустой; non-goals
08 §4.1 не тестируются.

## S3.1 — Unresolved placement в авторинге (микро-срез)

По поправке 08 §6: Blender composite import при отсутствующем
mesh/composite ресурсе узла строит красный placeholder-Empty вместо
блока, выдаёт `MH_W_UNRESOLVED_PLACEMENT` (зарегистрировать в реестре и
golden counts), `resource`-token сохраняется lossless, Export пишет узел
без изменений. Ambiguous same-kind duplicates и UE-сторона не меняются;
mesh-импорт §4.1 не меняется.

Acceptance (bpy): импорт композита с отсутствующим ресурсом → placeholder
+ warning, последующий экспорт даёт байт-идентичный узел; после появления
ресурса свежий импорт в чистую сцену даёт полноценный инстанс.

## S4 — Project Resource Index

По 08 §3. SQLite в `Saved/MimirBridge/ProjectIndex.sqlite`; писатель —
только UE. Full scan + incremental upsert (watcher/startup/publish);
duplicate policy (`MH_W_DUPLICATE_RESOURCE_NAME` скан /
`MH_E_AMBIGUOUS_RESOURCE_NAME` resolve, блок dependents);
`MH_W_PROBABLE_RESOURCE_RENAME` (совпадающий raw hash);
`MH_W_ORPHAN_REBOUND_CONTENT_DIVERGED`; self-publish token →
`SELF_PUBLISHED`; Dependencies из payload-ссылок; GeneratedAssets из Asset
Registry tags 08 §7. Deprecated Ledger и его commandlet-флаги
(`-writeledger`) удаляются вместе с заменой на индекс.

Решения OPEN-V4-12…16 (нормативно в 08 §§2, 3, 7): state machine и
rebuild-контракт индекса; шестой tag `MH.SourceHash` и
`UMHTextureSourceData` (S3-тест «exact five tags» обновить на шесть);
Dependencies только ResourceKey→ResourceKey с ролями
`texture|placement_mesh|placement_composite` (slot-рёбра — S5) и
provenance по кандидату; S4 без watcher — full scan + batched
`UpsertPaths` + Publish-интеграция, self-publish token in-memory;
probable-rename — биективная пара по raw_hash внутри generation; orphan
rebind — обычный импорт в generated path, дивергенция — бинарное
неравенство raw hash. Новые коды S4: `MH_W_PROBABLE_RESOURCE_RENAME`,
`MH_W_ORPHAN_REBOUND_CONTENT_DIVERGED`, `MH_E_AMBIGUOUS_GENERATED_ASSET`
(+ golden counts).

Acceptance: удаление .sqlite → рестарт → идентичный индекс; 10k синтетических
ресурсов — resolve по индексу без полного скана на ссылку; move-без-rename не
трогает UE asset; rename даёт orphan+новый ресурс.

## S5 — UE StaticMesh importer v4

По 08 §4, §7, §8. Direct FBX SDK → классификатор узлов по таблице 08 §4
(суффиксы/префиксы имён; global evaluated transforms; null-группы —
структура) → `FMHSceneIR` → полный in-place rebuild UStaticMesh: все LOD
(dense, из `_lodNN`), BodySetup (режимы по `_cls_*`/UCX), сокеты, слоты.
Привязка материалов ДО сборки: slot name == material logical name → индекс →
MI; отсутствие — `MH_E_UNRESOLVED_MATERIAL_REFERENCE` (введён в S3), блок
ресурса. `UMHStaticMeshImportData` receipt + Asset
Registry tags; commit applied state только после сохранения package. Diff:
raw hash равен → NO_CHANGE, иначе полный reimport. Путь ассета:
`/Game/MH/Generated/Meshes/<name>` (без префикса).

Acceptance: изменение любого FBX-домена (гео/LOD/коллизия/сокет) обновляет
ТОТ ЖЕ UObject целиком; правка в Static Mesh Editor детектится
(`MH_W_MANAGED_STATIC_MESH_LOCALLY_MODIFIED`) и перетирается следующим
импортом; FBX без каких-либо MH-метаданных импортируется полностью.
Mapper-facing FBX dump возвращается в этом срезе commandlet'ом поверх
`FMHSceneIR` с новым тегом `mh.fbxdump:4` (решение OPEN-V4-5); объём
задаётся потребностями S5 acceptance, старый v2-формат не наследуется.

## S6 — Watcher, startup, commandlets, UX

Startup scan (после Asset Registry), DirectoryWatcher c debounce ≥1s и
PIE-очередью, порядок импорта textures→materials→meshes→composites.
Команды UI: Scan Project / Import Changed / Publish Material / Publish
Composite / Show Duplicates / Find Broken References / Find Orphaned Assets.
Commandlets: `-run=MHScanSources`, `MHImportSources`, `MHValidateNames`,
`MHVerifyMaterials`, `MHVerifyComposites` (headless, nonzero exit на E).
Self-publish loop-тест (08 §3) обязателен.

Acceptance: сквозной сценарий на реальном source root: Blender export →
авто-импорт → правка `.material` → MI обновлён без rebuild меша → Publish
Composite → нет петли → отчёт в Message Log и JSON под Saved/ с новым
тегом `mh.analyze_sources:4` (решение OPEN-V4-5; v2-теги `:1` не
переиспользуются никогда).

## S7 — mh_asset_io: нативный FBX-кодек (строго после S6)

По `ADR_V4_mh_asset_io.md`. Нативный Python-модуль (nanobind/pybind11;
`ufbx`/`ufbx_write` vendored на зафиксированных коммитах, без network
fetch на сборке); ядро Blender-agnostic, обмен с bpy — только bulk-буферы
(`foreach_get`/`foreach_set`), без пер-вершинных Python-циклов.

Фаза A (импорт): декодер на `ufbx` заменяет ТОЛЬКО стадию 2 из 08 §4.1 —
стадии 0/1/3/4, оператор, предусловия и откат дельты не меняются;
плейсхолдерные материалы перестают возникать (слоты назначаются до
создания мешей). Фаза B (экспорт): НЕ начинается без отдельного
owner-решения о зрелости `ufbx_write`; обязательный validator
`write → чтение через ufbx → сравнение моделей` (отказ validator'а =
файл не публикуется); текущий Python-экспортёр — fallback до полного
паритета. Custom properties и расширение FBX-диалекта запрещены (08 §4;
ADR «Отвергнуто»).

Acceptance: side-by-side паритет фазы A с §4.1-импортом на golden-наборе
(классификация узлов, слоты, трансформы; геометрия в заданном допуске),
bpy-гейты обеих веток, packaging wheel в Blender Extension; фаза B — FBX
encoder'а читается UE-импортёром S5 и §4.1-импортом идентично выходу
текущего экспортёра, validator обязателен, fallback сохраняется.

---

## Сводные acceptance-тесты v4 (заменяют §21 черновика v3)

1. Move без rename → та же identity, UE asset на месте.
2. Rename → orphan + новый ресурс; probable-rename warning при том же hash.
3. Duplicate same-kind name → скан-warning, resolve-error, dependents
   заблокированы, остальные импортируются.
4. Cross-kind same stem (`garage.mesh.fbx`/`.material`/`.composite`) →
   три ресурса, все резолвятся.
5. Удаление ProjectIndex.sqlite → идентичное восстановление.
6. Любое изменение FBX → полный in-place rebuild того же UStaticMesh.
7. `.material`-only изменение → MI in-place, mesh не пересобирается.
8. Publish Material/Composite → полная перезапись source, read-back,
   NO_CHANGE на повторном импорте, без watcher-петли.
9. Локальная правка managed-ассета → warning, source восстанавливает.
10. Прерванная запись (crash до replace) → старый payload цел.
11. FBX c пустышкой-группой → иерархия в UE учтена (world transforms),
    parent вне ресурса → `MH_E_PARENT_OUTSIDE_RESOURCE`.
12. Blender-дубликат меша (`Shift+D`) → экспорт успешен без каких-либо
    UID-предупреждений (UID больше не существует).
