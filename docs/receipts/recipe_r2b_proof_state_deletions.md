# R2b-3 (Recipe Model v2.1) — удаление proof-состояния актора

Статус: **READY FOR REVIEW — merge после ревью Lead** (срез целиком состоит из
удалений: KICKOFF §7.4–7.5, docs/16 §7.2). Гейт удалений (OPEN-R2B-1):
`Mimir.V5.Composite.Proof.BuildPreflightFullClosure` зелёный в `main` после R2c
(#88); `Registry.IdentityAdmission` зелёный с R0a.

## 1. База и границы

- ветка: `recipe/r2b-3-proof-state-deletions`; база `origin/main` `fd1e49b`
  (после R2c #88 и тракера #89);
- удалено: `MHCompositeDefinitionSubsystem.{h,cpp}` целиком; из
  `AMHCompositeActor` — `ResolvedSignature` (UPROPERTY), `FMHCompactResolvedState`
  / `FMHCompactResolvedLeafState`, `CompactResolvedState`,
  `StoreCompactResolvedState`, `GetCompact*Signature`,
  `GetCompactResolvedLeafCount`, `GetCompactSelectedOptionCount`,
  `GetCompactResolvedStateAllocatedBytes`, `HasCompactResolvedDiagnostics`,
  `ResolvedDebugPlan` + lease API (`Retain/Release/HasResident/Invalidate`),
  `AppliedDefinition`, `PlacementDependencies`; параметр `Definition` у
  `MHCompileCompositePlacementV5` / `MHTryCompileCompositePlacementReseedV5`;
  поля подписей у `FMHCompositeOutlinerFreshness`;
- resolver, `MHCompiledRecipe`, `MHMaterializeLayout`, `MHProofCache`,
  компилятор размещения (кроме сигнатуры), `golden/` — не тронуты.

## 2. Acceptance

| # | Критерий | Результат |
|---|---|---|
| 1 | Лексический ноль удалённых сущностей **в коде** (docs/16 §7.2 `removed-entities`): `tools/check_normative_docs.py` теперь сканирует `ue/MimirComposite/Source` | GREEN: OK (RED был на `PlacementDependencies` и `FMHCompositeDefinitionKey`) |
| 2 | Актор без proof-состояния: нет свойства `ResolvedSignature`, нет класса `MHCompositeDefinitionSubsystem`, резидентный план без closure/подписей | GREEN: `Recipe.ActorHasNoProofState` |
| 3 | `DependsOnResource` без списка на акторе: root-ключ + ресурсы резидентного recipe-графа (`MHCollectRecipeGraphDependencies`) | GREEN: `Recipe.ActorReimportViaRecipeDependents`, `Seed.DeterminismNestedProfilesAndMove` (реимпорт профиля), `DefinitionPool.TargetedReimportInvalidatesOnce` |
| 4 | Ни один потребитель не читает удалённое: Break/Edit/Outliner/меню инспекции работают через `GetResolvedPlan()` | GREEN: полный suite |
| 5 | Число тестов: −1 (`DefinitionPool.IndexedLookupAndInvalidation`, гонял удалённый subsystem напрямую) +1 (`Recipe.ActorHasNoProofState`); `CompactResolvedState.LazyDebugPlan` переименован в `ResidentPlan.SingleAuthority` | см. §6 |
| 6 | Гейты KICKOFF §9 | §4 |

## 3. Red-first

RED — коммит `99adfed`: (а) `tools/check_normative_docs.py` — код-гейт по блоку
`removed-entities`: `VIOLATION: removed entity PlacementDependencies in code
…MHCompositeActor.cpp:403…` и `FMHCompositeDefinitionKey in code
…MHCompositeDefinitionSubsystem.cpp:41…` (exit 1); (б) тест
`Mimir.V5.Composite.Recipe.ActorHasNoProofState` — лог `R2B3_RED_TEST.log`,
строка 1078 `Result={Fail}`: «actor exposes no ResolvedSignature property»,
«definition cache subsystem class is gone».

GREEN — коммит `e1e5a3f`. Первые две GREEN-итерации падали на компиляции (пустой
namespace в заголовке актора; поля подписей в `MakeFreshness()` теста Outliner'а)
и на `PlacementDependencyView` («dead-root key remains observable») — см. §5
(`ObservedRecipeGraph`, soft-path корня). Лог `R2B3_GREEN4_TEST.log`: 15× `Success`.

## 4. Гейты

| Gate | Результат |
|---|---|
| non-unity/no-PCH build | RED `Succeeded` (`R2B3_RED_BUILD.log`); GREEN `Succeeded` (`R2B3_GREEN4_BUILD.log`) |
| `Recipe.*` + `ResidentPlan.*` + `Proof.*` + `PlacementDependencyView` | 15× `Success` (`R2B3_GREEN4_TEST.log`) |
| полный NullRHI suite, generic host | `Success=191`, 0 Fail (`R2B3_FULL2.log`); число тестов = 191 как до среза (−1 +1) |
| то же под `MimirCompositeV5S6.uproject` (isolated) | `Success=189 Fail=2` (`R2B3_FULL2_ISOLATED.log`): только известные pre-existing `Lifecycle.NoBuildBeforeRegistration`, `Seed.AppearanceMigration` |
| `BuildPlugin -StrictIncludes` | `ExitCode=0 (Success)` (`R2B3_STRICT2_UAT.log`, пакет `E:\MimirComposite_R_R2B3_Strict2_20260903`) |
| guarded force-unity | `Succeeded` (`R2B3_FORCE_UNITY2_UBT.log`) |
| `git diff --check` / `check_normative_docs.py` (docs + код) | чисто / OK |

## 5. Реализация

- `AMHCompositeActor`: единственный план — `ResidentPlan`; `GetResolvedPlan`,
  `UpdatePlacementBasis`, `OnConstruction`, `SetPlacementEditMode`,
  `GetEditedCompositeDocument`, reseed-diff проверяют его сиды/наличие вместо
  compact state. `DependsOnResource` = root-ключ ∨ ключ ∈ ресурсам резидентного
  recipe-графа (composite / static_mesh / placement_profile) — по docs/16 §7.2
  четвёртая строка: список зависимостей на акторе заменён обратной связью
  рецепта; материалы/текстуры — reconcile бакетов (R4). Граф последней
  попытки сборки (`ObservedRecipeGraph`) хранится и при неудаче — иначе
  placement с отсутствующим endpoint'ом не получил бы retry-нотификацию; корень
  наблюдается по идентичности soft-path даже когда ассет мёртв
  (`PlacementDependencyView`: «dead-root key remains observable»).
- `BreakComposites`, Edit-сессия, Outliner, «Inspect resolved plan» — без
  lease-обёрток; Outliner freshness = сиды + `PreviewRevision`.
- Компилятор размещения: параметр `Definition` (никогда не читался) удалён.
- `MHNotifyGeneratedResourceChanged` / `MHRebuildAllLoadedCompositeActors` —
  без инвалидации definition-кэша (его больше нет; рецепты и proof
  инвалидируются своими реестрами).
- `FMHDefinitionCacheMetrics` и поля `definition_cache_hits/misses` в
  `MH_PERF_MAPLOAD` **остаются** — с R2b-2 это счётчики кэша рецептов
  (совместимость M0-отчётов); `LookupProbes/InvalidationProbes/ClosureHitBuilds`
  больше никем не пишутся (всегда 0).

## 6. Удаления тестов и API (ревью Lead)

| Что | Почему | Замена |
|---|---|---|
| тест `DefinitionPool.IndexedLookupAndInvalidation` | гонял `UMHCompositeDefinitionSubsystem::GetOrBuildDefinition/InvalidateDefinition` напрямую; subsystem удалён | кэш рецептов: `Recipe.CompiledRecipeStructure` (ревизия, инвалидация), `DefinitionPool.TargetedReimportInvalidatesOnce` (miss=1/hit=2) |
| тест `CompactResolvedState.LazyDebugPlan` → переименован `ResidentPlan.SingleAuthority` | утверждал compact state + lease | тот же файл: резидентный план без closure/подписей, один объект на все чтения |
| helper `SeedTestSignature` + 1 утверждение (SeedTest), helper `AdmissionStoredSignature` + 1 утверждение (AppliedAdmission) | reflection удалённого свойства `ResolvedSignature` | подписи — только на proof-плане (R2b-2, R2c) |
| `InvalidateAllDefinitions` в `Perf.EndpointCounters`/`InstrumentationCounters`/`Registry.IdentityAdmission` (cold pass) | subsystem удалён | cold pass = `UMHCompiledRecipeRegistry::InvalidateAll` + `UMHEndpointPrototypeRegistry::InvalidateAll` (уже вызываются) |
| `U5_COTTAGE_METRICS`: поля `resolved_state_bytes`, `debug_resident` → `resident_plan` | compact state удалён | резидентный план (0/1) |
| API актора: `Retain/Release/HasResidentResolvedDebugPlan`, `HasCompactResolvedDiagnostics`, `GetCompact*`, `StoreCompactResolvedState`, `InvalidateResolvedDebugPlan` | U7-инструментарий compact/lease, старая модель | `GetResolvedPlan()`, `GetPreviewRevision()` |

## 7. Изменённые файлы

- удалены: `Public|Private/Composite/MHCompositeDefinitionSubsystem.{h,cpp}`
- `Public|Private/Composite/MHCompositeActor.{h,cpp}`,
  `Public|Private/Composite/MHCompositePlacementCompiler.{h,cpp}`,
  `Private/Composite/MHCompositeLevelSubsystem.cpp`,
  `Private/Composite/MHCompositePlacementEvents.cpp`,
  `Private/UI/MHCompositeOutliner.cpp`, `Public|Private/UI/MHCompositeOutlinerModel.{h,cpp}`,
  `Private/UI/MHSourceToolMenus.cpp`
- тесты: `MHCompositeActorRecipePreviewTest.cpp` (+1 тест), `MHCompositeDefinitionMetricsTest.cpp`
  (−1 тест), `MHCompositeISMMaterializationTest.cpp` (переименование),
  `MHCompositeISMCottageMetricsTest.cpp`, `MHCompositeSeedTest.cpp`,
  `MHCompositeAppliedPlanAdmissionTest.cpp`, `MHEndpointPrototypeRegistryTest.cpp`,
  `MHStaticMeshImporterTest.cpp`
- `tools/check_normative_docs.py` (код-гейт), `docs/receipts/recipe_r2b_proof_state_deletions.md`,
  `docs/RECIPE_EXECUTION_STATUS.md`

## 8. Вопросы Lead

Нет. Решение по каждой строке §6 — за Lead (KICKOFF: ревью каждого удаления).
