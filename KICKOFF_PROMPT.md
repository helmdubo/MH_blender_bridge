# KICKOFF — MimirComposite: переход на модель «рецепт + исполнитель» (Recipe Model)

> **АКТИВНЫЙ ПРОМПТ ИСПОЛНИТЕЛЯ. Заменяет `KICKOFF_PROMPT.md` целиком.**
> Дата ратификации owner: 2026-09-02. Этот документ самодостаточен: сессия,
> которая его читает, не имеет никакого другого контекста. Всё, что здесь не
> написано, — не норматив; всё, что противоречит этому документу в других
> файлах репозитория, — устарело и подлежит удалению (см. §7).

## 0. Кто ты и что делаешь

Ты — исполнитель в роли Principal Technical Artist UE-фронта в
`helmdubo/MH_blender_bridge` (Blender Extension `addon/mh4blend` + UE 5.7.4
plugin `ue/MimirComposite`). Owner (Alexander) единолично принимает и мержит PR;
внешний ai-аудитор получает крупные срезы. Ты не мержишь. Ты не меняешь Engine.

Задача программы: перевести редакторскую часть `ue/MimirComposite` с модели
«актор хранит доказуемо консистентное applied-состояние» на модель Dagor
«**рецепт + исполнитель**»: рецепт компилируется один раз на ассет, инстанс
хранит только `(asset, seed, appearanceSeed, transform, nodeOverrides)`,
материализация — чистая функция, провенанс проверяется только в точках выхода.
Одновременно — расширение листьев до произвольных UE-акторов и слой
per-instance оверрайдов трансформов узлов как основа для физической симуляции
и автосборки.

Это **не** переписывание плагина с нуля. Формат `.composite`, канонизация,
сиды и resolver, runtime-мост, Source-конвейер (импорт/индекс/батч) остаются.
Переписывается редакторский слой `Composite/` и его контракт с актором.

## 1. Обязательное чтение до любых правок

1. Этот документ целиком.
2. `README.md` целиком.
3. `docs/reference_notes/dagor_composit_research.md` — разбор composit в
   Dagor и сопоставление с текущим плагином. **Если файла нет — остановись и
   запроси у owner**, он существует вне репозитория.
4. `docs/receipts/m0_perf_instrumentation.md` — счётчики `mh.PerfTrace`
   и полевой протокол замеров.
5. Исходники, которые ты будешь менять: `ue/MimirComposite/Source/MimirCompositeEditor/
   {Public,Private}/Composite/*`, `Private/Performance/*`,
   `Public/Settings/MHCompositeSettings.h`, тесты в `MimirCompositeTests`.

Документы `docs/10_…`, `docs/11_…`, `docs/12_…`, `docs/14_…`, `docs/15_…` до
среза D0 (§8) считать **историей**: из них берутся только описание wire-формата,
сидов и runtime-моста. Разделы про applied state, подписи, definition cache и
receipt-admission на инстансе — не норматив, даже если написаны в повелительном
наклонении.

## 2. Что остаётся неизменным (не трогать)

| Область | Файлы | Причина |
|---|---|---|
| Wire-формат `.composite` v5, `.placement`, канонизация, хэши payload | `Composite/MHCompositeProtocol.*`, `Source/MHPayloadHashes.*`, `Runtime/Canonical/*` | формат заморожен, Blender пишет его |
| Сиды и resolver | `Runtime/Random/MHRandomStream.*`, `MHResolveCompositePlan`, `MHBuildRandomSourceClosure` | паритет с Dagor доказан golden-тестами |
| Runtime-мост | `Runtime/Composite/MHRuntimeCompositeActor.*`, `MHRuntimeCompositeInput.*`, `Editor/Composite/MHCompositeRuntimeBridge.*` | admission снапшота — это точка выхода, она остаётся |
| Source-конвейер | `Source/*`, `Index/*`, `StaticMesh/*`, `Material/*`, `Texture/*`, `Geometry/*`, `Diagnostics/*` | отдельная линия S (§6), в программу R не входит |
| Receipt на ассетах | `UMHStaticMeshImportData`, `UMHMaterialSourceData`, `UMHTextureSourceData`, теги `MH.*` при записи | receipt остаётся источником истины о провенансе; меняется только **где** он читается |
| Blender-аддон, `golden/`, `reference/` | — | вне scope; `golden/` только по явному пункту среза |

## 3. Целевая архитектура

Пять слоёв. Каждый — отдельный файл(ы), отдельный тест, чистая зависимость
сверху вниз. Никаких обратных ссылок из нижних слоёв в актор.

```
UMHCompositeAsset             рецепт (как сейчас; источник истины — .composite)
FMHCompiledRecipe             скомпилированный рецепт, 1 на ассет               ← Dagor CompositEntityPool.comp
UMHLeafPrototypeRegistry      прототип листа, 1 на FMHResourceKey               ← Dagor VirtualMpEntity + leaf pool
MHMaterialize(...)            чистая функция: рецепт+сиды+tm+overrides → placements
UMHInstancePoolSubsystem      плоские пулы ISM/акторов на мир, выдаёт хэндлы     ← Dagor riExtra pool
AMHCompositeActor             инстанс: Asset, Seed, AppearanceSeed, Transform, NodeOverrides
```

### 3.1 `FMHCompiledRecipe` (Editor, `Composite/MHCompiledRecipe.{h,cpp}`)

Кэш в editor subsystem, ключ — `TWeakObjectPtr<UMHCompositeAsset>` + `AppliedHash`
ассета. Компиляция **не грузит ни одного меша**: только `MHExtractCompositeV5`
и ключи ресурсов.

Содержимое:

- `TArray<FComponent>` в DFS-порядке; иерархия — интервалами `BeginInd/EndInd`
  (как в Dagor `loadAssetData`), обход линейный со стеком родительских матриц.
- `FComponent { NodePath; Options[{Kind, ResourceKey, WeightNormalized}];
  TransformKind (Matrix | Ranges); FMatrix CollapsedTm для узлов без девиаций;
  bAppearanceSeedBoundary; ProfileName }`.
- Веса нормализованы к сумме 1 при компиляции.
- Флаг `bGenerated` (есть узел с >1 варианта или с девиацией).
- `SeedAffectsResult` — кэш с generation-стампом, инвалидируется любой
  перекомпиляцией любого рецепта (глобальный счётчик, как `seedFlagGen` в Dagor).
- Вложенный композит — **ссылка** на скомпилированный рецепт дочернего ассета,
  не инлайн. Цикл — по стеку компиляции, диагностика `MH_E_COMPOSITE_CYCLE`.
- Обратный индекс `Dependents: ResourceKey → {recipes}` — единственный источник
  «кого пересобирать» при изменении ассета.
- Для resolver собирается прежний `FMHRandomSourceGraph` из ссылок; сам
  resolver не меняется. `RawHashes` в графе заполняются из receipt'ов
  прототипов (лениво, только для точек выхода).

### 3.2 `UMHLeafPrototypeRegistry` (Editor subsystem)

`FMHResourceKey → FMHLeafPrototype { TWeakObjectPtr<UObject> Object; EState
{Unresolved, Loading, Ready, Invalid}; FBox Bounds; FString ReceiptError;
uint32 Revision; TSubclassOf<AActor> ActorClass }`.

Правила:

- Резолв меша **только** по детерминированному пути
  `/Game/MH/Generated/<Folder>/<LogicalName>.<LogicalName>` через
  `FSoftObjectPath` / `LoadObject`. **Запрещено**: `IAssetRegistry::GetAssets`
  с tag-фильтром, `GetAssetsByTags`, `FAssetData(&Object)` и любое чтение
  `GetAssetRegistryTags` живого объекта в горячем пути.
- Receipt проверяется **один раз на ключ за сессию** по
  `UMHStaticMeshImportData` (LogicalName, SourceRelativePath, SourceHash) и
  пути объекта. Провал → `Invalid` + `ReceiptError`, объект остаётся в реестре
  как заглушка. Никакого `FinishCompilation`: receipt не зависит от состояния
  компиляции.
- Загрузка асинхронная (`FStreamableManager`), пока `Loading` — прототип
  отдаёт `PlaceholderMesh` из настроек плагина (`UMHCompositeSettings::
  PlaceholderMesh`, по умолчанию движковый куб).
- `Actor`-лист: класс из `UMHCompositeSettings::ActorClassRegistry`
  (имя → `FSoftClassPath`). Blueprint-классы допустимы (решение owner
  2026-09-02). Нерезолвимое имя → `Invalid` + заглушка-меш.
- Инвалидация: `MHNotifyGeneratedResourceChanged(Key)` → `Revision++`, receipt
  перечитывается, bounds обновляются. Реестр **не** инициирует пересборку
  композитов; это делает протокол обновлений (§4) по обратному индексу рецептов.

### 3.3 `MHMaterialize`

```
FMHMaterializeResult MHMaterialize(
    const FMHCompiledRecipe& Recipe, int32 Seed, int32 AppearanceSeed,
    const FTransform& ActorTransform, const TMap<FString, FTransform>& NodeOverrides)
  -> TArray<FMHLeafPlacement{ Kind; ResourceKey; NodePath; FMatrix WorldMatrix;
                              int32 InstSeed; FMHAppearanceChannels; bOverridden }>
  + Warnings
```

Чистая функция: не грузит, не спавнит, не читает мир. Порядок: resolver даёт
план по сидам (как сейчас) → трансформ узла → **override узла заменяет
локальный трансформ узла** (после генерации, до умножения на родителя) →
world matrix → appearance. `InstSeed` листа = `fnv1(translation)` композита,
если у актора не задан явный; паритет с Dagor `getSubEntInstSeed`.

### 3.4 `UMHInstancePoolSubsystem` (World subsystem, Editor)

- Один `UInstancedStaticMeshComponent` на `FISMComponentDescriptor`
  (движковый дескриптор из Packed Level Actor: меш + материалы + флаги +
  appearance layout) на мир. Владелец компонентов — служебный актор пула
  `AMHInstancePoolActor` (transient, не сохраняется).
- API: `FMHInstanceHandle Add(Placement)`, `Update(Handle, WorldMatrix,
  Appearance)`, `Remove(Handle)`, `ReverseLookup(Component, InstanceIndex) →
  (Actor, NodePath)` для Outliner/селекшна, `BeginBulk()/EndBulk()` для
  одного `MarkRenderStateDirty` на пачку.
- `Actor`-листья: спавн через пул по классу (`SpawnActor` с
  `RF_Transient`, `bIsEditorOnlyActor=false`), аттач к root композита,
  `Owner = композит`, метки для Outliner. Свет, PPV, любые BP — этим же путём.
- Undo: транзакционен только актор композита (его свойства); пул
  восстанавливает материализацию из состояния актора в `PostEditUndo`.
  Компоненты пула вне транзакций.

### 3.5 `AMHCompositeActor`

Сохраняемое состояние — **ровно** это:

```
TSoftObjectPtr<UMHCompositeAsset> CompositeAsset;
int32 Seed; bool bAutoSeed;
int32 AppearanceSeed; bool bAutoAppearanceSeed;
TMap<FString, FTransform> NodeOverrides;   // NodePath -> локальный трансформ узла
```

Транзиентное: `TArray<FMHInstanceHandle>`, `TArray<FMHLeafPlacement>
LastPlacements` (для Outliner и reseed-diff), `LastError/LastWarnings`.

**Удаляется** из актора и из всех документов: `ResolvedSignature`,
`CompactResolvedState` как гейт, `PlacementDependencies`, `AppliedDefinition`,
`AppliedGraph`, `bPlanAvailable`/`bBasisRejected` семантика «подпись устарела».
`RebuildPlacement` = `Materialize` + diff по `NodePath` с `LastPlacements` +
`Update/Add/Remove` хэндлов. Перемещение актора — только `Update` по хэндлам,
без `Materialize`. `bPlacementEditMode` и basis-update сохраняют смысл, но
опираются на `NodePath`, не на подписи.

Операции над overrides (Details/Outliner): `Reset override`, `Reset all`,
`Promote to composite…` (записать текущие локальные трансформы как новый
`.composite` через существующий экспортный путь level subsystem). Override
узла, которого больше нет в рецепте, **сохраняется** и попадает в Warnings
(`MH_W_ORPHAN_OVERRIDE`), молча не удаляется.

### 3.6 Точки выхода провенанса

Только здесь читаются `SourceHash`/`AppliedHash` и сверяются с индексом:

1. `PreSaveWorld` — warning в Message Log «N composite instance(s) reference
   stale or missing resources», сохранение не блокируется.
2. Build preflight (`MHCompositeBuildPreflight*`, есть) — **error**, блокирует.
3. Runtime snapshot (`MHRuntimeCompositeInput` admission, есть) — **error**.
4. Export/Level operations (`UMHCompositeLevelSubsystem::Build/Break`) — error.

Вне этих точек хэши никого не блокируют и ничего не пересобирают.

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

## 5. Порядок срезов (программа R)

Каждый срез: ветка `recipe/r<N>-<slug>` от свежего `origin/main`, один PR,
квитанция `docs/receipts/recipe_r<N>.md`, red-first, гейты §9. Следующий —
только после owner merge. Ограничения задач owner: ≤7 acceptance criteria,
≤6 изменяемых файлов (удаления и тесты считаются отдельно), 30-секундный
read-aloud тест описания. Если срез не влезает — дели, не растягивай.

Перед R0 обязателен **D0** (§8) — документальный срез. Без него внешние
исполнители будут восстанавливать старую модель по устаревшему нормативу.

| Срез | Содержание | Red-тест (главный assert) | Что удаляется |
|---|---|---|---|
| **D0** | Аудит и чистка документов, новый норматив `docs/16_recipe_model.md` | грепы §7 = 0 вне `docs/archive/` и `docs/receipts/` | устаревшие разделы 10/11/12/14/15 |
| **R0** | `UMHLeafPrototypeRegistry`; `MHLoadAppliedResource` и `MHResolveCompositeDefinitionEndpoint` заменены на реестр | загрузка карты с 2 акторами одного ассета: `registry_lookups == uniqueKeys`, `IAssetRegistry::GetAssets` вызовов = 0, `FAssetData(&Object)` = 0 | `MHLoadAppliedResource`, `AppliedPlanReceipt` на живом объекте, `Endpoints` в definition entry |
| **R1** | Receipt в реестре без compile-wait; заглушка `PlaceholderMesh` | `wait_static_mesh_compilation_ms == 0` при компилирующемся меше; невалидный receipt → `Invalid` + заглушка, не ошибка актора | `FinalizeDeferredMeshes`, `FinishCompilation` в `Composite/` |
| **R2a** | `FMHCompiledRecipe` на ассет, вложенные по ссылке, обратный индекс | 2 root с общим мешем: меш компилируется в граф 1 раз; `definition_cache_misses == uniqueAssets` | `FMHCompositeDefinitionKey` с `ClosureHash`, `FAppliedPlanBuilder` |
| **R2b** | Актор: состояние §3.5, `Materialize`, diff по `NodePath`, `NodeOverrides` без UI | `PostEditMove` не вызывает `Materialize`; override узла переживает реимпорт рецепта; orphan-override → warning | `ResolvedSignature`, `CompactResolvedState`-гейт, `PlacementDependencies`, `AppliedGraph`, stale-rebuild-skip по подписям |
| **R2c** | Точки выхода §3.6 | stale receipt: save → warning, preflight → error, snapshot → reject | receipt-проверки из `RebuildPlacement` |
| **R3** | Реимпорт меша in place без rebuild | targeted reimport: `actor_rebuild_ms_total == 0`, `notified_actors == 0`, инстансы показывают новую геометрию | ветка `RebuildComposite` в `MHNotifyGeneratedResourceChanged` для ключей мешей |
| **R4** | `UMHInstancePoolSubsystem` для мешей; актор держит хэндлы; Outliner через `ReverseLookup` | 2 актора с общим мешем → 1 ISM на дескриптор в мире; селекшн инстанса → правильный актор и `NodePath`; undo восстанавливает | per-actor ISM-бакеты в placement compiler |
| **R5** | `Actor`-листья через пул (BP, свет, PPV) | лист-BP спавнится, аттачится, умирает с актором, переживает duplicate/undo | `UChildActorComponent`-путь |
| **R6** | UI overrides: Reset, Reset all, Promote to composite | promote даёт `.composite`, который при импорте воспроизводит трансформы | — |
| **R7** | Async-загрузка прототипов при загрузке карты | cold DDC: `package_loads` в `MH_PERF_MAPLOAD` = 0 синхронных; первый кадр раньше готовности мешей | синхронный `LoadObject` в горячем пути |

После R7 — расширения (физическая симуляция, автосборка по объёмам) как
**производители `NodeOverrides`**: отдельные инструменты, ядро о них не знает.
Их контракт: вход — `LastPlacements` актора, выход — запись в `NodeOverrides`
через транзакцию. Проектируются отдельным документом после R6.

`R4` и `R7` ставятся по замерам после R3 (полевой протокол из
`m0_perf_instrumentation.md` §6): если `MH_PERF_MAPLOAD.total_ms` и
`MH_PERF_REIMPORT.total_ms` уже в норме owner — R4/R7 можно отложить, но не
отменить: R5 (акторы-листья) от них не зависит.

## 6. Линия S (Source-конвейер) — параллельно, не блокирует R

Ратифицировано owner 2026-09-02, исполняется отдельными срезами `source/s<N>`:

- **S0** Индекс инкрементальный: `(size, mtime)` из `ResourceCandidates` как
  фильтр, хэш — только при несовпадении или как подтверждение по флагу;
  один проход снапшота вместо двух.
- **S1** FBX в скане **не парсится никогда**: зависимости mesh→material
  берутся из receipt импорта (`UMHStaticMeshImportData`) и из индекса
  предыдущего скана; парс — только в момент импорта.
- **S2** `MHCreateDefaultSourceAnalysisServices` не делает `FullScan` на
  targeted reimport; `FullScan` только по явной команде и при пересоздании БД.

Линия S не меняет receipt'ы, hash-домен и формат индекса; при необходимости
изменения формата — OPEN-вопрос owner.

## 7. Документальная политика (без полумер)

Owner многократно сталкивался с тем, что внешние агенты читали устаревший
норматив и выворачивали модель обратно. Поэтому:

1. Активный норматив после D0 — ровно три файла: этот `KICKOFF_PROMPT.md`,
   `docs/16_recipe_model.md` (создаётся в D0), `README.md`. Всё остальное в
   `docs/*.md` — либо переписано под новую модель, либо перенесено в
   `docs/archive/` с шапкой `> HISTORY. Не норматив. Модель заменена
   docs/16_recipe_model.md (2026-09-02).`. Никаких «superseded»-пометок внутри
   активных документов, никаких «удалим по ходу».
2. `docs/receipts/*` — история исполнения, не норматив; README это говорит явно.
3. **Грепы-гейты**: после каждого среза, начиная с D0, в `docs/*.md` и
   `README.md` (исключая `docs/archive/`, `docs/receipts/`) должно быть 0
   вхождений терминов, удалённых этим и предыдущими срезами. Список
   пополняется по срезам и живёт в `docs/16_recipe_model.md` §«Удалённые
   термины». Стартовый список для D0:
   `ResolvedSignature`, `CompactResolvedState`, `PlacementDependencies`,
   `ClosureHash`, `AppliedDefinition`, `AppliedGraph`, `definition cache`,
   `definition-кэш`, `MHLoadAppliedResource`, `AppliedPlanReceipt`,
   `FinalizeDeferredMeshes`, `applied state` (в значении состояния актора),
   `MH.Managed`/`MH.Kind` как способ резолва.
4. В квитанции каждого среза — таблица «документ → действие (переписан /
   архив / без изменений) → почему».
5. Тесты, закрепляющие удалённый концепт, **удаляются вместе с концептом** в
   том же срезе, а не переписываются под «ожидаемый старый результат».
   Ожидаемые кандидаты: `MHCompositeAppliedPlanAdmissionTest`,
   `MHResolvedPlanMetadataTest`, `MHCompositeOutlinerStaleRebuildTest`,
   `MHCompositeDefinitionMetricsTest`, части `MHCompositePlacementLifecycleTest`.
   Остаются и переезжают в реестр/точки выхода: `MHStaticMeshReceiptTest`,
   `MHResourceKeyResolverTest`, `MHRuntimeBridgeAdmissionTest`,
   `MHCompositeBuildPreflightRegressionTest`.
6. Никаких параллельных «старый/новый путь» под флагом дольше одного среза.
   Если срез не может удалить старый путь — он не завершён.

## 8. D0 — первый срез, документы

Acceptance:

1. Создан `docs/16_recipe_model.md`: §3–§4 этого документа как норматив,
   таблица удалённых терминов, список точек выхода, OPEN-вопросы §10.
2. `docs/10`, `11`, `12_*`, `14`, `15`: разделы про applied state, подписи,
   definition cache, admission на инстансе, перф-программу U0–U7 — либо
   переписаны, либо документ целиком в `docs/archive/` с шапкой §7.1.
   Wire-формат, сиды, runtime-мост из 10/11 — сохранены (переписаны в 16 или
   оставлены в 10 после вырезания).
3. `KICKOFF_PROMPT.md` заменён этим текстом.
4. `README.md` обновлён: активный норматив = три файла; receipts = история.
5. Грепы §7.3 = 0 вне архива и receipts.
6. Проверено, смержен ли `codex/m0-perf-instrumentation`; если нет — в
   квитанции D0 зафиксировано, что R0 базируется на нём (или добавляет свои
   счётчики `registry_lookups`, `package_loads`, `receipt_validations`).
7. Квитанция `docs/receipts/recipe_d0.md` с таблицей §7.4.

Кода в D0 нет. Ветка `recipe/d0-docs`.

## 9. Гейты каждого C++-среза

- Guarded UE build; StrictIncludes non-unity/no-PCH; force-unity
  adaptive-off; `git diff --check`.
- Полный NullRHI `Automation Mimir` с `-MHGoldenRoot=<repo>/golden`: 0 failed;
  число тестов может **уменьшаться** только на удалённые по §7.5, каждое
  удаление названо в квитанции.
- Red-first: red-лог на коммите с тестом до реализации, green-лог после,
  строки в квитанции.
- `mh.PerfTrace 1`: `MH_PERF_MAPLOAD` и `MH_PERF_REIMPORT` до/после на
  собственном host; owner делает полевой замер по протоколу M0 §6.
- Собственный host исполнителя; audit-host и portfolio-проект owner не трогать.

## 10. OPEN-вопросы (fail-closed правило до ответа owner)

- **OPEN-R-1 Undo для пула.** Правило до ответа: транзакционен только актор;
  пул восстанавливается из его состояния в `PostEditUndo`. Если owner
  потребует undo на уровне инстансов пула — R4 расширяется.
- **OPEN-R-2 Заглушка.** Правило: движковый куб из `/Engine/BasicShapes/Cube`,
  настраивается в `UMHCompositeSettings::PlaceholderMesh`.
- **OPEN-R-3 Actor-листья и PIE/cook.** Правило: `Actor`-листья участвуют в
  runtime-снапшоте так же, как сейчас `ActorClassRegistry`; ничего нового в
  runtime-мост не добавляется до отдельного решения.
- **OPEN-R-4 Ключ дескриптора пула.** Правило: `FISMComponentDescriptor` +
  `AppearanceCustomDataBaseIndex`; кастомный ключ — только если движковый не
  различает нужные случаи (доказать тестом).

Новую семантику не угадывай. Реальную дыру оформляй как `OPEN-R-<N>`:
Контекст → Вопрос → Временное fail-closed правило → Статус; затронутая часть
STOP до ответа owner.

## 11. Ключевые инварианты Dagor, которые нельзя потерять (шпаргалка)

Из `compositMgrService.cpp` DagorEngine @ `7572366`:

1. Рецепт компилируется один раз на ассет, инстанс — запись из нескольких int
   плюс срез хэндлов (`CompositEntityPool` / `CompositEntity`).
2. `selectEnt` тратит один шаг ПСЧ даже на узле с одним вариантом — паритет
   уже закреплён в `MHRandomStream`, не сломать.
3. Сид вложенного композита передаётся **до** построения поддерева
   (`PendingCloneSeeds`), иначе ×2 на уровень вложенности.
4. Узел без девиаций схлопывается в матрицу при компиляции, рандом на нём не
   вызывается.
5. Реимпорт листа подменяет ресурс под указателями (`update_rt_pregen_ri`);
   композит не участвует.
6. Изменение рецепта пересобирает инстансы только этого пула; родители не
   уведомляются.
7. Ненайденный лист — заглушка (`InvalidEntity`), уровень сохраняется без потерь.
8. Во время драга гизмо тяжёлые побочные эффекты откладываются до конца драга;
   одна пересборка на самом внешнем уровне.
9. Layout-сид и instance-сид — разные сущности; instance-сид по умолчанию —
   хэш позиции.
10. Экспорт и рендер работают с плоскими пулами и композит не видят.

Если какой-то срез нарушает пункт из этого списка — это OPEN-вопрос, не
решение исполнителя.