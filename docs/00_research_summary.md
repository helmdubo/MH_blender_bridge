# 00 — Research Summary & Decision Log

> Этот документ — сжатый результат research-сессии. Он объясняет **почему** система устроена так, а не иначе.
> Исполнитель обязан прочитать его перед началом работы. Спорить с решениями можно, но только явно:
> предложение фиксируется как вопрос в PR/issue, решения из Decision Log не переопределяются молча.

## 1. Что мы строим

DCC-driven composite asset pipeline: Blender → versioned source files → UE5.
Философия взята из DagorEngine (`.composit.blk` + `.dag` + daEditor composite entities),
слабые места Dagor (name-based identity, filesystem resolver, отсутствие diff) — исправлены.

Центральная формула (зафиксирована ревью):

```
Composite = semantic definition graph        (граф узлов, uasset + текстовый файл)
Placement = definition reference + seed      (актёр в уровне)
Compiler  = definition + placement → native UE representation
Identity  = адрес (UID), а не имя и не порядок
```

## 2. Контекст: Dagor / dag4blend (изучен исходный код 2.12.0)

Авторинг-модель dag4blend, которую мы **сохраняем** (она уже используется в студии):
- Сцена `GEOMETRY`: одна Collection = один mesh-ресурс.
- Сцена `COMPOSITS`: Collection (`type='composit'`, плоская) = composite definition;
  Empty + `instance_collection` = placement; иерархия composite = object parenting Empty→Empty.
- `random.NNN` collection + безымянные Empty с `weight:r` = variant set (`ent{}` в blk).
- `nodes_split / node_rebuild / node_revert` = explode/rebuild с сохранением ссылки
  (при split `instance_type='NONE'`, но `instance_collection` остаётся — это и позволяет revert).

Ключевой урок: удобство Dagor создаётся **разделением Resource Definitions и Placement Graph**,
а не форматами файлов. Composite "растворяется" только при билде уровня; в редакторе это живая
сущность с уже разрешённым рандомом и сидом на размещение.

Что в dag4blend плохо и НЕ копируется: identity по имени (им пришлось делать `col['name']`
override против блендеровских `.001`), filesystem-cache (`cache_rw.py`, pickle, скан диска),
семантика через имена/regex (`.lods`, `random.`, `name:type`), глобалы импорта без cycle detection,
ручная axis-математика с багом (`offset_y` дважды, `offset_z` никогда).

Что берём как паттерны кода: скелет settings/prefs, `log()`/popup, `fix_type` (типизированные
props), обход Empty-графа из `cmp_export::write_node`, принцип `broken_properties`
(неизвестные ключи транспортируются, не теряются), UX explode/rebuild.

## 3. Decision Log (обязателен к соблюдению)

> **Правило идентификаторов решений:** номер `Dnn` присваивается и ведётся только
> этим репозиторием. Внешние директивы и аудиторские addendum ссылаются на решения
> по стабильному slug в квадратных скобках; их локальная нумерация не резервирует
> номер в этом журнале. При переносе директивы в Decision Log репозиторий назначает
> следующий свободный номер, не меняя slug и смысл решения.

> **Актуализация UX/источников:** D32–D38 ниже заменяют D2, D15, D17,
> D21–D23, D27 и bundle-части D31 там, где они противоречат отдельным
> FBX/Composite операциям.
> Исторический текст оставлен для аудита решений, но не является заданием на
> реализацию. Нормативный workflow — `04_source_workflows.md`.

**D1. [fbx-geometry-payload] FBX — только геометрический payload одного ресурса.**
Финальная формулировка контракта: **composite nodes никогда не извлекаются из FBX;
`.composite` задаёт граф, resolver находит payload по UID, FBX только наполняет
target Collection.** Никакой семантики, иерархии, placements или материальных
определений внутри FBX. Один FBX детерминированно создаёт один UStaticMesh.
Причина: legacy scene import и Interchange по-разному мангли имена/иерархию;
вывод composite semantics из FBX-иерархии хрупок; Blender FBX exporter
разворачивает collection instances.

**D2. Bundle = каталог, не архив.** `*.composite` (JSON, один файл = один composite),
`meshes/*.mesh.fbx` (один на mesh-ресурс), скрытый `export_manifest.json` — commit-marker,
пишется последним, атомарная замена. Пользователь видит две сущности: composite-файл и
геометрию — как `.composit.blk` и `.dag` в Dagor. Manifest = аналог невидимого `.folder.blk`/кеша.

**D3. Пять уровней identity:** ResourceUID (mesh datablock / material / actor class),
CompositeDefinitionUID, NodeUID (узел в definition), PlacementUID (актёр в уровне),
OccurrenceUID = Hash(PlacementUID, NodePathUID) — конкретное воплощение узла
(NodePathUID = хеш пути NodeUID'ов от корня, т.к. один child может входить дважды).
Имя — только display/package name/fallback. Rename ≠ Delete+Create. Linked duplicate =
общий ResourceUID; Make Single User = новый ResourceUID.
Исторический BundleUID удалён из текущей модели решением D33.

**D4. Keyed random, НЕ последовательный RNG** (принято по внешнему ревью, важно):
```
RandomValue = Hash(RngSchemaVersion, PlacementUID, OccurrenceUID, RandomChannelID, UserSeed)
```
`RandomChannelID` — закрытый реестр стабильных имён (variant_selection, offset_x, offset_y,
offset_z, rotation_yaw, rotation_pitch, scale_uniform, ...). Никакого FRandomStream с next():
добавление нового random-параметра не должно менять другие значения. Hash — стабильный
кроссверсионный (xxHash/CityHash по конкатенации), НЕ HashCombine движка.
`seed_policy: inherit | independent (+seed_salt)` — явное поле схемы (аналог ignoreParentInstSeed).
Reroll меняет только UserSeed placement'а. В MVP рандом не реализуется, но схема обязана его нести.

**D5. Composite Compiler в редакторе, результат хранится в уровне.** Как daEditor: разворот,
резолв вариантов и рандома происходит при размещении/пересборке в редакторе, НЕ при загрузке и
НЕ при куке. Актёр хранит Seed + CompiledFingerprint (hash definition). Пересборка с тем же сидом
= байт-в-байт тот же результат. При куке World Partition сплющивает содержимое штатно.

**D6. Компилируем в родные контейнеры UE, не строим свой runtime.** `UMHCompositeAsset` —
редакторский source of truth; таргеты компиляции: StaticMeshComponent (MVP),
позже ISM/HISM (по instance grouping key: mesh UID + material set + render state, НЕ по имени),
Level Instance (только для детерминированных поддеревьев без рандома в цепочке — LI требует
идентичного содержимого у всех размещений), Packed Level Actor (оптимизационный таргет).
VariantSet-узлы в LI некомпилируемы принципиально.

**D7. Compiled representation никогда не пишется обратно в source-граф.**

**D8. Никаких per-node override'ов в размещениях.** Решение владельца проекта: потроха
размещённого композита строго read-only (`потроха = f(definition, seed)`), клик проваливается
на актёра-владельца, попытка правки → откат + throttled notification. Легитимные пути изменения:
правка definition в Blender, Break, reroll seed, pin варианта (pin — допустим, это выбор,
а не деформация). Undo/Redo работает над входами компиляции (seed, transform актёра), не над выходом.

**D9. BP/Actor-узлы (аналог gameobj):** узел `kind=actor`, в Blender — Empty-заглушка
(куб, как gameobj в dag4blend), ссылка через `actor_resource_uid` + кешированный
`cached_soft_class_path` (redirectors UE не чинят строки в JSON — резолв через registry).
Компилятор спавнит **отдельного актёра** (не ChildActorComponent — грабли с физикой).
Ownership — ДАННЫЕ (`GeneratedByPlacementUID`, `OccurrenceUID` на спавненном актёре),
attachment — опционален и только где семантически корректен; перемещение композита выполняет
контроллер по ownership-индексу. Изменение BP не проходит через наш pipeline — штатный механизм UE.
Post-MVP.

**D10. [composite-cycle-policy] Циклы composite-ссылок диагностируются на всех
рубежах, а severity зависит от операции.** Blender Import: back-edge превращается
в unresolved-заглушку, остальной граф импортируется, код
`MH_W_COMPOSITE_CYCLE`. Blender Export и UE Import: операция блокируется, код
`MH_E_COMPOSITE_CYCLE`. Компилятор сохраняет защитный стек `CompositeUID` и
лимит глубины, чтобы повреждённые внешние данные никогда не приводили к зависанию
или крэшу. Нормативный инвариант реестра диагностик: **любой `MH_E_*` блокирует
текущую операцию, любой `MH_W_*` её не блокирует**. Суффикс кода называет
обнаруженный факт, а реакция импортёра описывается текстом диагностики. Ромбы
(переиспользование) легальны: различать «в стеке» и «уже развёрнут по другому пути».

**D11. UE-ассеты:** `UMHCompositeAsset` (импортируется из `.composite` через
UFactory+FReimportHandler, read-only generic editor, VisibleAnywhere, SourceJsonSnapshot внутри),
project Ledger/registry (ResourceUID → SoftObjectPath + hashes; конкретное имя
UE-типа не фиксируется source schema),
`AMHCompositeActor` (+ UActorFactory для drag&drop; PostSpawnActor → новый PlacementSeed;
PostDuplicate — политика сида настраиваемая). Ссылки между композитами — FSoftObjectPath,
заполняются при импорте из UID → Reference Viewer работает бесплатно. Auto-reimport через
directory watcher по manifest-файлам. Asset Registry tags:
ResourceUID/CompositeUID и node count. Исторические `UMHSourceBundle` и
bundle-tag superseded D33/D36.

**D12. Транспорт трансформов:** аддон конвертирует в UE-конвенцию (см, Z-up, кватернион)
на экспорте; на стороне UE нет матричной математики конверсии вообще. Геометрия идёт через
Blender FBX exporter + UE import с зафиксированными настройками. Первый go/no-go тест проекта:
асимметричный меш + несимметричный placement, мировая позиция контрольной вершины
Blender == UE.

**D13. LOD:** Blender FBX exporter не пишет FbxLODGroup → LOD-ы либо отдельными файлами
per-level с добавлением в существующий SM, либо Nanite/generated. Зафиксировать выбор в схеме
до реализации. В MVP — один LOD.

**D14. Break / Build New Composite (post-MVP):** Break растворяет один уровень, дети получают
производные сиды (визуально нейтрально), VariantSet выходит уже разрешённым вариантом
(с предупреждением о потере рулетки). Build New Composite ПИШЕТ `.composite`-файл на диск и
импортирует его штатной фабрикой (не создаёт uasset напрямую) — все композиты гомогенны,
круг Blender↔UE замыкаем.

**D15. Материалы в MVP:** существующая metadata-схема студии + порт текущих post-import
скриптов как шаг Finalize. Материал — ресурс с UID в dependency graph; записи хранятся
в `export_manifest.json`, отдельного `materials.json` нет (окончательно уточнено D22/D30).
**Статус: историческое решение; способ хранения material payload superseded D37
[material-source-files].** Положение о материале как UID-ресурсе сохраняется.

**D16. Миграция существующего контента:** операция Adopt Existing (привязка существующего
uasset к UID по имени/пути с подтверждением) — обязательна до раскатки на реальный проект,
в MVP не входит.

**D17. Registry.json (UE → Blender):** обратный реестр (uid → kind, name, path, owner,
для классов — DisplayName/PreviewBounds/Category/policies). В MVP опционален/пуст,
нужен для внешних ссылок, forks, BP-дропдауна.
**Статус: superseded D36 [uid-source-resolution] в части source-resolve:** registry
является только приоритетным hint и не заменяет подтверждение owning manifest.

**D18. Флаги диффа `UPDATE_RESOURCE` и `UPDATE_KIND`.** Ретаргет узла на другой
ресурс (Make Single User) и смена вида узла (mesh → composite_ref) при живом
node_uid — самостоятельные операции диффа, НЕ REMOVE+CREATE: живой UID = та же
сущность. Обнаружено при написании expected_diffs этапа A (без этих флагов
мутация make_single_user невыразима).

**D19. Публичный resource_uid mesh-ресурса = `collection['mh_uid']`.** Единица
экспорта — GEOMETRY-коллекция (одна коллекция = один FBX = один UStaticMesh);
именно её UID стоит в манифесте и в `resource_uid` узлов. `mesh['mh_uid']`
(datablock) и `obj['mh_uid']` — внутренняя identity: порядок сериализации
multi-object коллекций и арбитраж Make Single User. В манифест v1 не попадают.

**D20. ASCII-идентификаторы, NFC для остальных строк.** Имена ресурсов,
композитов и bundle — ASCII `[A-Za-z0-9_ -]` (валидация
`MH_E_NON_ASCII_RESOURCE_NAME`, без транслитерации: имена становятся именами
UE-пакетов). Все прочие строки (display_name, значения properties) — юникод,
нормализуемый в NFC на входе канон-формы и при записи файлов: NFD-написание
из macOS-пайплайна не должно давать фантомный дифф.

**D21. Исходники проекта = экспортированные файлы** (`.mesh.fbx`, `.composite`,
текстуры, служебный манифест). `.blend` — личный инструмент художника, вне VCS.
Следствия: Blender-импортер `.composite` — обязательная часть системы (блок 6
этапа B, а не post-MVP); ownership решается через VCS — правило «один bundle =
один .blend» отменено; манифест = квитанция транзакции экспорта. Операция
Export Selection (dependency closure композита для передачи третьей стороне,
флаг with-textures) — post-MVP.
**Статус: основной принцип сохраняется; bundle-терминология, границы поиска и
операции экспорта superseded D32, D33 и D36.**

**D22. Материал — полноценный ресурс с UID уже в MVP.** Хранение — секцией
`materials` в манифесте (отдельного materials.json НЕТ — решение владельца).
Связь мешей с материалами — таблица `material_slots` у mesh-ресурса
(`slot_name` → `material_uid`), НЕ парсинг имён из FBX.
**Статус: хранение секцией `materials[]` superseded D37
[material-source-files].** Материал остаётся полноценным UID-ресурсом, а
`material_slots` остаётся единственным контрактом связи mesh → material.

**D23. Текстуры — третий вид исходников.** Идентичность = относительный путь
под `texture_root` (настройка аддона), БЕЗ UID — явное исключение из D3
(носителя для UID нет; rename текстуры = REMOVE+CREATE с warning'ом).
Общая текстурная библиотека (как в Dagor); bundle текстуры не копирует.
**Статус: superseded D34 [texture-path-canonicalization].**

**D24. Средняя стадия (Analyzer / Ledger / diff) — всегда наша**, работает по
нашим файлам и в Interchange не переезжает. NodeContainer-pipeline — целевая
реализация геометрического backend'а (`FInterchangeGeometryBackend`), post-MVP,
принимается по golden-сравнению меш-в-меш с legacy. Хеши считаются экспортером
(источником), никогда — по результату трансляции.

**D25. Материалы при импорте — трёхстороннее сравнение:** base (applied-хеш в
Ledger) / theirs (манифест) / ours (текущий MI в проекте). Исходы: обычный
UPDATE; `LOCAL_EDIT` (MI правили руками в UE); `CONFLICT` (разошлись оба) —
интерактивный выбор keep/overwrite в Preview; в headless — политика проекта
(default: overwrite, source of truth — Blender).

**D26. Три слоя синхронизации, один импортер:** watcher (оба инструмента
открыты) + startup-скан (`OnAssetRegistryFilesLoaded`: манифесты vs Ledger,
сравнение ТОЛЬКО по content_hash, не по mtime; режим prompt/silent — настройка,
default prompt) + CI-commandlet (headless, post-MVP).

**D27. Зеркалирование иерархий.** В настройках аддона — корень исходников
проекта (`source_root`); художник экспортирует в подпапки этого дерева.
UE-импортер кладёт ассеты детерминированно: `content_root` (настройка проекта
UE) + относительный путь папки источника + имя ассета; целевой путь UE
вычисляется, не хранится. Текстуры так же: `texture_root` — подпапка (или
совпадение) `source_root`, Content Browser зеркалит структуру. Следствия:
перемещение файла-исходника при том же UID = флаг `MOVE` в диффе (не
REMOVE+CREATE); коллизия имён в одном целевом каталоге — ошибка Analyzer'а,
не молчаливый суффикс; префиксы имён ассетов SM_/MI_/T_/CA_ — настройка
проекта с этими дефолтами. Дагоровские абсолютные пути в метаданных —
временные: аддон нормализует под texture_root (вне root —
`MH_E_TEXTURE_OUTSIDE_ROOT`); утилита однократного remap старого корня.
**Статус: зеркалирование и `source_root` сохраняются; старая модель
`texture_root`, remap и безусловная ошибка outside-root superseded D34.**

**D28. Реестр shader_class** (уточняет D17). Список Master-материалов не
ведётся руками: истина — папка `master_root` в UE-проекте. UE-плагин
генерирует из неё registry.json (материальная часть D17-реестра перестаёт
быть опциональной). Blender-аддон читает registry, если он есть: неизвестный
shader_class → warning при экспорте (registry может быть устаревшим); UE-импорт
при отсутствии Master'а по пути → ошибка материала в diff-отчёте («создайте
Master или исправьте shader_class»), импорт остальных ресурсов не блокируется.
Кнопка Refresh registry в UE-плагине + автогенерация при startup-скане.

**D29. Skeletal вне MVP, расширяемость подтверждена.** Финальная source schema v1
не резервирует и не принимает `kind: "skeletal_mesh"`: добавление kind меняет
on-disk enum и потребует `schema_version: 2`. Скелетка также потребует отдельного
тега `mh.skelser:1`, не меняя static serializer;
cm-контекст-менеджер из reference-скрипта умеет skeletal-режим (`only_deform`) —
при портировании ветка сохраняется за флагом. Блокирующих расширение решений
в v1 нет.

**D30. Blender-источник материалов MVP — `Material.dagormat`.** Аддон dag4blend
должен быть включён при авторинге dagormat-материалов. `shader_class`, `optional`,
`sides` и `textures` читаются из dagormat; `sides` попадает в `params`, пустые
текстурные слоты не сериализуются. Если dagormat отсутствует или shader пуст,
экспортируется валидная заглушка `rendinst_simple` с пустыми params/textures.
UID хранится в `Material['mh_uid']`. Node-based Blender material не является
альтернативным источником метаданных в MVP.
Пустой Blender material slot блокирует экспорт: у него нет MaterialUID,
поэтому section нельзя однозначно восстановить в UE. Один `slot_name`
не может ссылаться на разные MaterialUID. В multi-object ресурсе
порядок таблицы детерминирован ObjectUID, затем slot index; повторный
MaterialUID не дублируется. UE связывает слоты по `slot_name`, а не
по позиции в сводном массиве.

**D31. Material fingerprint и версия манифеста (историческое pre-freeze
решение).** Семантический `content_hash` материала считается по канон-форме
`{shader_class, params, textures}`; `uid`, `name` и `kind` в fingerprint не
входят, поэтому rename остаётся `RENAME`. **Статус: предложенные здесь manifest
v2, bundle-поля и inline material payload полностью superseded D33 и D37.**
Семантика material fingerprint сохранена D37; этот pre-freeze формат не является
legacy-форматом production-кодека.

**D32. [separate-source-operations] В пользовательском UX нет Bundle Export.**
Все инструменты собраны в одной N-panel вкладке `MH`, но остаются независимыми
секциями/операциями: `FBX Export`, `Composites` с подрежимами Import/Export и
`Materials`. Общего запуска «экспортировать всё» нет. `source_root` при этом
существует как единая проектная граница resolver'а, нормализации путей и
зеркалирования в Content Browser; это не bundle, не Texture Root и не выбранная
за пользователя папка экспорта. Composite Import v1 всегда выполняет recursive
resolve и импорт найденной геометрии: переключателей `Recursive`/`With DAGs` нет.
Будущий `Structure only` — отдельный режим из ROADMAP, не скрытая комбинация флагов.
Решение supersedes D2/D21/D27 в части UX и единицы транзакции.

**D33. [manifest-resource-receipt] Manifest — incremental directory-local
квитанция собственных ресурсов, а не владение всем каталогом.** Соседний
`export_manifest.json` имеет `schema: "mh.export_manifest"`, `schema_version: 1`.
Единственная опись — `resources[]` с kind `static_mesh | composite | material`;
строка каждого ресурса содержит `source`, относительный к owning manifest, и
`content_hash`, а mesh-строка также несёт `material_slots`. Top-level
`materials[]` и `external_dependencies[]` отсутствуют: зависимости вычисляются
из `resource_uid` узлов `.composite` и `material_slots` mesh-строк. Каждая
успешная операция атомарно обновляет payload и делает upsert только выбранного
ресурса, сохраняя несвязанные строки; manifest не даёт права удалять остальные
файлы каталога. Старые `mh.bundle_manifest`, BundleUID/BundleName и полная замена
набора ресурсов superseded. Pre-freeze `materials[]` production-reader не
получает: golden-артефакты регенерируются, локальные выгрузки при необходимости
переносятся одноразовым внешним скриптом. Принятие точного SHA этого schema-doc
commit внешним ревьювером означает финальную заморозку v1; любое следующее
изменение on-disk схемы требует
`schema_version: 2`.

**D34. [texture-path-canonicalization] Текстурный путь — только строка
`textures: {texN: string}`; `external_path` не хранится.** При извлечении Blender
путь `//` сначала разворачивается относительно сохранённого `.blend`, затем все
разделители нормализуются. Если абсолютный файл находится под `source_root`, на
диск обязательно пишется относительный путь с `/`; вне `source_root` —
нормализованный абсолютный путь. Следовательно, internal **тогда и только тогда**,
когда записанная строка относительна; классификация вычисляется и не участвует в
канон-форме отдельным флагом. Абсолютное представление файла внутри root запрещено,
поэтому один файл не имеет двух канонических форм на разных машинах. Настройка
`texture_policy: transitional | strict`, default `transitional`: внешний путь даёт
`MH_W_TEXTURE_OUTSIDE_ROOT` при экспорте/импорте; `strict` повышает тот же факт до
блокирующего `MH_E_TEXTURE_OUTSIDE_ROOT` для будущего CI. Текстуры не копируются,
отдельных Texture Root/remap нет; зеркалирование D27 опирается на `source_root`.
Решение supersedes D23 и старую текстурную часть D27.

**D35. [composite-graph-source] Composite graph восстанавливается только из
`.composite`, не из FBX.** Definition — Collection; каждый placement — Empty с
`instance_collection`, трансформом и Custom Properties. Импорт всегда рекурсивно
резолвит ссылки через D36, создаёт ровно одну sibling target Collection на
ResourceUID, импортирует найденные FBX в эти Collections и собирает иерархию
Empty-инстансов; порядок наполнения и сборки не является частью контракта.
Неразрешённая ссылка сохраняет NodeUID и ResourceUID в заглушке, чтобы `Resolve
Missing` мог наполнить ту же Collection без пересборки графа. Циклы обрабатываются
по D10. Сам FBX — только geometry payload и не обязан содержать node/placement
данные. `properties{}` поддерживаемых нод сохраняется lossless; reserved
random/variant node kinds до отдельного контракта блокируются, а не угадываются.

**D36. [uid-source-resolution] Любая внешняя UID-ссылка резолвится только внутри
единого `source_root` и только через единственного owning manifest.** Правило
одинаково для `static_mesh`, `composite` и `material`. `registry.json` —
приоритетный candidate hint: `source_path` указывает на payload, опциональный
`manifest_path` — на предполагаемый manifest. Истиной он не является. Resolver
всегда сканирует все файлы с точным именем `export_manifest.json` под
`source_root`, подтверждает registry-кандидат и проверяет уникальность владельца.
Stale/invalid registry row даёт `MH_W_REGISTRY_STALE`/
`MH_W_REGISTRY_INVALID` и fallback к результату scan; две manifest-строки одного
UID дают блокирующий `MH_E_AMBIGUOUS_RESOURCE_OWNER`, а не выбор первой.
`source_path`/`manifest_path` в registry относительны к `source_root`.
Дисковый кеш до отдельного решения и замера запрещён.

Honest-результат resolver'а:
`uid -> {payload_path, owning_manifest_path, manifest_row}`. Резолв не завершён
одним payload, потому что `material_slots`, `content_hash` и будущие `lods[]`
живут в manifest-строке. `source` всегда относителен к owning manifest, а payload
обязан оставаться под `source_root`. Транзакционный import/export фиксирует набор
прочитанных manifests и перед commit проверяет, что они не изменились.

Import при отсутствии ресурса не прерывает остальной граф: создаётся unresolved
Empty/Collection с сохранёнными NodeUID/ResourceUID и
`mh_unresolved=True`; Empty показывается красным cube-placeholder, а лог получает
`MH_W_UNRESOLVED_RESOURCE`. `Resolve Missing`
повторяет каскад и наполняет те же сущности. Export вычисляет зависимости из
`.composite` и `material_slots`, ничего не записывая в `external_dependencies`:
unresolved mesh/composite блокирует операцию `MH_E_UNRESOLVED_EXTERNAL`, а
unresolved material, который текущая операция не должна экспортировать, даёт
warning со списком и быстрым действием `Export materials…`; FBX-specific policy
определена D38. Рекурсивный обход дедуплицирует ресурс по UID; cycle-policy — D10.

**D37. [material-source-files] Материал — самостоятельный source-файл и обычный
ресурс manifest.** Один `<sanitized_name>__<uid8>.material` со схемой
`mh.material`, version 1 соответствует одному MI; payload содержит `uid`, `name`,
`shader_class`, `params`, `textures`, а D34 задаёт единственную форму текстурных
строк. Manifest хранит только общую resource-row с `uid`, kind `material`,
`name`, `source` и `content_hash`; material-specific payload в row отсутствует,
inline `materials[]` запрещён. Хеш считается по семантической
канон-форме `{shader_class, params, textures}`, не по `uid`/`name`/kind.

Первое размещение файла выбирается либо явно в секции `Materials` (Material +
Directory), либо принятой owner-политикой FBX Export из D38. При следующих
экспортах D36 находит существующую пару
(payload, owning manifest), и это расположение всегда выигрывает: обновление идёт
in place, повторный выбор папки не требуется. Rename меняет `name` внутри payload
и manifest, но **не переименовывает файл автоматически**: соответствие
`sanitized_name` текущему `name` не требуется и не проверяется, уникальность
обеспечивает `uid8`. Ручной транзакционный `Rename file to match` относится к
ROADMAP. Composite Export не размещает material payload; FBX Export следует D38.
Shared material из общей папки резолвится один раз по UID независимо от числа
mesh-потребителей и их каталогов. Решение supersedes inline-хранение D15, D22 и
pre-freeze manifest-часть D31.

**D38. [fbx-export-materials-toggle] FBX Export имеет явный Boolean `Export
Materials`, default ON.** Это принятое владельцем post-freeze operational/UX
amendment, совместимое с Source Schema v1: ни один JSON field, canonical byte или
hash rule не меняется. При ON операция собирает каждый уникальный MaterialUID,
использованный объектами выбранной Collection hierarchy. Уже существующий
material payload резолвится по D36 и обновляется in place вместе со своим owning
manifest; UID без владельца впервые создаётся рядом с выбранным FBX output и
добавляется в тот же directory-local manifest. При OFF записываются только FBX и
mesh resource-row; material payload/rows не создаются и не обновляются, а missing
UID остаются в structured warning с действием `Export materials…`. Это не общий
dependency-closure export и не возвращение Bundle Export: действие ограничено
явным Boolean и material slots выбранной FBX Collection hierarchy. Текстуры в
обоих режимах только ссылаются по D34 и никогда не копируются.
Ошибки material extraction/preparation/write превращаются в
`MH_W_MATERIAL_NOT_FOUND` и не откатывают уже committed FBX; crash после
material marker остаётся глобально fail-closed по D36 и требует recovery этого
MaterialUID до следующего resource-write.

## 4. Известные риски (проверяются первыми)

R1. Axis/handedness (D12) — go/no-go в первую неделю кода.
R2. Стоимость per-collection FBX-экспорта на 50–100 коллекций — замер; митигция: content_hash
skip (обязателен с первого дня); запасной план (multi-object FBX + сплит в UE) — только по замеру.
R3. Дубликаты UID при Ctrl+D в Blender — детект перед экспортом, экспорт падает с "Fix"-кнопкой.
Молчаливый дубликат UID отравляет все диффы — это worst-case баг системы.

## 5. Терминология (единая для кода и доков)

| Термин | Значение |
|---|---|
| Bundle | устаревший термин прототипа; в текущем UX и manifest-контракте не используется |
| Resource | переиспользуемое определение (mesh/material/actor class/composite) |
| Composite (Definition) | граф узлов, файл `.composite`, ассет `UMHCompositeAsset` |
| Node | узел definition (kind: group / mesh / composite_ref / variant_set / actor) |
| Placement | размещение composite в уровне (`AMHCompositeActor`) |
| Occurrence | конкретное воплощение узла в конкретном placement |
| Compile / Recompile | разворот definition+seed в компоненты/актёры |
| Adopt | привязка существующего uasset к UID |
| Fork | копия ресурса с новым UID |
| Ledger | `UMHImportLedger` (бывш. UMHSourceBundle): учёт импортированного — UID → uasset, applied-хеши |
| Manifest Importer | `UMHManifestImporter` (бывш. UMHBundleImporter): исполнитель плана импорта |
