> RESEARCH (2026-09-01). Не норматив: §2 описывает состояние плагина на `813dd70` до программы R, §3.5 — черновой порядок срезов. Норматив модели — `docs/16_recipe_model.md`, программа срезов — `KICKOFF_PROMPT.md` §5.

# Composit в Dagor Engine: устройство сущности и модель для форка в UE5

Дата: 2026-09-01. Источник: `GaijinEntertainment/DagorEngine` @ `7572366` (2026-08-16).
Все ссылки вида `file:line` — на этот коммит. Blender-половина (dag4blend,
формат `.composit.blk`) уже разобрана в `docs/03_dag4blend_analysis.md` и
`docs/reference_notes/dagor_corpus_inventory_20260828.md`; здесь только движковая
и редакторская сторона: как сущность существует, обновляется и что вокруг неё.

## 0. Главный вывод

Composit в Dagor — не ассет-объект и не актор. Это **рецепт** (`.composit.blk`)
плюс **исполнитель рецепта** (`CompositEntityPool`), который материализует
листья в чужие плоские пулы (`rendInst`, `prefab`, `gameObj` …). Всё остальное в
редакторе и в игре — рендер, коллизия, экспорт, кук — работает с плоскими пулами
листьев и **никогда не видит композит**. Документация прямо: движок композитов не
знает, в уровень уходят только листья (`_docs/…/composit_blk.md:22-28`).

Из этого следуют четыре инварианта, которые и дают скорость:

1. Рецепт парсится **один раз на ассет**, а не на инстанс и не на загрузку карты.
2. Инстанс композита — запись из нескольких int плюс срез указателей на листья.
   Состояние инстанса, которое хранит уровень: имя ассета, `tm`, два сида.
3. Материализация — чистая функция `(рецепт, rndSeed, instSeed, tm)`. Никакого
   «applied state», никаких подписей, никакой валидации в момент инстанцирования.
4. Обновление локально: смена рецепта пересоздаёт инстансы только этого ассета;
   смена листа (dag/rendInst) композит вообще не трогает — ресурс подменяется под
   уже стоящими указателями.

Дальше — как именно это устроено, затем сопоставление с MimirComposite и
проект форка.

## 1. Анатомия сущности

Файл: `prog/tools/sceneTools/daEditorX/services/compositMgr/compositMgrService.cpp`
(1767 строк, один сервис). Интерфейсы: `include/de3_composit.h`,
`de3_objEntity.h`, `de3_entityPool.h`, `de3_randomSeed.h`, `de3_dataBlockIdHolder.h`.

### 1.1 Три слоя: сервис → пул на ассет → инстанс

| Слой | Класс | Что хранит |
|---|---|---|
| Сервис (один на редактор) | `CompositEntityManagementService` (`:1642`) | `MultiEntityPool<CompositEntity, CompositEntityPool>`; фабрика `createEntity`/`cloneEntity`; поиск пула по ассету `findPool` (линейный по nameId, `de3_entityPool.h:149`) |
| Пул (один на composit-ассет) | `CompositEntityPool` (`:240`) | скомпилированный рецепт `Tab<Component> comp`, кодирование иерархии `beginInd/endInd`, `QuantedEntitiesPool ePool` — общий массив `IObjEntity*` всех листьев всех инстансов, `Tab<Requirement> req`, статический bbox, флаги `generated/forceInstSeed0/quantizeTm`, подписка `IDagorAssetChangeNotify` на **свой** ассет (`:284`) |
| Инстанс | `CompositEntity` (`:133`) | `tm`, `rndSeed`, `instSeed`, `autoRndSeed:1`, `gizmoEnabled:1`, `interactiveMove:1`, `entIdx:30` (начало среза в `ePool`), `placeTypeOverride`, `boxId` (кэш bbox), `dataBlockId` |

Инстанс не владеет листьями напрямую: его листья — это `ePool.ent[entIdx .. entIdx+realEntCnt)`,
где `realEntCnt = comp.size() - emptyComponents` одинаков для всех инстансов пула.
Пустые компоненты (узлы без ассета, чисто трансформные группы) в срез не входят.

### 1.2 Компиляция рецепта: `loadAsset` / `loadAssetData` (`:295`, `:1233`)

BLK-дерево `node{ … node{…} }` разворачивается в **плоский массив** `comp` в
DFS-порядке. Иерархия кодируется интервалами: `beginInd[k]/endInd[k]` — диапазон
индексов `comp`, который является поддеревом; `HierIterBase` (`:243`) при линейном
обходе поддерживает стек родительских матриц. Рекурсии при материализации нет.

Один `Component` (`:1024`):

```
entList : Tab<Obj{ IObjEntity* ent; float weight; u16 dataBlockId }>  // варианты узла
type    : DST_TYPE_MATRIX | DST_TYPE_EILER                              // фикс. tm или диапазоны
data    : union { TMatrix tm; ObjCoordData{rotX/Y/Z, offX/Y/Z, xScale, yScale, scale : Point2} }
p       : { placeType, aboveHt }
labelId, realIdx, defColorIdx, setInstSeed0:1
```

Ключевые правила компиляции:

- Веса нормализуются к сумме 1 при загрузке (`:1290-1299`). Узел с `entList.size()>1`
  или с ненулевой девиацией любого диапазона ставит флаг пула `generated`
  (`:1310`, `:1385`). Узел без девиаций сразу схлопывается в `DST_TYPE_MATRIX`
  через `getStTm` (`:1378-1384`) — рандом на нём больше никогда не вызывается.
- `ent` каждого варианта — **виртуальная сущность** соответствующего пула
  (`loadAddEnt` → `DAEDITOR3.createEntity(*a, /*virtual*/true)`, `:1226`).
  Виртуальная сущность (`VirtualMpEntity`, `de3_entityPool.h:231`) — это хэндл на
  пул листа: без трансформа, отдаёт bbox пула, участвует только как прототип для
  `IObjEntity::clone`. Ненайденный ассет → `ent = NULL` и conError, узел живёт дальше.
- Вложенный композит — тоже виртуальная сущность, но уже `CompositEntityPool`
  дочернего ассета: пул создаётся при первом обращении (`createEntity` → `findPool`/`addPool`,
  `:1650`), цикл ловит `CircularDependenceChecker` по стеку ассетов.
- `dataBlockId` нумеруется в том же DFS-порядке, что и в дереве редактора
  (`compositeEditorTreeData.cpp:54-85`); это единственная связь «узел BLK ↔ лист-сущность» для UI.
- Если пул не `generated`, bbox считается статически из `bboxBuildPairs`
  (`:1393`, `recalcPoolBbox :905`) — инстансам bbox достаётся бесплатно.
- `require{}` (условные узлы по меткам) грузится в `req` (`:325-386`); в корпусе
  26k файлов не встречается ни разу — можно не переносить.

### 1.3 Материализация: `createSubEnt` (`:407`)

```
entIdx = ePool.addEntities(realEntCnt)            // один срез на инстанс
seed = rndSeed; pos_seed = getSubEntInstSeed(e, tm) // instSeed или fnv1(tm[3])
for comp[i] (не пустые):
    obj = comp[i].selectEnt(seed)                 // 1 вариант: rnd(seed); N: frnd → по весам
    leaf = IObjEntity::clone(obj.ent)             // O(1) запись в пул листа
    leaf.setSeed(seed); leaf.setPerInstanceSeed(instSeed)   // через IRandomSeedHolder
    leaf.setCompositPlaceTypeOverride / setColor / setDataBlockId / setSubtype(ST_NOT_COLLIDABLE)
```

Важно: `selectEnt` **всегда** тратит один шаг ПСЧ, даже для узла с одним вариантом
(`:1056-1060`) — это и есть причина, по которой в Dagor результат стабилен при
добавлении/удалении вариантов у соседних узлов. У MimirComposite паритет этого
потока уже закреплён (`golden/v5/dagor_random_probe`).

Для вложенного композита сиды передаются **до** построения поддерева через
`PendingCloneSeeds` (`:107`): иначе `clone()` собрал бы поддерево с дефолтным
сидом, а `setSeed` снёс бы его и собрал заново — «×2 на уровень вложенности»
(комментарий `:99-105`). Это прямой урок для форка: сид — аргумент конструктора,
а не пост-фактум сеттер.

### 1.4 Трансформ и размещение: `CompositEntityPool::setTm` (`:603`)

Один линейный проход по `comp` с `HierIter`. Для `DST_TYPE_EILER` матрица
генерируется детерминированно из `rndseed` (`getTm :1160`: порядок выборок
rotX, rotY, rotZ, offX, offY, offZ, scale, yScale; композиция `rotY*rotZ*rotX`,
затем масштаб). Затем placement (`PT_coll/collNorm/3pod/fnd/flt`) — **пропускается
пока `gizmoEnabled`** (`:641`), т.е. во время перетаскивания коллизия не трейсится.
Потом `leaf->setTm(stm)`.

Сид листа-инстанса (`getSubEntInstSeed :398`): явный `instSeed`, если есть, иначе
`mem_hash_fnv1(tm[3], 12 байт)` — хэш позиции композита. Явный сид делает большой
композит «перемещаемым бесплатно»: при движении сид не меняется, `setPerInstanceSeed`
ничего не пересоздаёт (комментарий `:391-397`). У самого композита `autoRndSeed`
подмешивает биты позиции в `rndSeed` на время `setTm` (`:1503-1505`).

### 1.5 Два сида и что где живёт

| Сид | Кто хранит | Семантика | Как в уровне |
|---|---|---|---|
| `rndSeed` | `CompositEntity` | выбор вариантов и рандомные трансформы (layout) | `entSeed` (`hmlEntity.cpp:1056`) |
| `instSeed` | `CompositEntity` → все листья | per-instance appearance (цвет, riExtra seed) | `entPerInstSeed` (`:1058`) |

Уровень (`LandscapeEntityObject::save :1004`) хранит **только** `entName`, `tm`,
`place_type`, два сида и пару override-флагов. При загрузке (`propsChanged :1421`)
сущность создаётся заново `createEntity(asset)` → `setSeed` → `setPerInstanceSeed`
→ `setTm`. Никакого сохранённого результата материализации нет. Нерешаемое имя →
`InvalidEntity` (куб-заглушка, `invalidEntMgrService.cpp:5-40`), данные уровня
сохраняются нетронутыми (комментарий `hmlEntity.cpp:1049-1053`).

### 1.6 Лист: как выглядит `rendInst`-сущность и почему она дешёвая

`riMgrServiceAces.cpp`. Пул `AcesRendInstEntityPool` на ассет держит
`Ptr<RenderableInstanceLodsResource> res`, `pregenId`, `riExtraIdx`. Инстанс
`AcesRendInstEntity` — `tm`, `riexHandle`, `instSeed`, `colorIdx`, `dataBlockId`.

- `isSeedSetSupported() == false` (`:665`): лист говорит композиту «layout-сид на
  меня не влияет» — композит использует это в `seedAffectsResult` (см. 1.8).
- Ресурс грузится **лениво**: `delayRiResInit` до первого кадра (`:129`, `:764`),
  riExtra-инстанс создаётся только когда попадает в видимый прямоугольник
  (`assureRiExtraCreated :863`, счётчик `pendingRiExtraCount`).
- `setTm` (`:3288`): `memcmp` с прежней матрицей → ранний выход; иначе
  `moveRIGenExtra44`, tiled-scene обновляется только вне bulk-скоупа.

### 1.7 Обновление при изменении ассетов

**Слежение за файлами** (`assetMgrTrackChanges.cpp`): поток `ReadDirectoryChangesW`
(`:149`) пишет события в msg-pipe; на главном потоке `trackChangesContinuous`
(`:307`) дедуплицирует (одно имя за 10 мс), различает «изменился сам `.blk` ассета»,
«изменился источник, который экспортер ассета назвал в `gatherSrcDataFiles`»
(вторичное изменение) и «изменился `#include`-файл» (карта include→ассеты,
`:56-74`). Затем `a.reloadBlk()` и `callAssetChangeNotifications(a)` (`:693`)
подписчикам по (тип, nameId).

**Composit изменился** (`onAssetChanged :875`): `onAssetRemoved` (все листья
всех инстансов уничтожены, `ePool.reset()`, `comp.clear()`) → `loadAsset` →
для каждого живого инстанса `createSubEnt` + `setTm` + `setSubtype` +
`setEditLayerIdx`. Затрагиваются **только инстансы этого пула**; родительские
композиты не уведомляются — их лист-указатель на дочерний `CompositEntity`
остаётся валидным, дочерний пул пересобрал своё поддерево под ним.
Один глобальный счётчик `seedFlagGen++` инвалидирует кэш `seedAffectsResult` во всех пулах.

**Лист (rendInst) изменился** (`riMgrServiceAces.cpp:1056`): `init0/init1(true)`
→ `rendinst::update_rt_pregen_ri(pregenId, *res)` — ресурс подменяется **in place**,
инстансы не пересоздаются, композит не участвует. Это ключевое: реимпорт геометрии
в Dagor стоит ровно одну загрузку ресурса.

**Редактор композитов в AssetViewer** (`compositeEditor.cpp`): правка дерева →
`treeData.convertTreeDataToDataBlock` → `editedAsset->props.setFrom(&block)` →
отложенный `callAssetChangeNotifications` (`updateAssetFromTree :1342`,
`onDelayedRefresh :1251`). То есть UI идёт **тем же путём, что и внешнее
изменение файла**; отдельного «live edit» протокола нет. Три уровня refresh
(`Entity` / `EntityAndTransformation` / `EntityAndCompositeEditor`,
`compositeEditorRefreshType.h`) отличаются только тем, какие панели перестраивать.
Сохранение (`saveComposit :801`) пишет BLK при выключенном трекере, чтобы не
получить собственное событие.

### 1.8 Кэши и батчи

- `seedAffectsResult()` (`:987`): ленивый ответ «влияет ли layout-сид на результат
  этого пула» с generation-стампом; рекурсивно спрашивает дочерние пулы, для
  листьев — `isSeedSetSupported`. При `false` `setSeed` (`:1453`) вообще не
  пересобирает поддерево. MimirComposite уже имеет аналог `SeedAffectsResult`.
- `recreateSubent` (`:1464`): если пул `generated` — release+create+setTm; иначе
  только `updateSubEntSeed` (пробросить новый сид листьям без пересоздания).
- `boxCache` (`:71`): bbox инстанса считается один раз (`calcBox :555`) и
  сбрасывается при пересоздании.
- Gizmo/interactive move (`:1535`, `setCompositInteractiveMove`): рекурсивная
  глубина `gizmoModeDepth`, `ScopedRIExtraBulkUpdate`/`ScopedRIExtraDeferGridUpdate`
  — во время перетаскивания коллизионная сетка и tiled scene не обновляются;
  одна пересборка (`recreateSubent`) и один `flushRIExtraGridDefer` в конце,
  только на самом внешнем уровне.
- `hasSplineSubEnt` (`:962`) — ленивый флаг, чтобы не гонять `setTm` при ресиде.

### 1.9 Зависимости для билда

`assetExp/refProviders/compositRefProv.cpp`: reference provider обходит `node{}`
и собирает `name:t`/`ent{name}`/spline-генераторы в `Ref` без типа — тип
резолвится по проектному списку `genObjTypes` (`srvEngine.cpp:380-408`: суффикс
`:type` или первый найденный тип из списка). Никаких хэшей, receipt'ов и applied
state в зависимостях нет: граф зависимостей = граф имён.

### 1.10 Runtime-вариант в daNetGame (для полноты)

`prog/daNetGameLibs/composite_entity/`: тот же рецепт как ECS-компонент
`nodes:shared:array` (`select_one` с `gen__weight`, `nested`, `localTransform` или
euler-диапазоны). `composite_entity_created_es` при появлении спавнит дочерние
entity по сиду и запоминает `subentityEids/Tms/LocalTms/ParentIdx/Names`;
`track=transform` пересчитывает `parentTM * subentityTms[i]`; on_disappear
уничтожает детей. Это ровно «рецепт + исполнитель» в рантайме, без
кук-flattening — подтверждает, что модель переносима на runtime-мост.

## 2. Сопоставление с MimirComposite (`813dd70`)

| Аспект | Dagor | MimirComposite сейчас | Следствие |
|---|---|---|---|
| Единица компиляции рецепта | пул на ассет, живёт всю сессию | `FMHCompositeDefinitionEntry` на root + `AppliedHash` + `ClosureHash`; закрытие валидируется целиком на каждый root | общий меш валидируется N_roots раз; на карте 240 акторов — сотни полных обходов |
| Резолв листа | имя → пул (`findPool`), один раз на ассет | `MHLoadAppliedResource` = полный скан Asset Registry на каждый вызов + receipt из 6 тегов через `FAssetData(&Object)` | тысячи линейных сканов реестра на загрузку карты |
| Валидация | нет в горячем пути; ненайденное → `InvalidEntity` | fail-closed receipt на каждый ресурс каждого closure, требует завершённой компиляции меша | `FinishCompilation` на game thread |
| Невыбранные варианты | виртуальные прототипы (ресурс лениво), инстансов нет | `Resource()` грузит пакет каждого варианта синхронно | `all_option_unique_meshes ≫ selected` |
| Где живут листья | плоские пулы на ассет (riExtra) | ISM-бакеты **внутри актора** | рендер/селекшн/экспорт видят дерево; нет world-level пула |
| Состояние инстанса | `tm`, `rndSeed`, `instSeed` | `Seed`, `AppearanceSeed`, `CompactResolvedState`, три подписи, `PlacementDependencies` | лишние инварианты, которые надо поддерживать при каждом rebuild |
| Реимпорт листа | in-place подмена ресурса; композиты не трогаются | `InvalidateDefinition` → полный `RebuildComposite` каждого зависимого актора (definition + компоненты) | самая дорогая операция в M0 (`actor_rebuild_ms_total`) |
| Изменение рецепта | пересборка инстансов одного пула | то же (`InvalidateDefinition` по ключу composite) | сопоставимо |
| Сид вложенного композита | передаётся до постройки поддерева | передаётся в `MHResolveCompositePlan` сверху вниз | уже правильно |
| bbox инстанса | кэш на инстанс | считается UE по компонентам | ок |
| Перетаскивание | placement и грид отключены до конца драга | `bPlacementEditMode`/`UpdatePlacementBasis` | сопоставимо |

Итог: MimirComposite уже воспроизвёл рецепт, сиды и семантику выбора. Отстают
именно те четыре инварианта из §0, которые в Dagor дают дешёвую загрузку и
дешёвое обновление: компиляция на ассет, прототип листа без валидации в горячем
пути, плоские пулы, локальность обновлений.

## 3. Проект форка: «рецепт приготовления» для UE5

Цель — не копировать `de3_*`, а перенести инварианты в термины UE5 при сохранении
receipt-политики Source Protocol и уже принятых решений (нет placement, нет
Blueprint-акторов, нет обратной записи в Dagor).

### 3.1 Слои

```
UMHCompositeAsset            рецепт (как сейчас; источник истины — .composite)
FMHCompiledRecipe            плоский скомпилированный рецепт, 1 на ассет     ← CompositEntityPool.comp
UMHLeafPrototypeRegistry     прототип листа, 1 на FMHResourceKey             ← VirtualMpEntity + leaf pool
AMHCompositeActor            инстанс: Asset, Seed, AppearanceSeed, Transform  ← CompositEntity
UMHInstancePoolSubsystem     плоские world-level пулы ISM/HISM по мешу        ← riExtra pool
```

**FMHCompiledRecipe** (editor subsystem, ключ — `UMHCompositeAsset*` + `AppliedHash`):
плоский `TArray<FComponent>` в DFS-порядке с `BeginInd/EndInd`; у компонента —
`Options[{Kind, ResourceKey, Weight (нормализованный), NodePath}]`, `TransformKind
(Matrix|Ranges)`, схлопнутая матрица для узлов без девиации, флаг `bGenerated`
пула, `SeedAffectsResult` с generation-стампом. Вложенный композит — ссылка на
компилированный рецепт дочернего ассета (не инлайн), цикл — по стеку, как в Dagor.
Компиляция не грузит ни одного меша: только `MHExtractCompositeV5` + ключи.

**UMHLeafPrototypeRegistry**: `FMHResourceKey → {TWeakObjectPtr<UStaticMesh>,
EState {Unresolved, Loading, Ready, Invalid}, Bounds, ReceiptOk, Revision}`.
Резолв — один раз на ключ за сессию: `GetAssetByObjectPath(AppliedPlanObjectPath(Key))`
(O(1)), receipt проверяется **здесь и только здесь**, без требования завершённой
компиляции (по `UMHStaticMeshImportData` + пути, а не по `GetAssetRegistryTags`).
Инвалидация прототипа — по нотификации реимпорта этого ключа. Это переносит
fail-closed receipt-политику из горячего пути в холодный, не ослабляя её.

### 3.2 Материализация инстанса

`Materialize(Recipe, Seed, AppearanceSeed, ActorTransform)` = чистая функция →
`TArray<FLeafPlacement{ResourceKey, WorldMatrix, InstSeed, AppearanceChannels, NodePath}>`.
Никаких загрузок внутри. Затем placements отдаются в `UMHInstancePoolSubsystem`,
который держит по одному (H)ISM на уникальный меш (на уровень / на ячейку World
Partition) и возвращает актору список хэндлов `(Bucket, InstanceIndex)`. Актор
хранит только хэндлы и `LastPlacements` для outliner/селекшна; компонентов у него
нет (кроме root). Селекшн ISM-инстанса → актор через обратную карту пула, как
уже сделано для per-actor бакетов в U5.

Прототип не готов (`Loading`) → инстанс ставится в пул-заглушку (Dagor:
`InvalidEntity`/`pendingRiExtraCount`) и переключается по готовности; загрузка
карты перестаёт ждать компиляцию вообще.

### 3.3 Протокол обновлений

| Событие | Действие | Аналог Dagor |
|---|---|---|
| изменился `.composite` (реимпорт) | инвалидировать `FMHCompiledRecipe` этого ассета и всех рецептов, которые на него ссылаются (обратный индекс уже есть — `DefinitionKeysByDependency`); ре-материализовать инстансы только этих ассетов | `onAssetChanged` пула |
| реимпорт меша in place | `Prototype.Revision++`, обновить bounds; ISM уже указывает на тот же `UStaticMesh` — пересборка не нужна. Пересобирать только если изменились material slots (сравнить по receipt) | `update_rt_pregen_ri` |
| появился ранее отсутствующий меш | прототип `Invalid → Ready`, инстансы в пуле-заглушке переносятся в реальный бакет | `InvalidEntity` → реальная сущность |
| смена `Seed` | если `Recipe.SeedAffectsResult == None` — только appearance-каналы; иначе `Materialize` + diff по `NodePath` (уже есть `MHTryCompileCompositePlacementReseedV5`) | `recreateSubent` / `updateSubEntSeed` |
| перемещение актора | `UpdateInstanceTransform` по хэндлам, без `Materialize` | `setTm` с `memcmp` |
| драг гизмо | накапливать, применять на `PostEditMove(bFinished)` | `gizmoEnabled` |

### 3.4 Что осознанно НЕ переносить

- `place_type`, `aboveHt`, коллизионный placement — решение owner, объектов не
  плейсим. В компилированном рецепте поле оставить, но не интерпретировать.
- `require{}` — ноль употреблений в корпусе.
- Резолв по «имя → первый подходящий тип из `genObjTypes`» — у нас ключ типизирован
  (`FMHResourceKey`), это лучше.
- `dag2composit` — legacy-конвертер, к теме не относится.
- Кук-flattening — в MimirComposite уже выбран runtime-мост (как в daNetGame
  `composite_entity`), оставить.

### 3.5 Порядок срезов (каждый измерим счётчиками M0)

1. **R0 Prototype registry.** Заменить `MHLoadAppliedResource` в `FAppliedPlanBuilder`
   и placement compiler на реестр прототипов с O(1) резолвом и одноразовым
   receipt. Ожидание: `build_applied_graph_ms` падает на порядок при warm DDC;
   `registry_lookups` (новый счётчик) → ~число уникальных ключей за сессию.
2. **R1 Receipt без compile-wait.** Валидация по `UMHStaticMeshImportData` + пути;
   `wait_static_mesh_compilation_ms → 0`; компиляция уходит в фон.
3. **R2 Compiled recipe на ассет.** `FMHCompiledRecipe` вместо definition-на-root;
   вложенные рецепты по ссылке; `Finished`-дедупликация становится глобальной
   по построению. Ожидание: `definition_cache_misses` ≈ число уникальных ассетов,
   а не root'ов.
4. **R3 In-place reimport short-circuit.** Нотификация реимпорта меша больше не
   вызывает `RebuildComposite`; только `Revision++`/bounds. Ожидание:
   `actor_rebuild_ms_total → 0` для targeted reimport без смены слотов.
5. **R4 Placeholder + lazy prototypes.** Загрузка карты без синхронных `LoadObject`
   для невыбранных вариантов; `all_option_unique_meshes` перестаёт быть ценой.
6. **R5 World-level instance pools** (U6 из программы). После R0–R4, по замерам.

R0–R1 не меняют формат, подписи и поведение resolver — это чистая замена
механизма резолва, покрываемая существующими 172 тестами плюс red-тестом на
счётчики. R2 меняет ключ кэша (нужен owner freeze на `FMHCompositeDefinitionKey`).

## 4. Открытые вопросы для owner

1. Receipt-политика: допустимо ли переносить проверку 6 тегов с «каждый closure»
   на «один раз на ключ за сессию + при нотификации реимпорта»? Семантически это
   та же fail-closed проверка, но её точка — прототип, а не инстанс.
2. Плоские пулы (R5): один ISM на меш на уровень ломает текущую модель
   «компоненты принадлежат актору» для undo/дубликата/копипаста. Dagor решает это
   тем, что уровень хранит только рецепт + сиды + tm; нам нужно то же самое
   (транзиентная материализация уже есть, но undo компонентов придётся заменить
   undo хэндлов).
3. Заглушка вместо fail-closed ошибки при отсутствующем листе: сейчас
   `MH_E_UNRESOLVED_COMPOSITE_REFERENCE` блокирует весь композит; Dagor показывает
   куб и продолжает. Предлагаю заглушку + warning в Message Log, ошибку — только
   на build preflight.
