> Status: NORMATIVE · Architecture version: Recipe Model v2.1 · Supersedes: docs/archive/14_v5_ue_editor_program.md, docs/archive/12_v5_s6_2_s6_3_slices.md (§§3–6), docs/archive/proposals/shared_composite_preview_cache.md · Document status: NORMATIVE · Target architecture rollout: TRANSITIONAL · Production cutover milestone: R2b

# 16 — ADR: модель «рецепт + исполнитель» (Recipe Model v2)

Ратифицировано owner 2026-09-02 после внешнего аудита
(`docs/reference_notes/external_audit_recipe_model_20260902.md`; его 12
требований инкорпорированы в `KICKOFF_PROMPT.md` v2 и сюда; при расхождении —
KICKOFF). Активные документы перечислены в `docs/NORMATIVE_INDEX.md`.

ADR фиксирует **направление архитектуры** редакторского слоя
`ue/MimirComposite` (`Source/MimirCompositeEditor/.../Composite/*`,
`Performance/*`, `Settings/MHCompositeSettings.h`, фазовое разделение resolver
в `MimirCompositeRuntime`). До среза R2b в коде живёт прежняя реализация
размещений; расхождение кода с этим документом — ожидаемое переходное
состояние, закрываемое срезами, а не основание восстанавливать старую модель.
Wire-формат, identity, индекс, receipt на ассетах, сиды и runtime-мост заданы
в `docs/10_source_protocol_v5_plan.md` и здесь только адресуются (§5).

## 0. Формула и три плоскости

> Dagor-подобный быстрый preview-исполнитель + Mimir-подобный строгий proof на
> границах.

| Плоскость | Что делает | Что ей запрещено |
|---|---|---|
| **Preview** | компиляция рецепта, выбор по сидам, материализация в пулы, заглушки, async-загрузка | хэши источников, full-closure proof, ожидание компиляции, чтение Asset Registry тегов |
| **Proof** | full closure, receipt freshness, `ClosureHash`/`ResolvedSignature`, admission runtime-снапшота, build preflight, export | блокировать загрузку карты или preview |
| **Source** | инкрементальный индекс файлов, targeted reimport, background freshness | парсить FBX в скане, делать FullScan на targeted reimport |

Подписи и хэши замыкания **остаются proof-артефактами** (resolver, golden,
runtime admission, preflight, export) и **перестают быть состоянием актора**.

```text
Рецепт компилируется один раз на ассет; мешей при компиляции не грузит.
Инстанс хранит только (asset, Seed, AppearanceSeed, transform, [NodeOverrides c R6]).
Материализация preview — чистая функция: не грузит, не спавнит, не читает мир, не считает хэши.
Endpoint резолвится по детерминированному пути; identity-admission один раз на ключ за сессию.
Ненайденный/незагруженный endpoint — заглушка и warning, не ошибка актора.
Freshness receipt (SourceHash/AppliedHash vs индекс) — только в точках выхода.
Реимпорт меша с тем же интерфейсом композит не трогает; child-рецепт не перекомпилирует родителей.
Рендер, селекшн, экспорт работают с плоскими пулами по домену ULevel и композит не видят.
```

Архитектурный порядок задаёт KICKOFF. Фактическую точку продолжения задаёт
`docs/RECIPE_EXECUTION_STATUS.md`; перед началом любого среза исполнитель
читает его и начинает **только** срез со статусом NEXT/READY.

## 1. Что остаётся неизменным

| Область | Файлы | Причина |
|---|---|---|
| Wire-формат `.composite` v5, `.placement`, канонизация, хэши payload | `Composite/MHCompositeProtocol.*`, `Source/MHPayloadHashes.*`, `Runtime/Canonical/*` | формат заморожен, Blender пишет его |
| Сиды и reference resolver | `Runtime/Random/MHRandomStream.*`, `MHResolveCompositePlan`, `MHBuildRandomSourceClosure` | паритет с Dagor доказан golden; допускается **только** фазовое разделение §2.3 без изменения выборок и математики |
| Proof-артефакты | `ClosureHash`, `ResolvedSignature`, runtime-admission, build preflight, export | остаются как доказательство; перестают быть состоянием актора |
| Runtime-мост | `Runtime/Composite/MHRuntimeCompositeActor.*`, `MHRuntimeCompositeInput.*`, `Editor/Composite/MHCompositeRuntimeBridge.*` | точка выхода |
| Receipt на ассетах | `UMHStaticMeshImportData`, `UMHMaterialSourceData`, `UMHTextureSourceData`, теги `MH.*` при записи | меняется только **где** и **что** из receipt читается (§2.4) |
| Source-конвейер | `Source/*`, `Index/*`, `StaticMesh/*`, `Material/*`, `Texture/*`, `Geometry/*`, `Diagnostics/*` | линия S (KICKOFF §6) |
| Семантика сидов актора | `Seed` (layout), `AppearanceSeed` (явный, сохраняемый, **не зависит от позиции**) | решение owner 2026-09-02; Dagor position-derived instSeed — opt-in политика вне программы |
| Blender-аддон, `golden/`, `reference/` | — | вне scope; `golden/` только по явному пункту среза |

## 2. Целевая архитектура

```text
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

### 2.1 `FMHCompiledRecipe` (Editor, `Composite/MHCompiledRecipe.{h,cpp}`)

Реестр — editor subsystem; ключ `UMHCompositeAsset* + RecipeRevision`
(`uint32`, инкремент при реимпорте/`PostEditChange` ассета); `AppliedHash`
ассета хранится в записи как debug-атрибут и **никогда** не сравнивается с
Source Root в preview. Компиляция не грузит ни одного меша: только
`MHExtractCompositeV5` и ключи ресурсов.

- `TArray<FComponent>` в DFS-порядке; иерархия интервалами `BeginInd/EndInd`;
  обход линейный со стеком родительских матриц.
- `FComponent { NodePath; NodeFingerprint (§2.7); Options[{Kind, ResourceKey,
  WeightRaw}]; TransformKind (Matrix | Ranges); canonical TRS как в источнике;
  bAppearanceSeedBoundary; ProfileName }`.
- **Веса хранятся сырыми, TRS — как в источнике.** Нормализация весов и
  схлопывание узлов без девиаций в матрицу — только после exhaustive parity
  (§2.3, гейт KICKOFF §9) отдельным срезом R8; до этого запрещены.
- `bGenerated`; `SeedAffectsResult` — кэш с generation-стампом, инвалидируется
  глобальным счётчиком при любой перекомпиляции любого рецепта.
- Вложенный композит — **ссылка** на скомпилированный рецепт дочернего ассета
  по хэндлу. Цикл — по стеку компиляции, `MH_E_COMPOSITE_CYCLE`.
- Обратный индекс `Dependents: ResourceKey → {recipes}` — только для
  локализации rematerialize (§4), **не** для перекомпиляции родителей.

### 2.2 `UMHEndpointPrototypeRegistry` (Editor subsystem)

`FMHResourceKey → FMHEndpointPrototype { TWeakObjectPtr<UObject> Object;
EState {Unresolved, Loading, Ready, Invalid}; FBox Bounds; FString
AdmissionError; uint32 Revision; uint32 PayloadRevision; uint32 BoundsRevision;
uint64 BucketDescriptorHash; uint64 CollisionInterfaceHash;
uint64 MaterialBindingHash; TSoftClassPtr<AActor> ActorClass }`.

- Резолв меша **только** по детерминированному пути
  `/Game/MH/Generated/<Folder>/<LogicalName>.<LogicalName>` (10 §8) через
  `FSoftObjectPath`. **Запрещено** в preview-плоскости: `IAssetRegistry::
  GetAssets` с tag-фильтром, `GetAssetsByTags`, `FAssetData(&Object)`,
  чтение `GetAssetRegistryTags` живого объекта, `FinishCompilation`. Теги
  `MH.*` — проекция receipt для индекса (10 §7), не механизм резолва.
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

### 2.3 Resolver: фазовое разделение (Runtime, единственная правка resolver)

Существующий `MHResolveCompositePlan(...)` становится обёрткой над тремя
фазами с **неизменной** последовательностью выборок и математикой:

```text
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
preview-путём. Расхождение = red. Selected dependencies сравниваются по ресурсам
графа (composite / static_mesh / placement_profile / actor): preview-плоскость
не грузит меши и не знает расширений mesh → material → texture; сам список —
proof-артефакт, preview его не потребляет (OPEN-R2A-1, закрыт owner 2026-09-02).

### 2.4 Два уровня admission endpoint'а

| Уровень | Где | Проверяет | При провале |
|---|---|---|---|
| **Identity admission** | реестр, один раз на ключ за сессию + при `Revision++` | объект по каноническому пути существует; embedded receipt есть и структурно валиден; `LogicalName` совпадает; `ImporterVersion` поддерживается | `Invalid` + заглушка + warning |
| **Source freshness proof** | только proof-плоскость (§2.6) и background audit | embedded `SourceHash`/`AppliedHash` vs `ProjectIndex` / payload | preflight/snapshot/export — error; save — warning |

Preview никогда не сравнивает receipt с Source Root.

### 2.5 `MHMaterializeLayout`

```text
FMHMaterializeResult MHMaterializeLayout(
    const FMHCompiledRecipe&, int32 Seed, int32 AppearanceSeed,
    const FTransform& ActorTransform, const FMHNodeOverrideSet* Overrides /*R6+*/)
  -> TArray<FMHLeafPlacement{ Kind; ResourceKey; NodePath; NodeFingerprint;
                              FMatrix WorldMatrix; FMHAppearanceChannels; bOverridden }>
  + Warnings
```

Чистая функция: не грузит, не спавнит, не читает мир, не считает хэши.

### 2.6 Точки выхода (proof plane)

1. `UMHProofCacheSubsystem` хранит состояния `Unknown | ProofPending | Fresh |
   Stale | Missing`. Ключ: root asset, его `RecipeRevision`, `Seed`,
   `AppearanceSeed`, generation `ProjectIndex`, `ImporterVersion` и ревизии
   endpoint-реестра для полного набора зависимостей. Сдвиг любого поля даёт
   `Unknown`; `RequestProof` ставит работу в game-thread ticker (не более одного
   placement за тик), а `FlushPendingProofs` завершает очередь синхронно по
   явному запросу.
2. `PreSaveWorld` вне cook вызывает только `AuditWorld`: для каждого состояния,
   кроме `Fresh`, пишет в Message Log «Mimir» один из
   `MH_W_PROOF_UNKNOWN`, `MH_W_PROOF_PENDING`, `MH_W_PROOF_STALE`,
   `MH_W_PROOF_MISSING`; для `Unknown` только планирует `RequestProof`.
   Синхронный proof и отказ сохранения карты здесь запрещены.
3. Build/cook preflight (`MHValidateRuntimeCompositeWorld`) и runtime snapshot
   (`MHBuildRuntimeCompositeInput`) вызывают `BuildProofNow` синхронно и
   блокируют выход при `Stale`/`Missing`. Экспорт, который потребляет
   placement, подчиняется тому же правилу.
4. `BuildProofNow` строит applied full closure, проверяет duplicate claims,
   выполняет `Layout → Appearance → Proof` и admission трансформов, затем
   сравнивает receipt-хэши только с `ProjectIndex` (или тестовым provider).
   Неизвестный индексу ключ не делает proof stale; расхождение даёт
   `MH_E_STALE_SOURCE: <key> receipt <hash> differs from source <hash>`.

Preview не читает этот кэш и не строит ни closure, ни подписи, ни source
freshness proof.

### 2.7 `NodeOverrides` (вводится в R6, после R5)

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
  трансформ (после генерации, до умножения на родителя). Путь найден,
  fingerprint не совпал → **не применять**,
  `MH_W_ORPHAN_OVERRIDE_IDENTITY_CHANGED`. Путь не найден →
  `MH_W_ORPHAN_OVERRIDE`. Orphan-записи сохраняются, не удаляются молча.
- Жизненный цикл (решение owner): override — рабочий слой; штатный финал —
  `Promote to composite` (новый `.composite` через экспортный путь level
  subsystem, актор переключается на него, overrides очищаются). Консервативный
  fingerprint принят: правка authored-трансформа **самого** узла гасит override
  с warning; сдвиг предка override не гасит — он движется с предком.
- Операции: `Reset override`, `Reset all`, `Promote to composite…`.

### 2.8 `UMHInstancePoolSubsystem` (World subsystem, Editor)

- **Домен пула**: `FPoolDomainKey { UWorld*; ULevel* OwningLevel;
  FISMComponentDescriptor; AppearanceCustomDataBaseIndex }`. Первая
  реализация — один служебный `AMHInstancePoolActor` (transient) на `ULevel`.
  World Partition cell и Data Layers — отдельное доказательство после полевого
  теста (OPEN-R-5).
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
  материализацию из его состояния в `PostEditUndo` (OPEN-R-1).

### 2.9 `Actor`-листья (R7, после capability-контракта)

- StaticMesh-листья — пул. Actor-листья — **не пул**: один transient
  editor-preview актор на выбранный лист, только из whitelist, без reuse
  между placements. Флаги preview-актора: `RF_Transient |
  RF_DuplicateTransient`, `bIsEditorOnlyActor = true`, не попадает в cook и
  PIE. Runtime-акторы создаёт runtime-мост отдельно.
- Пулинг конкретного класса — только через явный `IMHCompositePoolableActor`
  (safe reset/reuse), вводится отдельным срезом по потребности.

### 2.10 `AMHCompositeActor`

Сохраняемое состояние:

```text
TSoftObjectPtr<UMHCompositeAsset> CompositeAsset;
int32 Seed; bool bAutoSeed;
int32 AppearanceSeed; bool bAutoAppearanceSeed;   // семантика как сейчас (10 §6.9)
FMHNodeOverrideSet NodeOverrides;                  // с R6
```

Транзиентное: хэндлы, `LastPlacements` (Outliner, reseed-diff),
`LastWarnings`. **Перестают быть состоянием актора**: `ResolvedSignature`,
`CompactResolvedState` как гейт, список зависимостей размещения (четвёртая
строка §7.2), `AppliedGraph`, `AppliedDefinition`, любая логика «подпись
устарела → rebuild» и «карта обязана построить proof до первого кадра».

`BreakComposites` — операция preview-плоскости: она читает резидентный план и
снимает ровно один слой рецепта. Вложенные композиты остаются
`AMHCompositeActor` с layout- и appearance-сидами родителя; группы поднимают
детей, random оставляет только выбранную опцию. Plan-view компоненты не
транзакционны: записью Undo служит сам актор, а `PostEditUndo` ретирует
производную материализацию и восстанавливает preview штатным путём из
`CompositeAsset`, сидов и transform.

## 3. Точки выхода и proof — см. §2.4, §2.6

Preview и proof не смешиваются: preview ничего не блокирует и не сравнивает с
Source Root; proof строится только в четырёх точках §2.6 и в background audit.

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

## 5. Wire-формат, сиды, runtime-мост: где норматив

Не меняются программой R; заданы в `docs/10_source_protocol_v5_plan.md`:

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
| NodePath, `ClosureHash`, `ResolvedSignature` (proof-артефакты) | §13.3 |

Runtime-мост в одном абзаце: `FMHRuntimeCompositeInput` = сериализуемый
seed-free граф (`GraphBytes`) + `Bindings` на **все** варианты (включая
невыбранные и zero-weight) и actor-классы. `AMHRuntimeCompositeActor` подаёт
его тому же reference resolver с сидами размещения; PIE = packaged = editor по
плану. Admission снапшота — точка выхода §2.6.3. Ничего нового в runtime-мост
программа R не добавляет (OPEN-R-3).

## 6. Инварианты Dagor, которые нельзя потерять

Из `compositMgrService.cpp` DagorEngine @ `7572366`
(`docs/reference_notes/dagor_composit_research.md`); переносится **принцип**
(плоские массивы, целочисленные индексы, хэндлы ресурсов, непрерывные
диапазоны поддеревьев), а не структуры буквально, и **не** поведение rendInst
на произвольные UE-акторы.

1. Рецепт компилируется один раз на ассет; инстанс — несколько int и срез
   хэндлов.
2. `selectEnt` тратит один шаг ПСЧ даже на узле с одним вариантом — паритет
   закреплён в `MHRandomStream`.
3. Сид вложенного композита передаётся **до** построения поддерева.
4. Реимпорт листа подменяет ресурс под указателями; композит не участвует.
5. Изменение child-рецепта пересобирает только его инстансы; родители не
   перекомпилируются.
6. Ненайденный лист — заглушка; уровень сохраняется без потерь.
7. Во время драга трансформы обновляются, откладываются только тяжёлые
   побочные эффекты.
8. Layout-сид и appearance-сид — разные сущности (в Mimir оба явные).
9. Экспорт и рендер работают с плоскими пулами; композит-обёртки в финальном
   игровом представлении нет.

Срез, нарушающий пункт из этого списка, — OPEN-вопрос (§9), не решение
исполнителя.

## 7. Документальная политика: статусы, запрещённые утверждения, лексический гейт

Проверяется **нормативный статус**, не лексика (KICKOFF §7). Каждый active
документ начинается с `Status: NORMATIVE · Architecture version: Recipe Model
v2 · Supersedes: …` и перечислен в `docs/NORMATIVE_INDEX.md`; каждый
архивный — `Status: HISTORY · Do not use for implementation · Superseded by
docs/16_recipe_model.md` в `docs/archive/`. CI-гейт каждого PR —
`python tools/check_normative_docs.py` (индекс, статус-шапки, отсутствие
ссылок из active на archive как на норматив, receipts без нормативных
требований, лексический гейт ниже).

### 7.1 Запрещённые утверждения в active-документах (проверяет ревью)

1. «freshness/placement актора определяется `ResolvedSignature`» (или любой
   подписью/хэшем на акторе);
2. «карта обязана построить full closure proof до первого кадра»;
3. «реимпорт меша инвалидирует definition и требует rebuild актора»;
4. «preview сравнивает receipt с Source Root» / «загрузка карты ждёт
   компиляцию меша».

Сами термины `ClosureHash`, `ResolvedSignature`, `AppearanceSignature`,
`PlacementSignature` **разрешены** — это proof-артефакты (10 §13.3, §6.9).

### 7.2 Удалённые сущности кода (лексический ноль в active-документах)

Список — единственный источник для CI-гейта; пополняется срезом, который
удаляет сущность (строка «удаляет» — KICKOFF §5). Идентификаторы читаются
скриптом из этого блока:

```removed-entities
MHLoadAppliedResource
AppliedPlanReceipt
FinalizeDeferredMeshes
PlacementDependencies
FMHCompositeDefinitionKey
MHValidateAppliedCompositeRoot
MHResolveCompositeDefinitionEndpoint
PlacementInterfaceHash
```

| Сущность | Что вместо | Удаляет |
|---|---|---|
| скан Asset Registry с tag-фильтром на каждый резолв endpoint'а (первая строка блока) | `UMHEndpointPrototypeRegistry`, детерминированный путь | R0a (реализация → фасад), R0b (символ) |
| receipt из шести тегов через `FAssetData(&Object)` на живом объекте (вторая строка) | identity-admission по `UMHStaticMeshImportData`, один раз на ключ | R0a |
| ожидание компиляции мешей в горячем пути (третья строка) | R1: closure не ждёт, compiler ждёт только выбранные (`waited_meshes`); R4: async + заглушки, ожидания нет | R1 (символ), R4 (остаток ожидания) |
| список зависимостей размещения на акторе (четвёртая строка) | обратный индекс `Dependents` в `FMHCompiledRecipe` | R2b |
| ключ definition-кэша по root + closure (пятая строка) | ключ рецепта = ассет + `AppliedHash`; вложенные по ссылке | R2a |
| валидация applied-root в горячем пути (шестая строка) | proof-плоскость §2.6 | R0c |
| резолв endpoint'а definition-кэшем (седьмая строка) | реестр §2.2 | R0 |
| хэш интерфейса меша одним полем (восьмая строка) | пять хэшей/ревизий П4 | D0b (документ), R3 (код) |

Старый тест удаляется **только** после зелёного replacement-теста, названного в
квитанции (KICKOFF §7.5): `AppliedPlanAdmissionTest` — после
`PrototypeRegistryIdentityAdmissionTest` + `BuildPreflightFullClosureTest`.

## 8. Программа срезов

Порядок, red-asserts, тесты и гейты — `KICKOFF_PROMPT.md` §5, §9; здесь не
дублируются. D0a → M0 → R0 → R1 (переходный: ждать только выбранные меши) →
S0–S2 параллельно → R2a (фазовый resolver + shadow parity, не production) →
R2b (preview на `MHMaterializeLayout`, актор §2.10) → R2c (точки выхода) →
R3 (reconcile по пяти хэшам/ревизиям П4) → R4 (async endpoint'ы) → R5 (пулы
по `ULevel`, стабильные хэндлы) → R6 (`NodeOverrides` + UI) → R7 (actor-листья)
→ R8 опц. (нормализация/схлопывание после exhaustive parity). С R2a
`RecipeShadowParityTest` обязателен в каждом срезе.

Инструментация M0 (`mh.PerfTrace`, `MH_PERF_MAPLOAD` /
`MH_PERF_STARTUP_SCAN` / `MH_PERF_REIMPORT`) смержена в `main` (PR #60);
срез M0 программы v2 добавляет счётчики `registry_lookups`, `package_loads`,
`identity_admissions`. Полевой протокол — `docs/receipts/m0_perf_instrumentation.md` §6.

## 9. OPEN-вопросы (fail-closed правило до ответа owner)

Правило: новую семантику не угадывать; реальная дыра оформляется как
`OPEN-R-<N>` (Контекст → Вопрос → Временное fail-closed правило → Статус),
затронутая часть — STOP до ответа owner.

| # | Вопрос | Временное правило | Статус |
|---|---|---|---|
| OPEN-R-1 | Undo для пула | транзакционен только актор; пул восстанавливается в `PostEditUndo` | открыт |
| OPEN-R-2 | Заглушка | `/Engine/BasicShapes/Cube`, настраивается в `UMHCompositeSettings::PlaceholderMesh` | открыт |
| OPEN-R-3 | Actor-листья и runtime-снапшот | участвуют как сейчас `ActorClassRegistry`; runtime-мост не расширяется до отдельного решения | открыт |
| OPEN-R-4 | Ключ дескриптора | `FISMComponentDescriptor` + `AppearanceCustomDataBaseIndex`; кастомный ключ — только по доказанному тестом случаю | открыт |
| OPEN-R-5 | Домен пула для World Partition / Data Layers | пул на `ULevel`; WP-cell и Data Layers — после полевого теста R5, отдельный срез | открыт |
| OPEN-R-6 | Resolver и хэши замыкания | Фазовое разделение обязательно (проверено по коду `MHRandomStream.cpp:809`). | закрыт 2026-09-02 (D0b П1) |
| OPEN-R2A-1 | `SelectedDependencies` в preview-плане | preview не использует поле (proof-артефакт); shadow parity сравнивает его только по ресурсам графа (composite / static_mesh / placement_profile / actor), без mesh → material → texture | закрыт 2026-09-02 (owner) |
| OPEN-R2B-1 | Гейт удалений proof-состояния актора (R2b-3) | `BuildPreflightFullClosureTest` из KICKOFF §5 R2b = preflight-тест полного closure, который создаёт R2c; до него proof-поля актора (`CompactResolvedState`, `ResolvedSignature`, `AppliedDefinition`) — диагностика без гейтов | закрыт 2026-09-03 (owner делегировал решение близнецу) |
| OPEN-R2B-2 | Резидентный preview-план на актор (§2.10 «LastPlacements») | план хранится целиком (decisions, draws, nodes, leaves): draws нужны Outliner-трассе и reseed-diff; сжатие — только если полевой замер R4 (пулы) покажет проблему памяти | закрыт 2026-09-03 (owner делегировал решение близнецу) |
| OPEN-R-7 | Duplicate claim в preview | Preview-плоскость делает ноль tag-запросов. Duplicate claim обнаруживают source-плоскость (`duplicate_claim`) и `BuildProofNow` в preflight/snapshot (`MH_E_AMBIGUOUS_GENERATED_ASSET`). Старый preview-тест заменён proof-plane тестом `DuplicateClaimIsProofPlane` в R0c. | закрыт D0b П2; реализован в proof-плоскости R2c (2026-09-03) |
| OPEN-R4P-1 | Точное воспроизведение random внутри дочернего композита после Break | ребёнок получает `Seed` и `AppearanceSeed` родителя как есть; из-за смены корневого `NodePath` внутренний random может выбрать другой вариант | открыт, fail-closed: сиды родителя **Решение owner 2026-09-04 (по аудиту `dagor_composit_ue5_audit_20260904.md` §7.A): вариант близнеца — сохраняемый, версионированный контекст вызова на акторе-ребёнке (namespace потока = путь узла в родителе, appearance boundary = корень родителя), необязательный вход resolver; пустой контекст = текущее поведение, старые ассеты не меняются. Срез R4-pre-3 (близнец).** |

| OPEN-R-8 | Runtime-бэкенд для cook/PIE (компонент на лист в `AMHRuntimeCompositeActor`, WP закрыт) | **открыт**; owner 2026-09-04: портфолио — рендер без игрового билда, но плагин станет продуктом для FAB и cook понадобится другим разработчикам; правильный ответ (bake на cook vs shared runtime recipe) пока не известен. До решения runtime-мост не меняется; fail-closed: WP-отказ и покомпонентная материализация остаются как есть | открыт |
| OPEN-S-1 | Источник slot-рёбер mesh→material без парса FBX в скане (S1) | **закрыт 2026-09-02, вариант c (owner):** S0 достаточен — FBX парсится только для новых/изменённых по `(size, mtime)` файлов; S1 закрыт без кода; холодный скан портфолио измеряется полевым протоколом M0 §6 | закрыт |

**OPEN-S-1 — контекст.** KICKOFF §6 S1: «FBX в скане не парсится никогда;
зависимости mesh→material берутся из receipt импорта и из индекса предыдущего
скана». Receipt `UMHStaticMeshImportData` не хранит имена слотов, шесть тегов
Asset Registry их тоже не несут (10 §7: «ровно шесть»), а индекс по 10 §3 —
чистая проекция «скан + теги»: после удаления `.sqlite` рёбра нового/изменённого
меша без парса FBX восстановить неоткуда, и acceptance «удаление .sqlite →
идентичный индекс» ломается. Варианты: (a) седьмой тег/поле receipt с именами
слотов, пишется импортёром, читается индексом без загрузки объекта — меняет
10 §7; (b) рёбра для меша считаются «неизвестны, пока не импортирован», парс
только при импорте, `IsImportBlocked` по slot-рёбрам работает после первого
импорта — ослабляет 10 §3 (`unknown ≠ empty` уже допускает unknown, но не для
`ok`-кандидата); (c) оставить S0-поведение (парс только новых/изменённых FBX)
как достаточное и закрыть S1 без изменений. Вопрос owner: a / b / c.

**OPEN-R-7 — контекст.** Тест
`Mimir.V5.Composite.AppliedAdmission.DuplicateRootClaimBlocksPlanAndBreak` был
заменён в R0c тестом `Mimir.V5.Composite.Registry.DuplicateClaimIsProofPlane`:
preview сохраняет детерминированный канонический endpoint и не делает
tag-запросов. R2c завершил перенос: `BuildProofNow` проверяет claims полного
замыкания, поэтому preflight и snapshot отказывают с
`MH_E_AMBIGUOUS_GENERATED_ASSET`; source-плоскость независимо хранит
`duplicate_claim` в индексе.

Вопрос v1 о судьбе хэша плана резолвера снят: v2 §0 оставляет
`ClosureHash`/`ResolvedSignature` proof-артефактами (не состоянием актора).

## 10. Карта документов

Полный индекс — `docs/NORMATIVE_INDEX.md`. Кратко: норматив —
`KICKOFF_PROMPT.md`, этот ADR, `README.md`, справочник `docs/10`; справочные
исследования — `docs/reference_notes/`; история исполнения —
`docs/receipts/`; HISTORY — `docs/archive/` (00–09, 11–15, ADR, amendments,
аудиты, `QUESTIONS.md`, proposals, spikes, `README_pre_d0.md`,
`KICKOFF_PROMPT_v1_20260902.md`). Квитанция D0a —
`docs/receipts/recipe_d0a.md`.
