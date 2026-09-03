> Status: REFERENCE · исследование RS-1 по контракту `docs/contracts/research_dagor_composite_ops.md` (2026-09-03) · не норматив; проектные решения — только в разделе «открытые вопросы».

# daEditor (Dagor): как строится композит из выбранного и как он разбирается

Источник: `GaijinEntertainment/DagorEngine` @ `7572366` (main, 2026-08-16).
Ссылки — `путь:строки` на этот коммит. Модель композита, формат
`.composit.blk` и `compositMgrService.cpp` разобраны в
`docs/reference_notes/dagor_composit_research.md`; здесь только две операции
уровня и их окружение. Всё, что не подтверждено кодом, помечено явно.

Коротко: в daEditor **нет операции «собрать композит из выбранного в
сцене»** в смысле «заменить выбранные сущности одной». Есть «Export as
composit» — запись `.composit.blk` на диск без изменения сцены. «Split
composites» снимает ровно один слой, порождает обычные объекты уровня из
уже выбранных вариантов и работает через штатный undo объектного редактора,
который хранит не сущности, а лёгкие записи `(имя ассета, tm, два сида)`.

## 1. Build composite

### 1.1 В уровне: «Export as composit»

Команда контекстного меню объектного редактора HeightmapLand, доступна при
любом выделении (`prog/tools/sceneTools/daEditorX/HeightmapLand/hmlObject.cpp:1293-1294`,
обработчик `:1356-1358`). Реализация —
`HmapLandObjectEditor::exportAsComposit()`
(`prog/tools/sceneTools/daEditorX/HeightmapLand/hmlImportExport.cpp:164-316`).

Что делает, по порядку:

1. Создаёт `DataBlock` с `className:t="composit"` и одним корневым `node{ tm=IDENT }`
   (`hmlImportExport.cpp:169-172`).
2. Делит выделение на `LandscapeEntityObject` (сущности) и `SplineObject`
   (`:180-201`). Считает, сколько среди сущностей композитов (`comp_count`).
3. **Pivot** = позиция **первой** выбранной сущности (`center = tm.getcol(3)`,
   `:193-197`); если сущностей нет — первая точка первого сплайна (`:266-271`).
   Ориентация pivot не вычисляется: корневой `tm` тождественный, дочерние
   узлы получают свой **мировой** tm со сдвинутой на `-center` позицией
   (`:256-257`). Никакого усреднения bbox, никакого поворота.
4. Если выделение содержит композиты — вопрос «Split … iteratively to final
   entities before gathering?» (`:203-209`). При «да» выделение прогоняется
   через `splitComposits(...)` в цикле до исчерпания композитов (`:210-227`) —
   на **временных** объектах (в сцену они не добавляются), результат идёт в
   экспорт как плоский список. При «нет» композит попадает в новый файл как
   узел `name:"<asset>:composit"`.
5. Для каждой сущности: тип по `entity->getAssetTypeId()`; `-1` → ошибка в
   консоль и пропуск (`:233-243`). Имя узла — `entityName`, если в нём уже
   есть `:тип`, иначе `"<entityName>:<typeName>"` (`:251-254`). То есть
   допускается **любой** тип сущности с ассетом: rendInst, prefab, composit,
   gameObj и т.д.; фильтра по типу нет.
6. tm узла: `o->getTm()`; если у объекта задан `placeType` — вместо него
   берётся **размещённый** tm сущности `e->getTm(tm)` (`:245-248`), и в узел
   пишется `place_type:i` (`:258-259`). Сиды в узлы **не** пишутся.
7. Сплайны сохраняются как `node{ spline{…} }` с точками, сдвинутыми на
   `-center` (`:263-289`).
8. Диалог сохранения файла, путь по умолчанию `<sdk>/assets/`, расширение
   `.composit.blk` (`:297-314`). Файл создаётся на диске; **сущность в сцене
   не создаётся, исходные объекты не удаляются, в undo ничего не попадает** —
   сцена не менялась. Новый ассет подхватит трекер изменений asset manager'а
   (`assetMgrTrackChanges.cpp`), после чего его можно поставить как обычную
   сущность.

### 1.2 В AssetViewer: «Save selected nodes as a new composite»

Внутри редактора композитов (правка ассета, не уровня):
`CompositeEditor::saveSelectedAsNewComposite()`
(`prog/tools/AssetViewer/Entity/compositeEditor.cpp:1036-1136`). Условие —
выбранные узлы являются siblings одного родителя и их больше одного
(`canSaveSelectedAsComposite :1000-1034`). Узлы клонируются через
`DataBlock` **с их локальными tm без пересчёта** (`:1055-1064`) в новый
`.composit.blk` (`:1067-1076`). При включённой опции «replace in scene»
(`saveAsCompositeDlg.cpp:47`) выбранные узлы удаляются из родителя и на
позицию первого вставляется один узел `name:<newAsset>` (`:1096-1128`);
tm нового узла не задаётся (тождественный), поэтому геометрия совпадает.
Undo — снимок всего `DataBlock` ассета (`compositeEditorUndo.cpp:7-31, 49-66`).

Отдельно `CompositeAssetCreator::create` (`prog/tools/AssetViewer/compositeAssetCreator.cpp:99-115`)
создаёт **пустой** `.composit.blk` с одной строкой `className` — это «New
composit», не сборка из выделения.

## 2. Break / split composite

### 2.1 Команда и код

Контекстное меню «Split composites» (`hmlObject.cpp:1295-1296`, обработчик
`:1360-1362`). Реализация — две функции в
`prog/tools/sceneTools/daEditorX/HeightmapLand/hmlEntity.cpp`:
статическая `splitComposits(sel, compObj, decompObj, splitSplinesBlk, otherObj)`
(`:1489-1557`) — чистая функция «один слой», и интерактивная
`splitComposits()` (`:1559-1634`) — диалог, цикл рекурсии, undo.

### 2.2 Что снимается

**Ровно один слой.** Для каждого выбранного объекта-композита берётся
`ICompositObj::getCompositSubEntityCount()` / `getCompositSubEntity(j)` —
это **уже материализованные** дочерние сущности верхнего уровня
(`hmlEntity.cpp:1503-1507`). Вложенный композит — одна такая сущность
(`CompositEntity` дочернего пула), он превращается в самостоятельный
объект-композит и **остаётся композитом**. Рекурсия — только по чекбоксу
«Recursive (split sub-components as well)» (`:1575`), реализованному как
цикл повторных однослойных split'ов над результатом предыдущего
(`:1583-1601`), пока среди новых объектов есть композиты.

Random-узел: у пула уже стоит выбранный вариант (`createSubEnt` →
`comp[i].selectEnt(seed)` → `clone(obj.ent)`,
`services/compositMgr/compositMgrService.cpp:426-441`), поэтому split
видит только его. Имя нового объекта — `e->getObjAssetName()`
(`hmlEntity.cpp:1521-1523`) — имя **выбранного** ассета; невыбранные
варианты исчезают. Сущность без имени ассета (пустой узел) пропускается
(`:1521-1522`). GameObj — обычная сущность с именем ассета, обрабатывается
так же (`hmlEntity.cpp` не различает типы; `getObjAssetName` есть у всех
пулов). Сплайны — исключение: сохраняются через `saveSplineTo` в отдельный
`DataBlock` и заново создаются как `SplineObject` (`:1508-1519`, `:1616-1628`).

Placement: в новый объект копируется только `placeType` из
`getCompositSubEntityProps(j)` (`:1547-1549`); tm берётся **размещённый**
(`e->getTm(tm)`, `:1550-1553`) и ставится как `setWtm`. То есть после split
объект имеет запечённый мировой трансформ плюс тот же `placeType`, который
при следующем `propsChanged` разместит его заново по коллизии — ссылки на
профиль как таковой в Dagor нет (`placeType` — enum, не ассет).

### 2.3 Сиды детей

Точная формула (`hmlEntity.cpp:1524-1538`):

```
if (child has IRandomSeedHolder):  seed = child.getSeed(); perInst = child.getPerInstanceSeed()
else:                              seed = parent.getSeed(); perInst = parent.getPerInstanceSeed()
obj = new LandscapeEntityObject(assetName, seed); obj.setPerInstSeed(perInst)
```

Что возвращают getters:

- Вложенный композит (`CompositEntity`): `getSeed() = rndSeed`,
  `getPerInstanceSeed() = instSeed` (`compositMgrService.cpp:196,198`).
  `rndSeed` ему выставил `cloneEntity` из `PendingCloneSeeds`
  (`:1693-1700`) — это **текущее значение бегущего сида родителя после
  `selectEnt`** для этого компонента (`:426-441`: `seed` продвигается
  `rnd`/`frnd` в `selectEnt`, `:1053-1069`, и передаётся как `rnd_seed`).
  `instSeed` = `inst_seed` родителя (или 0 при `setInstSeed0`) — сам он
  равен явному `instSeed` родителя либо `fnv1(tm[3])` позиции родителя
  (`getSubEntInstSeed`, `:398-405`). Поэтому вложенный композит после split
  **воспроизводит себя точно**: те же rndSeed/instSeed, что были внутри.
- RendInst: `getSeed() = instSeed`, `getPerInstanceSeed() = autoInstSeed ? 0 :
  instSeed`, `isSeedSetSupported() = false`
  (`services/riMgr/riMgrServiceAces.cpp:661-665`). Layout-сид для листа не
  имеет смысла, ему передаётся его же instSeed; per-instance сид сохраняется,
  если был явным.

Сид **родителя** нигде не сохраняется: композит удаляется (`:1605`), и его
`rndSeed` живёт только в undo-записи.

### 2.4 Трансформы и имена

World tm ребёнка — `e->getTm(tm)` (`:1551`), т.е. то, что пул положил в
`setTm` с учётом euler-генерации и placement
(`compositMgrService.cpp:603-700`). Локальный пересчёт не нужен: сущность
уже стоит в мире. Имя: `"<compositObjName>_<assetName>"` со срезанным
`:type` (`:1543-1545`), затем уникализация `setUniqName` (`:1608-1615`).
Слой редактирования наследуется от композита, если lpIndex совпадает
(`:1539-1540`).

### 2.5 Undo

`getUndoSystem()->begin(); removeObjects(compObj, /*undo*/true);
addObjects(decompObj, true); … accept("Decomposit N objects")`
(`hmlEntity.cpp:1604-1630`). Механика — штатные `UndoRemoveObjects` /
`UndoAddObjects` объектного редактора
(`prog/tools/sharedInclude/EditorCore/ec_ObjectEditor.h:464-497`): undo-запись
держит `Ptr` на **объекты-записи** (`LandscapeEntityObject`), не на
сущности. `onRemove` уничтожает сущность (`hmlEntity.cpp:1272`), `onAdd`
при отсутствии сущности вызывает `propsChanged()` (`:1273-1285`), который
создаёт её заново из `(entityName, tm, rndSeed, perInstSeed)`
(`:1421-1470`). Undo split = удалить N новых записей (их сущности гибнут) +
вернуть запись композита (сущность пересоздаётся из рецепта). Никакого
восстановления внутренностей: **состояние = рецепт + сиды + tm**.

## 3. Почему быстро

- Никаких чтений ассетов при split: все дочерние сущности уже существуют в
  плоских пулах; split только читает `getObjAssetName`, сиды, `getTm`,
  `placeType` (`hmlEntity.cpp:1503-1553`) — O(детей верхнего слоя) чтения
  полей.
- Новые `LandscapeEntityObject` создаются **без сущности**
  (конструктор `:124-131`, `entity = NULL`); сущность появляется в `onAdd →
  propsChanged` (`:1273-1285`) как `createEntity(asset)` — O(1) запись в пул
  ассета (`compositMgrService.cpp:1650-1690`, `riMgrServiceAces.cpp`) без
  валидации receipt'ов, без загрузки ресурса (у rendInst он лениво,
  `delayRiResInit`), без ожидания компиляции.
- Удаление композита — `onRemove → destroy_it(entity)` → пул освобождает
  срез `ePool` (`releaseSubEnt`); внутренние листья не «перекладываются» в
  новые объекты, а уничтожаются и создаются заново уже под новыми записями —
  это дешевле любого переноса владения, потому что запись пула — десятки байт.
- Export as composit — O(выделения) записи в `DataBlock` и один файл; сцену
  не трогает вовсе.
- Сложность: split — O(N_top-level детей) операций над пулами, каждая O(1)
  amortized; undo — то же, но с пересозданием одной композитной сущности
  (O(её замыкания) через `createSubEnt/setTm`). Тяжёлые побочные эффекты
  (riExtra grid, tiled scene) — только через `setTm` при создании, внутри
  bulk-скоупов пула.

## 4. Промежуточная семантика

**Edit in place в уровне — нет.** В HeightmapLand композит — непрозрачный
объект: можно менять tm, сиды, `placeType`, override auto-inst-seed
(`hmlEntity.cpp:428-457`), но не структуру. Селекция pixel-perfect ходит по
поддереву только чтобы попасть в bbox (`hmlObject.cpp:2406-2435`), hit всё
равно относится к `LandscapeEntityObject` композита. Структура правится
**только в AssetViewer** для ассета целиком (`compositeEditor.cpp`), и
изменение доходит до всех инстансов уровня через
`callAssetChangeNotifications` → `onAssetChanged` пула
(`compositMgrService.cpp:875-905`). Обратной операции «inline вложенный
композит в родителя» в редакторе композитов не найдено (проверено меню
`compositeEditor.cpp:1769-1801` и панель `compositeEditorPanel.cpp:469-513`:
Add node / Add entity / Change asset / Delete / Save as new / Save unique).

**Маркеров «сущность из композита» нет.** После split объект — обычный
`LandscapeEntityObject`; единственный след — имя `"<composit>_<asset>"`
(`hmlEntity.cpp:1545`) и унаследованный edit-layer (`:1539`). Внутри
живого композита дочерние сущности отличаются `subtype ST_NOT_COLLIDABLE`
(`compositMgrService.cpp:458`) и `dataBlockId` (`:455`), но при split эти
поля не переносятся — новая сущность создаётся с нуля.

## 5. Соответствие нашей модели

Наши операции: `UMHCompositeLevelSubsystem::BuildComposites` (pivot = центр
bbox выделения, `MHCompositeLevelSubsystem.cpp:349-364`) и
`BreakComposites` (`:532-…`, `MHCollectBreakSpecs :181-230` итерирует
`Plan.Leaves` всех глубин). Рецепт — `FMHCompiledRecipe`, верхний слой —
`Components` с `ParentIndex == INDEX_NONE` (`MHCompiledRecipe.h:62`),
вложенный композит — `NestedRecipe` (`:66`).

| Dagor | Что это в терминах нашей модели | Открытые вопросы |
|---|---|---|
| Export as composit: файл на диск, сцена не меняется, undo нет | `BuildComposites` = **экспорт рецепта** (`.composite` в source root → индекс → импорт → `UMHCompositeAsset`), без спавна актора и без удаления выделения; или два шага: экспорт + отдельный «Place». | Владелец решает, нужен ли «замена выделения одним актором» как второй шаг; в Dagor его нет. |
| Pivot = первая выбранная сущность, без поворота | Наш pivot — центр bbox. Оба — тождественная ориентация. | Оставить bbox-центр (детерминирован от выделения, не от порядка клика) или паритет с Dagor? Влияет на паритет `.composite`, экспортированных из UE, при обратном импорте в Blender. |
| Узел = `name:type` любой сущности с ассетом; composit → узел-ссылка, при желании предварительный split | Узел `Mesh` / `Actor` / `Composite` грамматики v5 §6.1; выбранный `AMHCompositeActor` → узел `Composite` со ссылкой, не инлайн. | Наш `BuildComposites` сейчас принимает только `AStaticMeshActor`? Нужно ли «split before gather» как опция. |
| `place_type` в узел, tm — размещённый | Placement не плейсим (решение owner); tm — actor transform. | — |
| Split: один слой, дети — `ICompositObj::getCompositSubEntity` | `BreakComposites` = обход **`Components` верхнего слоя** (`ParentIndex == INDEX_NONE`) резолвленного layout'а: `Mesh` → `AStaticMeshActor`, `Actor` → актор класса, `Composite` → **`AMHCompositeActor` дочернего ассета**, пустой узел → ничего. Не `Plan.Leaves`. | Нужен доступ к «выбранный вариант + world tm + сиды на компонент верхнего слоя» из `MHResolveCompositeLayout` (сейчас plan даёт листья, не компоненты). |
| Random → выбранный вариант, невыбранные исчезают | То же: компонент верхнего слоя после Layout имеет ровно один `Options[selected]`. | — |
| Сиды ребёнка-композита: `rndSeed` = бегущий сид родителя после `selectEnt` компонента; `instSeed` = inst-сид родителя (явный или `fnv1(pos)`) | Наш resolver уже передаёт layout-сид сверху вниз в `MHResolveCompositePlan` (§6.6 doc 10); Break должен взять **тот сид, который получил вложенный composite при resolve**, и `AppearanceSeed` родителя (у нас явный, не позиционный). | Экспортирует ли `FMHResolvedCompositePlan` сид вложенного композита на момент входа в него? Если нет — расширение plan/layout (не решение этого документа). |
| Сиды ребёнка-меша: `seed = instSeed`, perInst если явный | У меша сидов нет; `AppearanceSeed` — канал appearance, переносить в `AStaticMeshActor` некуда. | Нужен ли перенос appearance-каналов (custom data) на разобранный меш? |
| Имя `"<composit>_<asset>"`, layer наследуется | Label актора `"<ActorLabel>_<LogicalName>"`, тот же `ULevel`/folder. | — |
| Undo = remove/add **записей**, сущность пересоздаётся из `(asset, tm, seeds)` в `onAdd` | Именно то, что описывает ADR §2.10 и OPEN-R-1: транзакционен только актор с `(asset, seeds, tm)`; plan-view компоненты **не** транзакционны, `PostEditUndo` материализует заново. Дефект №3 полевого теста — следствие транзакционных компонентов. | Порядок: делать Break на верхнем слое до R4 (пулы) или внутри R4 — зависит от того, можно ли снять `RF_Transactional` с plan-view компонентов раньше пулов. |
| Быстро, потому что нет чтений ассетов и валидации | Break не должен вызывать `BuildProofNow`/`MHCheckGeneratedAssetClaims` (дефект №1); достаточно Layout + `ResolveEndpoint` для верхнего слоя. | — |
| Edit in place в уровне отсутствует; структура только в редакторе ассета | У нас `BeginEdit/CommitEdit` уровня — шире, чем у Dagor. | Не тема RS-1. |
| Split нескольких композитов сразу; не-композиты в выделении игнорируются с сообщением | То же. | — |

## 6. Что нужно повторить у нас (сводка для R4-pre)

1. Break = один слой рецепта: компоненты с `ParentIndex == INDEX_NONE`
   после Layout; вложенный композит остаётся `AMHCompositeActor` со своим
   ассетом, `Seed` = сид, полученный им при resolve, `AppearanceSeed` =
   родительский; random → только выбранный вариант; пустой узел — ничего.
   Рекурсивный режим — цикл однослойных Break'ов, а не обход `Plan.Leaves`.
2. Break без proof: никакого `BuildProofNow` и tag-запросов; только Layout +
   `ResolveEndpoint` для N компонентов верхнего слоя.
3. World tm ребёнка — из Layout (уже посчитан), не пересчёт.
4. Имена `"<label>_<logicalName>"`, тот же уровень/folder.
5. Undo как в Dagor: транзакция хранит **записи** (актор с `asset, seeds, tm`
   + спавненные акторы), сущности/компоненты пересоздаются в
   `PostEditUndo`/`OnConstruction`; plan-view компоненты не должны быть
   `RF_Transactional`.
6. Build = экспорт рецепта на диск в source root (как Export as composit), без
   удаления выделения; pivot — открытый вопрос (Dagor: первая сущность,
   без поворота).
7. Не переносить: `placeType`, спец-путь для сплайнов, `dataBlockId`.
