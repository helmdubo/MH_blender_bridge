# 16 — Модель «рецепт + исполнитель» (Recipe Model): норматив редакторского слоя MimirComposite

Статус: **ратифицировано owner 2026-09-02** (`KICKOFF_PROMPT.md`, срез D0).
Активный норматив репозитория — ровно три файла: `KICKOFF_PROMPT.md`
(роль исполнителя, программа срезов, гейты), этот документ (модель и её
инварианты) и `README.md` (карта и полевые команды). Всё остальное в
`docs/` — либо протокольный справочник (`docs/10_source_protocol_v5_plan.md`,
`docs/reference_notes/`), либо история (`docs/archive/`, `docs/receipts/`).
Карта документов — §10.

Документ описывает **редакторскую часть** `ue/MimirComposite`
(`Source/MimirCompositeEditor/.../Composite/*`, `Performance/*`,
`Settings/MHCompositeSettings.h`) и её контракт с актором. Wire-формат
`.composite`/`.placement`, identity, индекс, receipt на ассетах, сиды и
runtime-мост заданы в `docs/10_source_protocol_v5_plan.md` и здесь только
адресуются (§5).

## 0. Формула

```text
Рецепт компилируется один раз на ассет.
Инстанс хранит только (asset, seed, appearanceSeed, transform, nodeOverrides).
Материализация — чистая функция; она не грузит, не спавнит, не читает мир.
Лист резолвится по детерминированному пути, один раз на ключ за сессию.
Ненайденный или незагруженный лист — заглушка, не ошибка актора.
Провенанс (receipt, хэши) проверяется только в точках выхода.
Обновление локально: рецепт пересобирает только свои инстансы,
реимпорт меша композит не трогает.
Рендер, селекшн и экспорт работают с плоскими пулами и композит не видят.
```

Это перенос инвариантов Dagor `CompositEntityPool` / `CompositEntity`
(разбор — `docs/reference_notes/dagor_composit_research.md`) в термины UE5 при
сохранении Source Protocol v5. Программа не переписывает плагин: формат,
канонизация, сиды и resolver, runtime-мост и Source-конвейер остаются (§1).
Переписывается редакторский слой `Composite/` и его контракт с актором.

## 1. Что неизменно

| Область | Файлы | Причина |
|---|---|---|
| Wire-формат `.composite` v5, `.placement`, канонизация, хэши payload | `Composite/MHCompositeProtocol.*`, `Source/MHPayloadHashes.*`, `Runtime/Canonical/*` | формат заморожен, Blender пишет его |
| Сиды и resolver | `Runtime/Random/MHRandomStream.*`, `MHResolveCompositePlan`, `MHBuildRandomSourceClosure` | паритет с Dagor доказан golden-тестами |
| Runtime-мост | `Runtime/Composite/MHRuntimeCompositeActor.*`, `MHRuntimeCompositeInput.*`, `Editor/Composite/MHCompositeRuntimeBridge.*` | admission снапшота — точка выхода, она остаётся |
| Source-конвейер | `Source/*`, `Index/*`, `StaticMesh/*`, `Material/*`, `Texture/*`, `Geometry/*`, `Diagnostics/*` | отдельная линия S (KICKOFF §6), в программу R не входит |
| Receipt на ассетах | `UMHStaticMeshImportData`, `UMHMaterialSourceData`, `UMHTextureSourceData`, теги `MH.*` при записи | receipt остаётся источником истины о провенансе; меняется только **где** он читается |
| Blender-аддон, `golden/`, `reference/` | — | вне scope; `golden/` только по явному пункту среза |

## 2. Слои

Пять слоёв, чистая зависимость сверху вниз, каждый — отдельный файл(ы) и
отдельный тест. Никаких обратных ссылок из нижних слоёв в актор.

```text
UMHCompositeAsset             рецепт (источник истины — .composite)
FMHCompiledRecipe             скомпилированный рецепт, 1 на ассет               ← Dagor CompositEntityPool.comp
UMHLeafPrototypeRegistry      прототип листа, 1 на FMHResourceKey               ← Dagor VirtualMpEntity + leaf pool
MHMaterialize(...)            чистая функция: рецепт+сиды+tm+overrides → placements
UMHInstancePoolSubsystem      плоские пулы ISM/акторов на мир, выдаёт хэндлы     ← Dagor riExtra pool
AMHCompositeActor             инстанс: Asset, Seed, AppearanceSeed, Transform, NodeOverrides
```

### 2.1 `FMHCompiledRecipe` (Editor, `Composite/MHCompiledRecipe.{h,cpp}`)

Кэш в editor subsystem; ключ — `TWeakObjectPtr<UMHCompositeAsset>` +
`AppliedHash` ассета. Компиляция **не грузит ни одного меша**: только
`MHExtractCompositeV5` и ключи ресурсов.

- `TArray<FComponent>` в DFS-порядке; иерархия — интервалами `BeginInd/EndInd`
  (как в Dagor `loadAssetData`); обход линейный со стеком родительских матриц.
- `FComponent { NodePath; Options[{Kind, ResourceKey, WeightNormalized}];
  TransformKind (Matrix | Ranges); FMatrix CollapsedTm для узлов без девиаций;
  bAppearanceSeedBoundary; ProfileName }`.
- Веса нормализованы к сумме 1 при компиляции.
- Флаг `bGenerated` (есть узел с >1 варианта или с девиацией).
- `SeedAffectsResult` — кэш с generation-стампом; инвалидируется любой
  перекомпиляцией любого рецепта (глобальный счётчик, как `seedFlagGen`).
- Вложенный композит — **ссылка** на скомпилированный рецепт дочернего ассета,
  не инлайн. Цикл — по стеку компиляции, диагностика `MH_E_COMPOSITE_CYCLE`.
- Обратный индекс `Dependents: ResourceKey → {recipes}` — единственный
  источник «кого пересобирать» при изменении ассета.
- Для resolver собирается прежний `FMHRandomSourceGraph` из ссылок; сам
  resolver не меняется. `RawHashes` в графе заполняются из receipt'ов
  прототипов лениво, только для точек выхода (§3).

### 2.2 `UMHLeafPrototypeRegistry` (Editor subsystem)

`FMHResourceKey → FMHLeafPrototype { TWeakObjectPtr<UObject> Object; EState
{Unresolved, Loading, Ready, Invalid}; FBox Bounds; FString ReceiptError;
uint32 Revision; TSubclassOf<AActor> ActorClass }`.

- Резолв меша **только** по детерминированному пути
  `/Game/MH/Generated/<Folder>/<LogicalName>.<LogicalName>` (10 §8) через
  `FSoftObjectPath` / `LoadObject`. **Запрещено**: `IAssetRegistry::GetAssets`
  с tag-фильтром, `GetAssetsByTags`, `FAssetData(&Object)` и любое чтение
  `GetAssetRegistryTags` живого объекта в горячем пути. Теги `MH.*` —
  проекция receipt для индекса (10 §7), не механизм резолва листа.
- Receipt проверяется **один раз на ключ за сессию** по
  `UMHStaticMeshImportData` (LogicalName, SourceRelativePath, SourceHash) и
  пути объекта. Провал → `Invalid` + `ReceiptError`; объект остаётся в
  реестре как заглушка. Никакого `FinishCompilation`: receipt не зависит от
  состояния компиляции меша.
- Загрузка асинхронная (`FStreamableManager`); пока `Loading`, прототип
  отдаёт `PlaceholderMesh` из настроек плагина
  (`UMHCompositeSettings::PlaceholderMesh`, по умолчанию движковый куб).
- `Actor`-лист: класс из `UMHCompositeSettings::ActorClassRegistry`
  (имя → `FSoftClassPath`). Blueprint-классы допустимы (owner 2026-09-02).
  Нерезолвимое имя → `Invalid` + заглушка-меш.
- Инвалидация: `MHNotifyGeneratedResourceChanged(Key)` → `Revision++`,
  receipt перечитывается, bounds обновляются. Реестр **не** инициирует
  пересборку композитов; это делает протокол обновлений (§4) по обратному
  индексу рецептов.

### 2.3 `MHMaterialize`

```text
FMHMaterializeResult MHMaterialize(
    const FMHCompiledRecipe& Recipe, int32 Seed, int32 AppearanceSeed,
    const FTransform& ActorTransform, const TMap<FString, FTransform>& NodeOverrides)
  -> TArray<FMHLeafPlacement{ Kind; ResourceKey; NodePath; FMatrix WorldMatrix;
                              int32 InstSeed; FMHAppearanceChannels; bOverridden }>
  + Warnings
```

Чистая функция: не грузит, не спавнит, не читает мир. Порядок: resolver даёт
план по сидам (10 §6.6, §13.1, §13.8, §6.9) → трансформ узла → **override
узла заменяет локальный трансформ узла** (после генерации, до умножения на
родителя) → world matrix → appearance. `InstSeed` листа = `fnv1(translation)`
композита, если у актора не задан явный; паритет с Dagor `getSubEntInstSeed`.

### 2.4 `UMHInstancePoolSubsystem` (World subsystem, Editor)

- Один `UInstancedStaticMeshComponent` на `FISMComponentDescriptor`
  (движковый дескриптор из Packed Level Actor: меш + материалы + флаги +
  appearance layout) на мир. Владелец компонентов — служебный актор пула
  `AMHInstancePoolActor` (transient, не сохраняется).
- API: `FMHInstanceHandle Add(Placement)`, `Update(Handle, WorldMatrix,
  Appearance)`, `Remove(Handle)`, `ReverseLookup(Component, InstanceIndex) →
  (Actor, NodePath)` для Outliner/селекшна, `BeginBulk()/EndBulk()` для
  одного `MarkRenderStateDirty` на пачку.
- `Actor`-листья: спавн через пул по классу (`SpawnActor` с `RF_Transient`,
  `bIsEditorOnlyActor=false`), аттач к root композита, `Owner = композит`,
  метки для Outliner. Свет, PPV, любые BP — этим же путём.
- Undo: транзакционен только актор композита (его свойства); пул
  восстанавливает материализацию из состояния актора в `PostEditUndo`.
  Компоненты пула вне транзакций (OPEN-R-1).

### 2.5 `AMHCompositeActor`

Сохраняемое состояние — **ровно** это:

```text
TSoftObjectPtr<UMHCompositeAsset> CompositeAsset;
int32 Seed; bool bAutoSeed;
int32 AppearanceSeed; bool bAutoAppearanceSeed;
TMap<FString, FTransform> NodeOverrides;   // NodePath -> локальный трансформ узла
```

Транзиентное: `TArray<FMHInstanceHandle>`, `TArray<FMHLeafPlacement>
LastPlacements` (для Outliner и reseed-diff), `LastError/LastWarnings`.

Актор **не хранит** результат материализации, подписи, сжатое состояние
резолва, список зависимостей размещения, скомпилированный граф или флаги
«подпись устарела». Точный перечень удаляемых сущностей — §7.

`RebuildPlacement` = `Materialize` + diff по `NodePath` с `LastPlacements` +
`Update/Add/Remove` хэндлов. Перемещение актора — только `Update` по хэндлам,
без `Materialize`. `bPlacementEditMode` и basis-update сохраняют смысл, но
опираются на `NodePath`, не на подписи.

Операции над overrides (Details/Outliner): `Reset override`, `Reset all`,
`Promote to composite…` (записать текущие локальные трансформы как новый
`.composite` через существующий экспортный путь level subsystem). Override
узла, которого больше нет в рецепте, **сохраняется** и попадает в Warnings
(`MH_W_ORPHAN_OVERRIDE`); молча не удаляется.

После R7 физическая симуляция и автосборка по объёмам проектируются как
**производители `NodeOverrides`**: отдельные инструменты, ядро о них не знает.
Их контракт: вход — `LastPlacements` актора, выход — запись в `NodeOverrides`
через транзакцию.

## 3. Точки выхода провенанса

Только здесь читаются `SourceHash`/`AppliedHash` receipt'ов и сверяются с
индексом:

1. `PreSaveWorld` — warning в Message Log «N composite instance(s) reference
   stale or missing resources»; сохранение не блокируется.
2. Build preflight (`MHCompositeBuildPreflight*`) — **error**, блокирует.
3. Runtime snapshot (admission `MHRuntimeCompositeInput`) — **error**.
4. Export/Level operations (`UMHCompositeLevelSubsystem::Build/Break`) — error.

Вне этих точек хэши никого не блокируют и ничего не пересобирают. Это переносит
fail-closed receipt-политику Source Protocol из горячего пути в холодный, не
ослабляя её.

## 4. Протокол обновлений

| Событие | Действие | Не делать |
|---|---|---|
| Реимпорт `.composite` (свой или вложенный) | перекомпилировать рецепт этого ассета; по обратному индексу — рецепты, которые на него ссылаются; `RebuildPlacement` только у акторов этих рецептов | не трогать акторов других рецептов |
| Реимпорт меша in place (тот же `UStaticMesh*`) | `Registry.Revision++`, bounds; если material slots изменились — `Update` appearance у хэндлов этого ключа | **не** `RebuildPlacement`, не перекомпилировать рецепты |
| Меш появился (был `Invalid`/`Loading`) | прототип → `Ready`; пул переносит инстансы этого ключа с заглушки на реальный дескриптор | не пересобирать актора |
| Смена `Seed` | `SeedAffectsResult == None` → только appearance; иначе `Materialize` + diff | не пересоздавать неизменившиеся хэндлы |
| Смена `AppearanceSeed` | `Update` appearance-каналов у хэндлов | — |
| Перемещение актора | `Update(WorldMatrix)` по хэндлам | не `Materialize` |
| Драг гизмо | накапливать; применить на `PostEditMove(bFinished)` | не обновлять пул на каждый тик |
| Изменение `NodeOverrides` | `Materialize` + diff по `NodePath` (меняются только затронутые поддеревья) | — |
| Загрузка карты | `PostRegisterAllComponents` → `RebuildPlacement` с заглушками для `Loading`; ни одного синхронного `LoadObject` меша, ни одного `FinishCompilation` | — |

## 5. Wire-формат, сиды, runtime-мост: где норматив

Эти контракты **не меняются** программой R и заданы в
`docs/10_source_protocol_v5_plan.md`:

| Контракт | Раздел 10 |
|---|---|
| Identity, `ResourceKey`, каноничные имена | §2 |
| Project Resource Index (проекция, статусы, рёбра) | §3 |
| FBX-транспорт и классификация узлов | §4 |
| `.material` и текстуры | §5 |
| `.composite` v5: грамматика, parent-local T/R/S, shear | §6.1–§6.2 |
| `.placement` v1 и его применение | §6.3, §13.2, §13.4 |
| Blender authoring, source closure, export batch | §6.4–§6.5 |
| Layout seed `mh.random_stream:1`, path-derived streams | §6.6, §13.1, §13.8 |
| Appearance seed `mh.appearance:1`, границы, каналы | §6.9 |
| Runtime-мост `AMHRuntimeCompositeActor`, Break, cook | §6.7 |
| Receipt на ассетах, шесть тегов `MH.<Name>` | §7 |
| Генерируемые пути UE | §8 |
| NodePath, closure hash, хэш плана резолвера | §13.3 |

Runtime-мост в одном абзаце: `FMHRuntimeCompositeInput` = сериализуемый
seed-free граф (`GraphBytes`) + `Bindings` на **все** варианты (включая
невыбранные и zero-weight) и actor-классы. `AMHRuntimeCompositeActor` подаёт
его тому же resolver с сидами размещения; PIE = packaged = editor по плану.
Admission снапшота — точка выхода §3.3: там и только там stale receipt даёт
error. Ничего нового в runtime-мост программа R не добавляет (OPEN-R-3).

## 6. Инварианты Dagor, которые нельзя потерять

Из `compositMgrService.cpp` DagorEngine @ `7572366`
(`docs/reference_notes/dagor_composit_research.md`):

1. Рецепт компилируется один раз на ассет; инстанс — запись из нескольких int
   плюс срез хэндлов.
2. `selectEnt` тратит один шаг ПСЧ даже на узле с одним вариантом — паритет
   закреплён в `MHRandomStream`, не сломать.
3. Сид вложенного композита передаётся **до** построения поддерева, иначе
   ×2 на уровень вложенности.
4. Узел без девиаций схлопывается в матрицу при компиляции; рандом на нём не
   вызывается.
5. Реимпорт листа подменяет ресурс под указателями; композит не участвует.
6. Изменение рецепта пересобирает инстансы только этого пула; родители не
   уведомляются.
7. Ненайденный лист — заглушка; уровень сохраняется без потерь.
8. Во время драга гизмо тяжёлые побочные эффекты откладываются до конца
   драга; одна пересборка на самом внешнем уровне.
9. Layout-сид и instance-сид — разные сущности; instance-сид по умолчанию —
   хэш позиции.
10. Экспорт и рендер работают с плоскими пулами и композит не видят.

Срез, нарушающий пункт из этого списка, — OPEN-вопрос (§9), не решение
исполнителя.

## 7. Удалённые термины и греп-гейт

Owner многократно сталкивался с тем, что внешние агенты восстанавливали старую
модель по устаревшему нормативу. Поэтому термины удалённых концептов
запрещены в активных документах, а не помечены как superseded.

**Гейт.** После каждого среза, начиная с D0, обе команды дают пустой вывод.
Первая — все активные документы, кроме этого файла; вторая — этот файл без
таблицы §7 (единственное место, где термины перечислены намеренно):

```bash
T='ResolvedSignature|CompactResolvedState|PlacementDependencies|ClosureHash|AppliedDefinition|AppliedGraph|definition cache|definition-кэш|MHLoadAppliedResource|AppliedPlanReceipt|FinalizeDeferredMeshes|applied[ -]state|MH\.Managed|MH\.Kind'
grep -rIil -E "$T" README.md docs --exclude-dir=archive --exclude-dir=receipts --exclude-dir=reference_notes --exclude=16_recipe_model.md
sed -n '/^## 7\./,/^## 8\./!p' docs/16_recipe_model.md | grep -i -E "$T"
```

Область: `README.md`, `docs/**/*.md`. Исключения: `docs/archive/` (история
под шапкой `HISTORY`), `docs/receipts/` (история исполнения),
`docs/reference_notes/` (датированные исследования чужих движков и прежнего
состояния плагина; улика, не норматив), `KICKOFF_PROMPT.md` §7.3 (стартовый
список). Список терминов пополняется срезом, который удаляет концепт, и живёт
только в таблице ниже.

| Термин | Был | Что вместо | Удаляет |
|---|---|---|---|
| `ResolvedSignature` (свойство актора, гейт «подпись устарела») | derived-подпись плана на `AMHCompositeActor`; stale-rebuild-skip по подписям | diff по `NodePath` с `LastPlacements`; хэш плана резолвера остаётся артефактом паритета golden-векторов (`resolved_signature`, 10 §13.3; OPEN-R-5) | D0 (docs), R2b (code) |
| `CompactResolvedState` как гейт | сжатое состояние резолва на акторе, сверка перед rebuild | `Materialize` — чистая функция, сверять нечего | D0, R2b |
| `PlacementDependencies` | список зависимостей размещения на акторе | обратный индекс `Dependents` в `FMHCompiledRecipe` | D0, R2b |
| `ClosureHash` как ключ кэша | часть `FMHCompositeDefinitionKey` | ключ рецепта = ассет + `AppliedHash`; `closure_hash` плана резолвера остаётся (10 §13.3) | D0, R2a |
| `AppliedDefinition`, `AppliedGraph` | скомпилированный граф, хранимый актором | `FMHCompiledRecipe`, 1 на ассет, ссылка из актора не хранится | D0, R2a/R2b |
| `definition cache` / `definition-кэш` (`FMHCompositeDefinitionEntry` на root + closure) | кэш определений по root-композиту | кэш `FMHCompiledRecipe` по ассету; вложенные по ссылке | D0, R2a |
| `MHLoadAppliedResource` | скан Asset Registry с tag-фильтром на каждый резолв | `UMHLeafPrototypeRegistry`, детерминированный путь | D0, R0 |
| `AppliedPlanReceipt` на живом объекте | receipt из шести тегов через `FAssetData(&Object)` | receipt по `UMHStaticMeshImportData`, один раз на ключ | D0, R0 |
| `FinalizeDeferredMeshes` / `FinishCompilation` в `Composite/` | ожидание компиляции меша в горячем пути | `PlaceholderMesh` + async, receipt не зависит от компиляции | D0, R1 |
| `applied state` в значении состояния актора | «актор хранит доказуемо консистентное применённое состояние» | §2.5; на ассетах это называется **receipt** | D0 |
| теги `MH.<Name>` как способ резолва листа | `GetAssetsByTags` / tag-фильтр | путь `/Game/MH/Generated/...` (10 §8); теги — проекция receipt для индекса | D0, R0 |

Тесты, закрепляющие удалённый концепт, удаляются вместе с концептом в том же
срезе (KICKOFF §7.5); каждое удаление называется в квитанции среза.

## 8. Программа срезов

Порядок, red-тесты, удаляемое и гейты — `KICKOFF_PROMPT.md` §5 и §9; здесь не
дублируются. Срезы: D0 (документы) → R0 реестр прототипов → R1 receipt без
compile-wait → R2a/R2b/R2c рецепт, актор, точки выхода → R3 реимпорт без
rebuild → R4 пул инстансов → R5 actor-листья → R6 UI overrides → R7 async
прототипы. Линия S (Source-конвейер) исполняется параллельно и в программу R
не входит (KICKOFF §6).

Инструментация M0 (`mh.PerfTrace`, `MH_PERF_MAPLOAD` /
`MH_PERF_STARTUP_SCAN` / `MH_PERF_REIMPORT`) смержена в `main` (PR #60);
R0 базируется на ней и добавляет счётчики `registry_lookups`,
`package_loads`, `receipt_validations`. Полевой протокол замеров —
`docs/receipts/m0_perf_instrumentation.md` §6.

## 9. OPEN-вопросы

Правило: новую семантику не угадывать; реальная дыра оформляется как
`OPEN-R-<N>` (Контекст → Вопрос → Временное fail-closed правило → Статус),
затронутая часть — STOP до ответа owner.

### OPEN-R-1 — Undo для пула

- Контекст: компоненты `UMHInstancePoolSubsystem` вне транзакций.
- Вопрос: нужен ли undo на уровне инстансов пула?
- Правило: транзакционен только актор; пул восстанавливается из его состояния
  в `PostEditUndo`. Если owner потребует undo инстансов — R4 расширяется.
- Статус: открыт.

### OPEN-R-2 — Заглушка

- Правило: движковый куб `/Engine/BasicShapes/Cube`, настраивается в
  `UMHCompositeSettings::PlaceholderMesh`.
- Статус: открыт.

### OPEN-R-3 — Actor-листья и PIE/cook

- Правило: `Actor`-листья участвуют в runtime-снапшоте так же, как сейчас
  `ActorClassRegistry`; ничего нового в runtime-мост не добавляется до
  отдельного решения.
- Статус: открыт.

### OPEN-R-4 — Ключ дескриптора пула

- Правило: `FISMComponentDescriptor` + `AppearanceCustomDataBaseIndex`;
  кастомный ключ — только если движковый не различает нужные случаи
  (доказать тестом).
- Статус: открыт.

### OPEN-R-5 — Хэш плана резолвера после удаления подписи актора

- Контекст (найдено в D0): KICKOFF §3.5 удаляет подпись плана на акторе
  (первая строка таблицы §7) «из актора и из всех документов», а §2
  замораживает resolver, runtime-мост и `golden/`.
  При этом resolver в runtime-модуле (`MHRandomStream.cpp`) считает хэш плана
  из прообраза 10 §13.3, `AMHRuntimeCompositeActor` хранит его,
  `golden/v5/*` содержат поля `resolved_signature`, `appearance_signature`,
  `placement_signature`, а отчёт плана печатает `resolved_signature` и
  `closure_hash`.
- Вопрос: распространяется ли удаление на хэш плана в resolver, runtime-акторе
  и golden-векторах (это правка замороженных областей), или только на
  свойство редакторского актора и его гейт-семантику?
- Временное правило: удаляются **только** свойство `AMHCompositeActor` и все
  решения по нему (stale-rebuild-skip, admission на инстансе) — срез R2b.
  Resolver, `AMHRuntimeCompositeActor`, отчёт плана и golden-векторы не
  трогаются; в документах хэш плана называется по имени поля golden-вектора
  (`resolved_signature`) и описывается как артефакт паритета, не состояние
  актора. Точка выхода §3.3 сверяет receipt'ы, а не подпись.
- Статус: открыт, ответ owner нужен до R2b.

## 10. Карта документов

| Документ | Роль |
|---|---|
| `KICKOFF_PROMPT.md` | активный промпт исполнителя: роль, программа R/S, гейты |
| `docs/16_recipe_model.md` | этот норматив модели |
| `README.md` | карта репозитория и полевые команды |
| `docs/10_source_protocol_v5_plan.md` | протокольный справочник v5: identity, индекс, FBX, материалы, `.composite`/`.placement`, сиды, receipt, runtime-мост (§5) |
| `docs/reference_notes/*` | датированные исследования Dagor/dag4blend; улика, не норматив |
| `docs/receipts/*` | история исполнения срезов; квитанция ≠ owner acceptance |
| `docs/archive/*` | история под шапкой `HISTORY`: 00–09 (v1–v4), 11–15 (срезы v5, программа U0–U7), ADR, amendments, аудиты, `QUESTIONS.md` (решённые `OPEN-V4-*`/`OPEN-V5-*` — их нормативный остаток уже перенесён в 10 §13), proposals, spikes |

Квитанция среза D0 с таблицей «документ → действие → почему» —
`docs/receipts/recipe_d0.md`.
