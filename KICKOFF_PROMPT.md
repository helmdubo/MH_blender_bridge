# KICKOFF v2 — MimirComposite: Recipe Model (preview plane + proof plane)

> **АКТИВНЫЙ ПРОМПТ ИСПОЛНИТЕЛЯ. Заменяет `KICKOFF_PROMPT.md` целиком.**
> Status: NORMATIVE · Architecture version: Recipe Model v2.1 (D0b) · Ратифицировано
> owner 2026-09-02 после внешнего аудита. Версия v1 (2026-09-02, до аудита) —
> HISTORY, не использовать. Этот документ самодостаточен: сессия, которая его
> читает, не имеет другого контекста.

## 0. Кто ты и что делаешь

Ты — исполнитель в роли Principal Technical Artist UE-фронта в
`helmdubo/MH_blender_bridge` (Blender Extension `addon/mh4blend` + UE 5.7.4
plugin `ue/MimirComposite`). Owner (Alexander) единолично принимает и мержит PR;
внешний ai-аудитор рецензирует крупные срезы. Ты не мержишь. Ты не меняешь Engine.

Программа переводит редакторский слой `ue/MimirComposite` на модель
**«рецепт + исполнитель»** по формуле:

> Dagor-подобный быстрый preview-исполнитель + Mimir-подобный строгий proof на границах.

Три плоскости, которые нельзя смешивать:

| Плоскость | Что делает | Что ей запрещено |
|---|---|---|
| **Preview** | компиляция рецепта, выбор по сидам, материализация в пулы, заглушки, async-загрузка | хэши источников, full-closure proof, ожидание компиляции, чтение Asset Registry тегов |
| **Proof** | full closure, receipt freshness, `ClosureHash`/`ResolvedSignature`, admission runtime-снапшота, build preflight, export | блокировать загрузку карты или preview |
| **Source** | инкрементальный индекс файлов, targeted reimport, background freshness | парсить FBX в скане, делать FullScan на targeted reimport |

Это не переписывание плагина с нуля. Формат `.composite`, канонизация, сиды,
reference resolver, runtime-мост, Source-конвейер остаются. Переписывается
редакторский слой `Composite/` и его контракт с актором.

Архитектурный порядок задаёт KICKOFF. Фактическую точку продолжения задаёт
`docs/RECIPE_EXECUTION_STATUS.md`; перед началом любого среза исполнитель
читает его и начинает **только** срез со статусом NEXT/READY.

## 1. Обязательное чтение до любых правок

1. Этот документ целиком.
2. `README.md`, `docs/NORMATIVE_INDEX.md` (создаётся в D0a).
3. `docs/reference_notes/dagor_composit_research.md` — разбор composit в Dagor.
   Если файла нет — остановись и запроси у owner.
4. `docs/reference_notes/external_audit_recipe_model_20260902.md` — внешний
   аудит v1 (положить рядом; если нет — запросить). Его 12 требований
   инкорпорированы сюда; при расхождении — этот документ.
5. `docs/receipts/m0_perf_instrumentation.md` — счётчики `mh.PerfTrace` и
   полевой протокол.
6. Исходники, которые будешь менять: `ue/MimirComposite/Source/
   MimirCompositeEditor/{Public,Private}/Composite/*`, `Private/Performance/*`,
   `Public/Settings/MHCompositeSettings.h`, resolver в `MimirCompositeRuntime`
   (только фазовое разделение, §3.3), тесты в `MimirCompositeTests`.

Документы со статусом `HISTORY` (после D0a — в `docs/archive/`) для реализации
не используются, даже если написаны в повелительном наклонении.

## 2. Что остаётся неизменным

| Область | Файлы | Причина |
|---|---|---|
| Wire-формат `.composite` v5, `.placement`, канонизация, хэши payload | `Composite/MHCompositeProtocol.*`, `Source/MHPayloadHashes.*`, `Runtime/Canonical/*` | формат заморожен, Blender пишет его |
| Сиды и reference resolver | `Runtime/Random/MHRandomStream.*`, `MHResolveCompositePlan`, `MHBuildRandomSourceClosure` | паритет с Dagor доказан golden; допускается **только** фазовое разделение §3.3 без изменения выборок и математики |
| Proof-артефакты | `ClosureHash`, `ResolvedSignature`, runtime-admission, build preflight, export | остаются как доказательство; перестают быть состоянием актора |
| Runtime-мост | `Runtime/Composite/MHRuntimeCompositeActor.*`, `MHRuntimeCompositeInput.*`, `Editor/Composite/MHCompositeRuntimeBridge.*` | точка выхода |
| Receipt на ассетах | `UMHStaticMeshImportData`, `UMHMaterialSourceData`, `UMHTextureSourceData`, теги `MH.*` при записи | меняется только **где** и **что** из receipt читается (§3.4) |
| Source-конвейер | `Source/*`, `Index/*`, `StaticMesh/*`, `Material/*`, `Texture/*`, `Geometry/*`, `Diagnostics/*` | линия S (§6) |
| Семантика сидов актора | `Seed` (layout), `AppearanceSeed` (явный, сохраняемый, **не зависит от позиции**) | решение owner 2026-09-02; Dagor position-derived instSeed — opt-in политика вне программы |
| Blender-аддон, `golden/`, `reference/` | — | вне scope; `golden/` только по явному пункту среза |

## 3. Целевая архитектура

```
UMHCompositeAsset                 authoring-рецепт (источник истины — .composite)
        │
        ▼
FMHCompiledRecipeRegistry         плоская программа рецепта, 1 на ассет
                                  без загрузки мешей, без closure proof
        │
        ├──────────────────────────────┐
        ▼                              ▼
MHMaterializeLayout                MHBuildCompositeProof
preview plane                      proof plane
без хэшей                          full closure + receipts + signatures
                                        │
                                        ▼
                                   FMHProofCache
                                   background proof cache по RecipeRevision (R2c)
        │
        ▼
UMHEndpointPrototypeRegistry      точный путь, identity-admission, async-загрузка
        │
        ▼
UMHInstancePoolSubsystem          пулы по домену (ULevel), стабильные хэндлы
        │
        ▼
AMHCompositeActor                 asset + сиды + (позже) безопасные overrides
```

`FMHProofCache` — editor subsystem с background task по `RecipeRevision`;
реализуется в R2c.

### 3.1 `FMHCompiledRecipe` (Editor, `Composite/MHCompiledRecipe.{h,cpp}`)

Реестр — editor subsystem; ключ `UMHCompositeAsset* + RecipeRevision`
(`uint32`, инкремент при реимпорте/`PostEditChange` ассета); `AppliedHash`
ассета хранится в записи как debug-атрибут и **никогда** не сравнивается с
Source Root в preview. Компиляция не грузит ни одного меша: только
`MHExtractCompositeV5` и ключи ресурсов.

- `TArray<FComponent>` в DFS-порядке; иерархия интервалами `BeginInd/EndInd`;
  обход линейный со стеком родительских матриц.
- `FComponent { NodePath; NodeFingerprint (§3.7); Options[{Kind, ResourceKey,
  WeightRaw}]; TransformKind (Matrix | Ranges); canonical TRS как в источнике;
  bAppearanceSeedBoundary; ProfileName }`.
- **Веса хранятся сырыми, TRS — как в источнике.** Нормализация весов и
  схлопывание узлов без девиаций в матрицу — только после exhaustive parity
  (§3.3, гейт §9) отдельным срезом; до этого запрещены.
- `bGenerated`; `SeedAffectsResult` — кэш с generation-стампом, инвалидируется
  глобальным счётчиком при любой перекомпиляции любого рецепта.
- Вложенный композит — **ссылка** на скомпилированный рецепт дочернего ассета
  по хэндлу. Цикл — по стеку компиляции, `MH_E_COMPOSITE_CYCLE`.
- Обратный индекс `Dependents: ResourceKey → {recipes}` — только для
  локализации rematerialize (§4), **не** для перекомпиляции родителей.

### 3.2 `UMHEndpointPrototypeRegistry` (Editor subsystem)

`FMHResourceKey → FMHEndpointPrototype { TWeakObjectPtr<UObject> Object;
EState {Unresolved, Loading, Ready, Invalid}; FBox Bounds; FString
AdmissionError; uint32 Revision; uint32 PayloadRevision; uint32 BoundsRevision;
uint64 BucketDescriptorHash; uint64 CollisionInterfaceHash;
uint64 MaterialBindingHash; TSoftClassPtr<AActor> ActorClass }`.

- Резолв меша **только** по детерминированному пути
  `/Game/MH/Generated/<Folder>/<LogicalName>.<LogicalName>` через
  `FSoftObjectPath`. **Запрещено** в preview-плоскости: `IAssetRegistry::
  GetAssets` с tag-фильтром, `GetAssetsByTags`, `FAssetData(&Object)`,
  чтение `GetAssetRegistryTags` живого объекта, `FinishCompilation`.
- Загрузка выбранных endpoint'ов асинхронная (`FStreamableManager`); пока
  `Loading` — `UMHCompositeSettings::PlaceholderMesh` (по умолчанию
  `/Engine/BasicShapes/Cube`). Невыбранные варианты не загружаются.
- Пять хэшей/ревизий интерфейса меша для пула (единое поле заменено срезом
  П4), считаются при `Ready` и при каждом `Revision++`:
  `PayloadRevision` — геометрия / render resource → render refresh;
  `BoundsRevision` — пространственные bounds → bounds cache, streaming bounds;
  `BucketDescriptorHash` — поля совместимости `FISMComponentDescriptor`
  (material slots: число, порядок, дефолтные материалы; sections и их флаги;
  LOD count) → миграция бакета;
  `CollisionInterfaceHash` — `BodySetup` / collision policy / trace companion
  → recreate physics state;
  `MaterialBindingHash` — slots / default materials / override compatibility
  → reconcile дескриптора материалов.
- `Actor`-лист: класс из `UMHCompositeSettings::ActorClassRegistry`
  (whitelist имя → `FSoftClassPath`; Blueprint допустим — решение owner).
  Имя вне whitelist → `Invalid` + заглушка-меш.

### 3.3 Resolver: фазовое разделение (Runtime, единственная правка resolver)

Существующий `MHResolveCompositePlan(...)` становится обёрткой над тремя
фазами с **неизменной** последовательностью выборок и математикой:

```
MHResolveCompositeLayout(...)       // выбор вариантов, трансформы узлов
MHResolveCompositeAppearance(...)   // appearance-каналы по AppearanceSeed
MHBuildCompositeProof(...)          // full closure, RawHashes, ClosureHash, ResolvedSignature
MHResolveCompositePlan = Layout → Appearance → Proof   // прежний публичный контракт
```

Preview вызывает Layout + Appearance на графе, собранном из compiled recipes
(сырые веса, canonical TRS). Proof-плоскость вызывает полную обёртку.
Текущий `MHResolveCompositePlan` (`MimirCompositeRuntime/Private/Random/
MHRandomStream.cpp:809`) первым действием вызывает `MHBuildRandomSourceClosure`,
которое жёстко завершается ошибкой `missing or invalid raw payload hash` для
любого ресурса полного замыкания, включая невыбранные варианты. Фазовое
разделение `Layout → Appearance → Proof` **обязательно**; первый шаг R2a —
его реализация, а не проверка.

**Shadow parity — постоянный CI-гейт**, не разовая проверка: на всех golden-
векторах, экстремальных/малых весах, глубокой вложенности, non-uniform scale,
отрицательных вращениях, граничных RawU32 сравниваются decisions, leaves,
матрицы, appearance-каналы, selected dependencies между reference-обёрткой и
preview-путём. Расхождение = red.

### 3.4 Два уровня admission endpoint'а

| Уровень | Где | Проверяет | При провале |
|---|---|---|---|
| **Identity admission** | реестр, один раз на ключ за сессию + при `Revision++` | объект по каноническому пути существует; embedded receipt есть и структурно валиден; `LogicalName` совпадает; `ImporterVersion` поддерживается | `Invalid` + заглушка + warning |
| **Source freshness proof** | только proof-плоскость (§3.6) и background audit | embedded `SourceHash`/`AppliedHash` vs `ProjectIndex` / payload | preflight/snapshot/export — error; save — warning |

Preview никогда не сравнивает receipt с Source Root.

### 3.5 `MHMaterializeLayout`

```
FMHMaterializeResult MHMaterializeLayout(
    const FMHCompiledRecipe&, int32 Seed, int32 AppearanceSeed,
    const FTransform& ActorTransform, const FMHNodeOverrideSet* Overrides /*R6+*/)
  -> TArray<FMHLeafPlacement{ Kind; ResourceKey; NodePath; NodeFingerprint;
                              FMatrix WorldMatrix; FMHAppearanceChannels; bOverridden }>
  + Warnings
```

Чистая функция: не грузит, не спавнит, не читает мир, не считает хэши.

### 3.6 Точки выхода (proof plane)

1. `PreSaveWorld` **читает** background proof cache (ключ: `RecipeRevision`
   root'а, generation индекса, `ImporterVersion`, `Registry.Revision`) и
   выводит warning по состоянию `Fresh | Stale | Missing | ProofPending |
   Unknown`. Сам proof в `PreSaveWorld` не строится. Синхронно дождаться
   полного proof имеют право только build preflight и runtime snapshot
   admission (явные действия пользователя).
2. Build preflight (`MHCompositeBuildPreflight*`) — error, блокирует.
3. Runtime snapshot (`MHRuntimeCompositeInput` admission) — error.
4. Export / `UMHCompositeLevelSubsystem::Build` (preflight) — error.

Только здесь строится full closure и читаются `SourceHash`/`AppliedHash`.

`Break` — **не** proof boundary, а операция preview-плоскости
(`docs/16_recipe_model.md` §2.6): она читает резидентный план и снимает ровно
один слой рецепта, не строит full closure и не делает tag-запросов Asset
Registry.

### 3.7 `NodeOverrides` (R6-O — опциональный последний срез семьи R6; контекст, публикация и уникальность — R6-D0…R6-U, docs/16 §2.7)

Слой per-instance переопределений локального трансформа узла — основа для
физической симуляции и автосборки (производители overrides, вне программы).

- Ключ: `FMHNodeOverrideKey { FString NodePath; uint64 NodeFingerprint }`.
  Индексный `NodePath` сдвигается при вставке sibling, поэтому путь без
  fingerprint не идентифицирует узел.

```
NodeFingerprint = Hash(
    semantic kind,
    resource key,
    own authored local transform (canonical),
    relevant semantic node data (options set, profile name),
    ParentSemanticFingerprint )
ParentSemanticFingerprint = Hash(kind, resource key, structural role, его ParentSemanticFingerprint)
                             — без authored transform родителя
```

- `DisplayName` **не** входит (presentation-only); рядом хранятся
  `ExpectedLabel/CurrentLabel` для диагностики.
- Canonical-представление трансформа: нормализованный знак кватерниона,
  `-0.0 → 0.0`, фиксированная сериализация float, порядок компонентов, запрет
  NaN/Inf. Raw-байты `FTransform` не хэшируются.
- Правило: изменился authored transform **самого** узла → override не
  применяется, `MH_W_ORPHAN_OVERRIDE_IDENTITY_CHANGED`; изменился transform
  **предка** → override валиден и движется с предком; сменился семантический
  родитель → override не применяется.
- Применение: путь найден и fingerprint совпал → override заменяет локальный
  трансформ (после генерации, до умножения на родителя). Путь найден, fingerprint
  не совпал → **не применять**, `MH_W_ORPHAN_OVERRIDE_IDENTITY_CHANGED`. Путь не
  найден → `MH_W_ORPHAN_OVERRIDE`. Orphan-записи сохраняются, не удаляются молча.
- Жизненный цикл (решение owner): override — рабочий слой; штатный финал —
  `Promote to composite` (новый `.composite` через экспортный путь level
  subsystem, актор переключается на него, overrides очищаются). Консервативный
  fingerprint принят: правка authored-трансформа **самого** узла гасит override
  с warning; сдвиг предка override не гасит — он движется с предком.
- Операции: `Reset override`, `Reset all`, `Promote to composite…`.

### 3.8 `UMHInstancePoolSubsystem` (World subsystem, Editor)

- **Домен пула**: `FPoolDomainKey { UWorld*; ULevel* OwningLevel;
  FISMComponentDescriptor; AppearanceCustomDataBaseIndex }`. Первая реализация —
  один служебный `AMHInstancePoolActor` (transient) на `ULevel`. World Partition
  cell и Data Layers — отдельное доказательство после полевого теста (OPEN-R-5).
- **Стабильные хэндлы**: `FMHInstanceHandle { BucketId; SlotId; Generation }`.
  Бакет держит `SlotId → ISM index` и `ISM index → SlotId`; при swap-remove в
  `RemoveInstance` обе карты обновляются. `(Component*, InstanceIndex)` наружу
  не выдаётся никогда.
- API: `Add/Update/Remove(Handle)`, `ReverseLookup(Component, ISMIndex) →
  (Actor, NodePath)`, `BeginBulk()/EndBulk()` (один `MarkRenderStateDirty` и
  один physics/nav refresh на скоуп), групповые операции по owner:
  `HideOwner/ShowOwner/RemoveOwner/MoveOwner/SetOwnerEditorVisibility` —
  `SetVisibility()` на ISM-компоненте для этого **непригоден**.
- **Reconcile по пяти хэшам/ревизиям** (§4): только `PayloadRevision`/
  `BoundsRevision` → render/bounds refresh; `BucketDescriptorHash` → миграция
  бакета; `CollisionInterfaceHash` → recreate physics state;
  `MaterialBindingHash` → reconcile дескриптора материалов.
- Undo: транзакционен только актор композита; пул восстанавливает
  материализацию из его состояния в `PostEditUndo`.

### 3.9 `Actor`-листья (R7, после capability-контракта)

- StaticMesh-листья — пул. Actor-листья — **не пул**: один transient
  editor-preview актор на выбранный лист, только из whitelist, без reuse
  между placements. Флаги preview-актора: `RF_Transient |
  RF_DuplicateTransient`, `bIsEditorOnlyActor = true`, не попадает в cook и
  PIE. Runtime-акторы создаёт runtime-мост отдельно.
- Пулинг конкретного класса — только через явный `IMHCompositePoolableActor`
  (safe reset/reuse), вводится отдельным срезом по потребности.

### 3.10 `AMHCompositeActor`

Сохраняемое состояние:

```
TSoftObjectPtr<UMHCompositeAsset> CompositeAsset;
int32 Seed; bool bAutoSeed;
int32 AppearanceSeed; bool bAutoAppearanceSeed;   // семантика как сейчас
FMHNodeOverrideSet NodeOverrides;                  // с R6
```

Транзиентное: хэндлы, `LastPlacements` (Outliner, reseed-diff),
`LastWarnings`. **Перестают быть состоянием актора**: `ResolvedSignature`,
`CompactResolvedState` как гейт, `PlacementDependencies`, `AppliedGraph`,
`AppliedDefinition`, любая логика «подпись устарела → rebuild» и «карта
обязана построить proof до первого кадра».

## 4. Протокол обновлений

| Событие | Действие | Не делать |
|---|---|---|
| Реимпорт `.composite` | перекомпилировать рецепт **только этого** ассета, `Revision++`; инвалидировать `SeedAffectsResult` upstream; rematerialize placements, содержащих его invocation (первая реализация — полный rematerialize их root; целевая — только subtree) | перекомпилировать родительские рецепты; трогать акторов других рецептов |
| Реимпорт меша in place, изменились только `PayloadRevision` и/или `BoundsRevision` | render/bounds refresh бакетов, без миграции и без rebuild | `Materialize`, rebuild актора, перекомпиляция рецептов |
| Реимпорт меша, изменился `BucketDescriptorHash` / `CollisionInterfaceHash` / `MaterialBindingHash` | миграция бакета / recreate physics / reconcile материалов только у бакетов этого меша | полный rebuild |
| Texture payload reimport in place | ничего в пулах | — |
| MI-параметры (scalar/vector/texture) изменились in place | ничего в пулах | — |
| Material object identity / slot binding изменились | reconcile дескриптора затронутых бакетов | rebuild актора |
| Physical material mapping изменился | reconcile collision/trace-интерфейса затронутых бакетов | — |
| Меш появился (был `Invalid`/`Loading`) | прототип → `Ready`; перенос инстансов с заглушки | rebuild актора |
| Смена `Seed`, `SeedAffectsResult == None` | сохранить значение; layout, appearance и хэндлы не трогать | — |
| Смена `Seed`, `SeedAffectsResult == ChildSeedsOnly` | обновить только endpoint'ы, реально потребляющие layout-сид (вложенные рецепты с `bGenerated`) | — |
| Смена `Seed`, `SeedAffectsResult == Transform` | пересчитать трансформы, `Update` хэндлов | — |
| Смена `Seed`, `SeedAffectsResult == Topology` | Layout + diff add/update/remove | пересоздавать неизменившиеся хэндлы |
| Смена `AppearanceSeed` | обновить только appearance-каналы; `bAutoAppearanceSeed` = «сгенерировать один раз при создании/дублировании», не «вычислять из позиции» | — |
| Перемещение актора (вне драга) | `Update(WorldMatrix)` по хэндлам | `Materialize` |
| **Драг гизмо** | каждый кадр: `Update` трансформов инстансов в `BeginBulk/EndBulk`, без collision/nav/snapping, без per-instance `MarkRenderStateDirty`; на `bFinished`: один physics/nav refresh, snapping, bounds | замораживать визуальное движение до отпускания |
| Изменение `NodeOverrides` | Layout + diff по затронутым поддеревьям | — |
| Загрузка карты | `PostRegisterAllComponents` → Layout с заглушками для `Loading`; ноль синхронных `LoadObject` мешей, ноль `FinishCompilation`, ноль proof | — |

## 5. Программа срезов

Каждый срез: ветка `recipe/<id>-<slug>` от свежего `origin/main`, один PR,
квитанция `docs/receipts/recipe_<id>.md`, red-first, гейты §9. Квитанция среза
обновляет `docs/RECIPE_EXECUTION_STATUS.md`. Следующий — только
после owner merge. Ограничения owner: ≤7 acceptance criteria, ≤6 изменяемых
файлов (удаления и тесты отдельно), 30-секундный read-aloud тест. Не влезает —
дели.

| Срез | Содержание | Главный red-assert | Тесты |
|---|---|---|---|
| **D0a** | ADR `docs/16_recipe_model.md` (Status: PROPOSED → NORMATIVE после R2b), `docs/NORMATIVE_INDEX.md`, архив HISTORY-документов, status-заголовки, CI-проверки §7 | CI-проверки §7 зелёные; ни один active doc не утверждает «freshness актора определяется подписью» / «карта строит proof до кадра» | кода нет |
| **M0** | инструментация (готово в `codex/m0-perf-instrumentation`, ждёт merge) | — | добавить `registry_lookups`, `package_loads`, `identity_admissions` |
| **R0** | `UMHEndpointPrototypeRegistry` с identity-admission; `MHLoadAppliedResource` и `MHResolveCompositeDefinitionEndpoint` заменены | 2 актора одного ассета: `registry_lookups == uniqueKeys`; `GetAssets`=0; `FAssetData(&Object)`=0 в preview | новый `PrototypeRegistryIdentityAdmissionTest`; `AppliedPlanAdmissionTest` остаётся до R2c |
| **R1** | переходный: граф без тяжёлых loads, resolve, загрузить и ждать **только выбранные** меши | `waited_mesh_set == selected_compiling_mesh_set`; `waited_mesh_set ∩ unselected_mesh_set == ∅`; `all_option_unique_meshes / selected_unique_meshes` — метрика квитанции, не условие | — (полное снятие ожидания — R4) |
| **S0–S2** | параллельно, §6 | — | свои |
| **R2a** | `FMHCompiledRecipe` (сырые веса, canonical TRS, ссылки), фазовый resolver §3.3, **shadow parity** как diagnostic command + CI-гейт; preview-путь ещё не production | parity на всех golden-векторах = 0 расхождений | `RecipeShadowParityTest` |
| **R2b** | preview переключается на `MHMaterializeLayout`; актор — состояние §3.10; proof остаётся в §3.6 | `PostEditMove` не вызывает Layout; загрузка карты не вызывает Proof | старые тесты подписей на акторе удаляются **только** после зелёных `PrototypeRegistryIdentityAdmissionTest` + `BuildPreflightFullClosureTest` |
| **R2c** | точки выхода §3.6 с freshness-proof; `AppliedPlanAdmissionTest` мигрирует в proof | stale receipt: save → warning, preflight → error, snapshot → reject | `BuildPreflightFullClosureTest` |
| **R3** | семантический reconcile §4 по пяти хэшам/ревизиям П4: mesh, material, texture, phmat, child recipe (без перекомпиляции родителей) | targeted reimport меша с тем же интерфейсом: `actor_rebuild_ms_total == 0`, `recipes_recompiled == 0`; изменение child-рецепта: `parent_recipes_recompiled == 0` | `ResourceReconcileTest` |
| **R4** | async-загрузка выбранных endpoint'ов, заглушки; **снятие остатка ожидания из R1** | cold DDC: `package_loads_sync == 0`, `wait_static_mesh_compilation_ms == 0`; первый кадр раньше готовности мешей | — |
| **R5** | `UMHInstancePoolSubsystem` по домену `ULevel`, стабильные хэндлы, owner-операции видимости, Outliner через `ReverseLookup` | remove одного инстанса не ломает `ReverseLookup` остальных (swap-remove тест); `HideOwner` скрывает только инстансы актора; undo восстанавливает | `InstancePoolHandleStabilityTest` |
| **R5-F** | Lifecycle-фиксы по аудиту: sync rows/leaf-array после миграции бакета, Undo во время Edit завершает сессию, `MigrateBucket` сохраняет placement policy, pool-актор виден в Game View | после миграции бакета во время активного Edit Contents строки компилятора и leaf-array пула не расходятся; Undo, начатый во время Edit, закрывает сессию, а не оставляет открытый невалидный draft | `Pool.RowsSyncAfterBucketMigration`, `Edit.UndoDuringEditEndsSession` |
| **R5b-2** | Selection-seam вьюпорта: клик по инстансу пула → `ReverseLookup` → выделяется owner-композит; работает при закрытом Composite Outliner; highlight только инстансов owner'а; F — по bounds owner'а; Delete/Duplicate/gizmo действуют на root | клик по ISM-инстансу без открытого Outliner выделяет owner `AMHCompositeActor`, не pool-актор; F фокусирует камеру на bounds owner'а, не всего бакета | `Viewport.ClickSelectsOwnerViaReverseLookup`, `Viewport.FFocusesOwnerBounds` |
| **R6-D0** | Контекст вложенного редактирования: root actor → Edit Contents → вложенный вызов → draft подкомпозита с root-размещением и effective transform родителя как контекстом; UI «Редактируется / Контекст / Сохранение»; Cancel не меняет источник; правятся логические узлы, не ISM-инстансы пула | открытие draft вложенного вызова не меняет исходный `.composite` до явного сохранения; Cancel восстанавливает состояние без побочных правок источника | `Edit.NestedLeafRoundTrip`, `Edit.CancelLeavesSourceUnchanged` |
| **R6-D1** | Gizmo вложенного узла относительно effective transform непосредственного родителя (`World = LocalEffective * ParentEffectiveWorld`), проверка представимости до `FTransform`, multiselect родитель+потомок — один раз, одна Undo-транзакция на drag | drag вложенного узла при одновременно выбранных родителе и потомке применяет перемещение ровно один раз; transform-only drag не вызывает `RemoveOwner`/`Add` всего композита | `Gizmo.NestedNodeSingleApplyWithParentSelected`, `Gizmo.TransformOnlyDragSkipsOwnerRebuild` |
| **R6-D2** | Публикация общего определения (Apply Shared Definition): bytes → проверка → атомарная запись в Source Root → импорт → targeted refresh всех потребителей; ошибка записи/импорта сохраняет исходное состояние | неудачная запись/импорт не меняет резидентное состояние ни одного потребителя; успешная публикация обновляет всех потребителей без полного rebuild карты | `Publish.FailedWriteLeavesConsumersUnchanged`, `Publish.TargetedRefreshAllConsumers` |
| **R6-U** | Save Unique / Make Unique с явной областью: Edit Shared Definition / Make Child Unique in This Definition / Make Unique for This Placement (copy-on-write до root); различие procedural variant vs Bake Current Result; `CallContext` при переименовании/копировании сохраняется или явно запекается | Make Unique for This Placement не меняет соседние размещения того же общего определения; совпадение `Seed` без сохранённого/запечённого `CallContext` недостаточно для идентичного воспроизведения | `Promote.NestedOccurrenceIsolation`, `MakeUnique.SeedAloneInsufficientWithoutCallContext` |
| **R6-O** (опц., последний в семье R6) | Persistent instance overrides (бывший `NodeOverrides`): устойчивая identity (`NodeUID` + occurrence address), состояния Applied/Inactive/Orphan/Conflict, parity Preview/Proof/Export/Runtime, wire-формат v6 с планом миграции | два структурно идентичных sibling-узла получают разный устойчивый `NodeUID` и не путают чужие overrides; правка authored transform самого узла даёт Conflict, движение предка — нет | `NodeOverride.RepeatedIdenticalSiblingIdentity`, `NodeOverride.AncestorMoveDoesNotConflict` |
| **R7-0** | Capability-контракт actor-листьев: поддержанные классы и их preview lifecycle, разрешённые параметры, appearance/seed inputs; runtime backend (OPEN-R-8) и World Partition (OPEN-R-5) — отдельные контракты, не внутри R7 | класс вне capability-контракта → `Invalid` + заглушка, как сейчас whitelist | `Capability.ClassOutsideContractFallsBackToPlaceholder` |
| **R7-1** | Миграция существующего `UChildActorComponent`/`TryLoadClass` пути компилятора на контракт R7-0; `gameobj` placeholder не становится исполняемым actor автоматически; без generic сериализации всех Blueprint properties; без автопулинга actor-листьев | миграция не меняет наблюдаемое поведение существующих actor-листьев вне контракта; `gameobj`-заглушка остаётся заглушкой без явного маппинга класса | `Migrate.ChildActorPathPreservesBehavior` |
| **R8** (опц.) | нормализация весов / схлопывание матриц — только после замеров (Owner→handles index, Component→bucket index, реальные physics/nav-затраты, аллокации графа/плана в `MHMaterializeLayout`); не первый кандидат | exhaustive parity = 0 | расширение `RecipeShadowParityTest` |

После R6 — физическая симуляция и автосборка по объёмам как производители
`FMHNodeOverrideSet`-транзакций (вход `LastPlacements`, выход — транзакция в
overrides); ядро не различает источник override; проектируются отдельным
документом.

## 6. Линия S (Source-конвейер) — параллельно, не блокирует R

- **S0** индекс инкрементальный: `(size, mtime)` из `ResourceCandidates` как
  фильтр, хэш — при несовпадении или по флагу-подтверждению; один проход снапшота.
- **S1** FBX в скане **не парсится никогда**: зависимости mesh→material — из
  receipt импорта и предыдущего индекса; парс только при импорте.
- **S2** targeted reimport не делает `FullScan`; `FullScan` — явная команда и
  пересоздание БД.

Формат индекса, receipt'ы, hash-домен не меняются; иначе — OPEN.

## 7. Документальная политика (решение owner 2026-09-02)

Проверяется **нормативный статус**, не лексика.

1. Каждый active документ начинается с
   `Status: NORMATIVE · Architecture version: Recipe Model v2 · Supersedes: …`
   и перечислен в `docs/NORMATIVE_INDEX.md`. Каждый архивный —
   `Status: HISTORY · Do not use for implementation · Superseded by
   docs/16_recipe_model.md` и лежит в `docs/archive/`. `docs/receipts/*` —
   история исполнения, не норматив (README говорит это явно).
2. CI-проверки (скрипт в `tools/`, гейт каждого PR): все `docs/*.md` вне
   `archive/` и `receipts/` есть в `NORMATIVE_INDEX.md`; все файлы в
   `archive/` имеют HISTORY-заголовок; active-документы не ссылаются на
   `archive/` как на норматив; PR не добавляет нормативных требований в
   `receipts/`.
3. Запрещённые **утверждения** в active-документах (проверяет ревью D0a и
   каждого среза): «freshness/placement актора определяется
   `ResolvedSignature`», «карта обязана построить full closure proof до
   первого кадра», «реимпорт меша инвалидирует definition и требует rebuild
   актора». Сами термины `ClosureHash`, `ResolvedSignature` **разрешены** —
   это proof-артефакты.
4. Лексический ноль (grep-гейт) — только для **удалённых сущностей кода**,
   список в `docs/16_recipe_model.md`, стартово: `MHLoadAppliedResource`,
   `AppliedPlanReceipt` (на живом объекте), `FinalizeDeferredMeshes`,
   `PlacementDependencies`, `FMHCompositeDefinitionKey`, `MHValidateAppliedCompositeRoot`;
   восьмая строка (D0b, документ) — единое поле хэша интерфейса меша,
   разделённое П4 на пять хэшей/ревизий (список — `docs/16_recipe_model.md` §7.2).
5. Старый тест удаляется **только** после зелёного replacement-теста,
   названного в квитанции; каждое удаление — отдельной строкой в квитанции.
6. Никаких параллельных «старый/новый production-путь» дольше одного среза.
   Reference resolver в proof-плоскости и preview-исполнитель — не два
   production-пути, а две плоскости с постоянным parity-гейтом.

## 8. D0a — первый срез

Acceptance:

1. `docs/16_recipe_model.md`: §0–§4 этого документа как ADR, `Status: PROPOSED`,
   список удалённых сущностей кода, список запрещённых утверждений, OPEN-вопросы §10.
2. `docs/NORMATIVE_INDEX.md` создан; `README.md` описывает три плоскости и
   политику §7.
3. `docs/10`, `11`, `12_*`, `14`, `15`: разделы про wire-формат, сиды,
   runtime-мост либо остаются в переписанном документе со status-заголовком,
   либо перенесены в 16; всё остальное — в `docs/archive/` с HISTORY-заголовком.
4. `KICKOFF_PROMPT.md` заменён этим текстом; v1 — в `docs/archive/`.
5. Внешний аудит положен в `docs/reference_notes/external_audit_recipe_model_20260902.md`.
6. CI-скрипт §7.2 добавлен и зелёный.
7. Квитанция `docs/receipts/recipe_d0a.md` с таблицей «документ → действие → почему».

Кода нет. Ветка `recipe/d0a-docs`.

## 9. Гейты каждого C++-среза

- Guarded UE build; StrictIncludes non-unity/no-PCH; force-unity adaptive-off;
  `git diff --check`.
- Полный NullRHI `Automation Mimir` с `-MHGoldenRoot=<repo>/golden`: 0 failed;
  число тестов уменьшается только по §7.5.
- Red-first: red-лог до реализации, green после, строки в квитанции.
- С R2a — `RecipeShadowParityTest` обязателен и зелёный в каждом последующем срезе.
- `mh.PerfTrace 1`: `MH_PERF_MAPLOAD`, `MH_PERF_REIMPORT` до/после на host
  исполнителя; owner — полевой замер по протоколу M0 §6.
- Собственный host; audit-host и portfolio-проект owner не трогать.
- Документальные CI-проверки §7.2 зелёные.

## 10. OPEN-вопросы (fail-closed правило до ответа owner)

- **OPEN-R-1 Undo для пула.** Правило: транзакционен только актор; пул
  восстанавливается в `PostEditUndo`.
- **OPEN-R-2 Заглушка.** Правило: `/Engine/BasicShapes/Cube`, настраивается в
  `UMHCompositeSettings::PlaceholderMesh`.
- **OPEN-R-3 Actor-листья и runtime-снапшот.** Правило: участвуют как сейчас
  `ActorClassRegistry`; runtime-мост не расширяется до отдельного решения.
- **OPEN-R-4 Ключ дескриптора.** Правило: `FISMComponentDescriptor` +
  `AppearanceCustomDataBaseIndex`; кастомный ключ — только по доказанному тестом случаю.
- **OPEN-R-5 Домен пула для World Partition / Data Layers.** Правило: пул на
  `ULevel`; WP-cell и Data Layers — после полевого теста R5, отдельный срез.
- **OPEN-R-6 Resolver и хэши замыкания.** Статус: закрыт 2026-09-02 (D0b П1).
  Фазовое разделение обязательно (проверено по коду `MHRandomStream.cpp:809`).
- **OPEN-R-7 Duplicate claim в preview.** Статус: закрыт 2026-09-02 (D0b П2).
  Preview не делает tag-запросов Asset Registry, в том числе для обнаружения
  duplicate-claim; дубликат обнаруживают source-плоскость (`duplicate_claim`
  в индексе) и build preflight (`MH_E_AMBIGUOUS_GENERATED_ASSET`); тест
  `DuplicateRootClaimBlocksPlanAndBreak` мигрирует в preflight-тест в R2c
  (до R2c остаётся, помечен `@migrate:R2c`).

Новую семантику не угадывай. Реальную дыру оформляй как `OPEN-R-<N>`:
Контекст → Вопрос → Временное fail-closed правило → Статус; затронутая часть
STOP до ответа owner.

**Правило DECIDED / STOP для внешних исполнителей (owner 2026-09-05, чтобы
сократить круги; исполнителю есть доверие).** Исполнитель **решает на месте**
и пишет в контракт среза раздел `DECIDED-<срез>-N` (что противоречило, какой
вариант выбран, какой отвергнут и почему), если решение одновременно:
(а) лежит внутри закрытого списка файлов среза; (б) не трогает тесты,
публичный API, resolver, нормативные документы (`docs/16_recipe_model.md`,
KICKOFF) и коды диагностик; (в) при противоречии внутри контракта выбирает
вариант, согласный с red-тестом — тест остаётся нормой. Ревьювер (близнец)
принимает или откатывает `DECIDED` на ревью; откат — возврат PR одной строкой.
`STOP + OPEN` остаётся только для остального: нужна правка теста или API,
ломается существующий тест, нужен новый код `MH_E_*`/`MH_W_*`, решение задевает
норматив.

## 11. Инварианты Dagor, которые нельзя потерять (шпаргалка)

Из `compositMgrService.cpp` DagorEngine @ `7572366`; переносится **принцип**
(плоские массивы, целочисленные индексы, хэндлы ресурсов, непрерывные
диапазоны поддеревьев), а не структуры буквально, и **не** поведение rendInst
на произвольные UE-акторы.

1. Рецепт компилируется один раз на ассет; инстанс — несколько int и срез хэндлов.
2. `selectEnt` тратит один шаг ПСЧ даже на узле с одним вариантом — паритет закреплён в `MHRandomStream`.
3. Сид вложенного композита передаётся **до** построения поддерева.
4. Реимпорт листа подменяет ресурс под указателями; композит не участвует.
5. Изменение child-рецепта пересобирает только его инстансы; родители не перекомпилируются.
6. Ненайденный лист — заглушка; уровень сохраняется без потерь.
7. Во время драга трансформы обновляются, откладываются только тяжёлые побочные эффекты.
8. Layout-сид и appearance-сид — разные сущности (в Mimir оба явные).
9. Экспорт и рендер работают с плоскими пулами; композит-обёртки в финальном игровом представлении нет.
