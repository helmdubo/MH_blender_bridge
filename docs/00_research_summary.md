# 00 — Research Summary & Decision Log

> Этот документ — сжатый результат research-сессии. Он объясняет **почему** система устроена так, а не иначе.
> Исполнитель обязан прочитать его перед началом работы. Спорить с решениями можно, но только явно:
> предложение фиксируется как вопрос в PR/issue, решения из Decision Log не переопределяются молча.

## 1. Что мы строим

DCC-driven composite asset pipeline: Blender → versioned Source Bundle → UE5.
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

**D1. FBX — только геометрический payload одного ресурса.** Никакой семантики, иерархии,
placements, материальных определений внутри FBX. Один FBX детерминированно создаёт один UStaticMesh.
Причина: legacy scene import и Interchange по-разному мангли имена/иерархию; вывод composite
semantics из FBX-иерархии хрупок; Blender FBX exporter разворачивает collection instances.

**D2. Bundle = каталог, не архив.** `*.composite` (JSON, один файл = один composite),
`meshes/*.mesh.fbx` (один на mesh-ресурс), скрытый `export_manifest.json` — commit-marker,
пишется последним, атомарная замена. Пользователь видит две сущности: composite-файл и
геометрию — как `.composit.blk` и `.dag` в Dagor. Manifest = аналог невидимого `.folder.blk`/кеша.

**D3. Шесть уровней identity:** BundleUID, ResourceUID (mesh datablock / material / actor class),
CompositeDefinitionUID, NodeUID (узел в definition), PlacementUID (актёр в уровне),
OccurrenceUID = Hash(PlacementUID, NodePathUID) — конкретное воплощение узла
(NodePathUID = хеш пути NodeUID'ов от корня, т.к. один child может входить дважды).
Имя — только display/package name/fallback. Rename ≠ Delete+Create. Linked duplicate =
общий ResourceUID; Make Single User = новый ResourceUID.

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

**D10. Циклы composite-ссылок — hard error, три рубежа:** Blender-экспорт (DFS, локально +
по registry), UE-импорт (топосортировка — детект бесплатно), компилятор (стек CompositeUID +
error-заглушка в сцене + лимит глубины ~64; никогда не крэшить). Ромбы (переиспользование) —
легальны: различать "в стеке" и "уже развёрнут по другому пути".

**D11. UE-ассеты:** `UMHCompositeAsset` (импортируется из `.composite` через
UFactory+FReimportHandler, read-only generic editor, VisibleAnywhere, SourceJsonSnapshot внутри),
`UMHSourceBundle` (manifest: ResourceUID → SoftObjectPath + hashes),
`AMHCompositeActor` (+ UActorFactory для drag&drop; PostSpawnActor → новый PlacementSeed;
PostDuplicate — политика сида настраиваемая). Ссылки между композитами — FSoftObjectPath,
заполняются при импорте из UID → Reference Viewer работает бесплатно. Auto-reimport через
directory watcher по manifest-файлу. Asset Registry tags: CompositeUID, bundle, node count.

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
скриптов как шаг Finalize. Отдельный materials.json — решение отложено. Материал — ресурс
с UID в dependency graph (обязательно к моменту incremental reimport материалов).

**D16. Миграция существующего контента:** операция Adopt Existing (привязка существующего
uasset к UID по имени/пути с подтверждением) — обязательна до раскатки на реальный проект,
в MVP не входит.

**D17. Registry.json (UE → Blender):** обратный реестр (uid → kind, name, path, owner,
для классов — DisplayName/PreviewBounds/Category/policies). В MVP опционален/пуст,
нужен для внешних ссылок, forks, BP-дропдауна.

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

## 4. Известные риски (проверяются первыми)

R1. Axis/handedness (D12) — go/no-go в первую неделю кода.
R2. Стоимость per-collection FBX-экспорта на 50–100 коллекций — замер; митигция: content_hash
skip (обязателен с первого дня); запасной план (multi-object FBX + сплит в UE) — только по замеру.
R3. Дубликаты UID при Ctrl+D в Blender — детект перед экспортом, экспорт падает с "Fix"-кнопкой.
Молчаливый дубликат UID отравляет все диффы — это worst-case баг системы.

## 5. Терминология (единая для кода и доков)

| Термин | Значение |
|---|---|
| Bundle | каталог экспорта одного .blend |
| Resource | переиспользуемое определение (mesh/material/actor class/composite) |
| Composite (Definition) | граф узлов, файл `.composite`, ассет `UMHCompositeAsset` |
| Node | узел definition (kind: group / mesh / composite_ref / variant_set / actor) |
| Placement | размещение composite в уровне (`AMHCompositeActor`) |
| Occurrence | конкретное воплощение узла в конкретном placement |
| Compile / Recompile | разворот definition+seed в компоненты/актёры |
| Adopt | привязка существующего uasset к UID |
| Fork | копия ресурса с новым UID |
