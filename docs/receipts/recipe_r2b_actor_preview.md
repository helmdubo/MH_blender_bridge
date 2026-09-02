# R2b-2 (Recipe Model v2.1) — актор строит preview через рецепт

Статус: **READY FOR REVIEW — merge только после ревью Lead** (в срезе есть
удаления кода актора и правки/удаления старомодельных тестов, §7.5 «замена до
удаления», KICKOFF §5 R2b). Вторая часть R2b: `AMHCompositeActor` собирает
preview из скомпилированного рецепта через `MHMaterializeLayout`; ни applied
graph, ни closure, ни definition cache в preview-плоскости больше не участвуют;
план резидентен, перемещение актора не вызывает Layout, загрузка карты не
вызывает Proof.

## 1. База и границы

- ветка: `recipe/r2b-actor-preview`; база `origin/main` `cace15f` (после R2b-1
  #84 и тракера #85);
- затронуты: `MHCompositeActor.{h,cpp}`, `MHMaterializeLayout.{h,cpp}`,
  `MHCompiledRecipe.{h,cpp}` (публикация `MHResolvePreviewGraph`, `InvalidateAll`),
  `MHCompositeLevelSubsystem.cpp` (proof-admission в Break),
  `UI/MHCompositeOutlinerModel.{h,cpp}` (freshness), новый тест
  `MHCompositeActorRecipePreviewTest.cpp`, правки тестов из §6;
- resolver (`MHRandomStream`), компилятор размещения, proof-плоскость
  (`MHBuildAppliedCompositeGraph`, runtime-мост, preflight, export),
  `UMHCompositeDefinitionSubsystem` (остаётся для proof/индекса), `golden/` — не
  тронуты.

## 2. Acceptance (KICKOFF §5 R2b, docs/16 §2.10, §4)

| # | Критерий | Результат |
|---|---|---|
| 1 | Preview переключён на `MHMaterializeLayout`: `RebuildPlacement` = `Compile(asset)` (кэш `asset + RecipeRevision`) → materialize → компилятор размещения | GREEN: `ActorPreviewNoProofOnLoad` — `BuildAppliedGraph.Calls == 0`, definition cache не опрашивается, план резидентен |
| 2 | Загрузка карты не вызывает Proof (closure/подписи) | GREEN: тот же тест; `MHResolvePreviewGraph` обнуляет подписи, closure не строится |
| 3 | `PostEditMove` не вызывает Layout: basis-update по резидентному плану | GREEN: `ActorMoveNoLayout` — `ResolveCompositePlan.Calls == 0` после `SetActorLocation`, хэндлы сдвинуты, rebuild-счётчик не изменился |
| 4 | Targeted reimport доходит до актора через ресурсы recipe-графа (включая вложенные композиты); чужие ключи не перестраивают | GREEN: `ActorReimportViaRecipeDependents` |
| 5 | Reseed-diff и edit-session не делают повторного resolve (резидентный план; edit-Tick — `MHResolvePreviewGraph`, без prospective raw hash) | GREEN: полный suite (§4) |
| 6 | Outliner freshness без подписей: `PreviewRevision` + сиды | GREEN: outliner-тесты полного suite |
| 7 | Гейты KICKOFF §9 | §4 |

## 3. Red-first

Тесты `Mimir.V5.Composite.Recipe.{ActorPreviewNoProofOnLoad, ActorMoveNoLayout,
ActorReimportViaRecipeDependents}` (`MHCompositeActorRecipePreviewTest.cpp`);
фикстура — `MHRecipeTestFixture.h` (R2a/R2b-1) + реальные mesh-receipt'ы.

RED — коммит `698e6c3`: перемещение = 1 Layout (ленивый re-resolve compact
state, инструментирован stage-scope'ом `ResolveCompositePlan`), загрузка карты =
applied graph + 2 обращения к definition cache. Лог `R2B2_RED_TEST.log`,
строки 1080/1089: `Result={Fail}`; dependents-тест — regression guard, `Success`.

GREEN — коммит `0020220`. Лог `R2B2_GREEN4_TEST.log`: 9× `Result={Success}`
(`Mimir.V5.Composite.Recipe.*`).

## 4. Гейты

| Gate | Результат |
|---|---|
| non-unity/no-PCH build | RED `Succeeded` (`R2B2_RED_BUILD.log`); GREEN `Succeeded` (`R2B2_GREEN4_BUILD.log`) |
| `Mimir.V5.Composite.Recipe.*` (9 тестов) | 9× `Success` (`R2B2_GREEN4_TEST.log`) |
| полный NullRHI suite, generic host | `Success=188`, 0 Fail (`R2B2_FULL4.log`) |
| то же под `MimirCompositeV5S6.uproject` (isolated) | `Success=186 Fail=2` (`R2B2_FULL4_ISOLATED.log`): только известные pre-existing `Lifecycle.NoBuildBeforeRegistration`, `Seed.AppearanceMigration`; все 9 `DefinitionPool.*` зелёные |
| `BuildPlugin -StrictIncludes` | `ExitCode=0 (Success)` (`R2B2_STRICT4_UAT.log`, пакет `E:\MimirComposite_R_R2B2_Strict4_20260903`) |
| guarded force-unity | `Succeeded` (`R2B2_FORCE_UNITY4_UBT.log`) |
| `git diff --check` / `check_normative_docs.py` | чисто / OK |

## 5. Реализация

- `RebuildPlacement`: `UMHCompiledRecipeRegistry::Compile` (hit/miss пишутся в
  прежние счётчики `definition_cache_hits/misses` — теперь это кэш рецептов) →
  `MHMaterializeLayout` под stage `ResolveCompositePlan` → `CandidateGraph` =
  recipe-граф (для `RootDefinition` компилятора и `MHRecordMapLoadGraph`),
  `PlacementDependencies` = ресурсы recipe-графа (`MHCollectRecipeGraphDependencies`:
  composite / static_mesh / placement_profile; **без** material/texture — по
  docs/16 §4 их изменения не перестраивают актор, reconcile — R4);
  `CandidatePlan` = резидентный план. Ошибка сборки → как раньше: старый вид из
  `ResidentPlan` + диагностический маркер.
- Identity admission корня в preview: `UMHEndpointPrototypeRegistry::ResolveEndpoint(composite:<root>)`
  обязан вернуть тот же ассет (канонический путь, embedded receipt) — иначе
  `MH_E_UNRESOLVED_COMPOSITE_REFERENCE`, плана нет. Это §2.4 (identity), не
  сравнение с Source Root; tag-запросов нет.
- Proof-admission на точке выхода (§2.6, п. 4): `UMHCompositeLevelSubsystem::BreakComposites`
  строит applied graph + `MHResolveCompositePlan` + transform admission сам и
  отказывает с диагностикой applied-плоскости (`MH_E_SOURCE_INDEX_INVALID`,
  `MH_E_UNRESOLVED_COMPOSITE_REFERENCE: … <token>`) до любого спавна; спеки
  Break строятся по proof-плану (layout идентичен preview по shadow parity).
  Остальные точки выхода (preflight, snapshot, export, PreSaveWorld) уже строят
  свой applied graph или переходят в R2c.
- Инвалидация рецептов по нотификациям (`MHNotifyGeneratedResourceChanged`,
  docs/16 §4): ключ `composite:` → `InvalidateComposite` (только этот
  рецепт, родители не перекомпилируются); ключ `placement_profile:` →
  `InvalidateProfile` (рецепты, инлайнящие профиль). Обе дыры найдены тестами
  `DefinitionPool.TargetedReimportInvalidatesOnce` и
  `Seed.DeterminismNestedProfilesAndMove` после переключения.
- Identity admission корня повторяется только после нотификации
  (`Registry.Invalidate` → re-admission, §2.2 «раз на ключ за сессию»);
  receipt, мутированный in place без нотификации, preview не перепроверяет —
  это поймает proof-плоскость. Тест `InvalidRootReceiptBlocksPlanAndBreak`
  теперь объявляет мутацию как импорт (`MHNotifyCompositeAssetChanged`).
- `ResidentPlan` (§2.10 «LastPlacements») хранится в акторе; `GetResolvedPlan`
  возвращает его без re-resolve; `UpdatePlacementBasis` и reseed-diff читают его;
  `PreviewRevision++` на каждый успешный build/edit-step.
- Удалено из актора: `ResolvePlanFromCompactState` (ленивый
  Layout+Appearance+Proof на каждый basis-update и на каждое чтение плана),
  ветка `MHBuildAppliedCompositeGraph` и обращение к
  `UMHCompositeDefinitionSubsystem::GetOrBuildDefinition`, prospective raw hash в
  edit-Tick. `CompactResolvedState`, `ResolvedSignature`, `AppliedDefinition`,
  `AppliedGraph` (= recipe-граф) остаются как диагностика без гейтов — их
  удаление R2b-3 (OPEN-R2B-1).
- `MHMaterializeLayout` дополнен `Graph` в результате (известен и при ошибке
  layout — для retry-нотификаций); `MHResolvePreviewGraph` вынесен публично для
  edit-session.
- Outliner: `FMHCompositeOutlinerFreshness` сравнивает `Seed`, `AppearanceSeed`,
  `PreviewRevision`; поля подписей остались в структуре до R2b-3.

## 6. Затронутые старомодельные тесты (ревью Lead)

После переключения preview на рецепты полный suite дал 15 падений на generic
и ещё 7 (DefinitionPool) на isolated — все они утверждали старую модель.
Ни один тест не удалён целиком; ниже — что заменено и что удалено внутри
тестов (каждое место помечено комментарием `// R2b-2:` в коде). Три
категории причин: **(A)** актор больше не несёт подписей/closure (preview);
**(B)** невыбранные/невалидные endpoint'ы не блокируют preview — это proof
(Break/preflight); **(C)** счётчики definition-cache/applied-graph.

| Тест | Категория | Замена | Удалено |
|---|---|---|---|
| Seed.DeterminismNestedProfilesAndMove | A | равенство подписей → `MHCompareRecipeShadowParity` (actor vs proof-план applied graph, actor vs actor); смена профиля → подпись proof-плана меняется + preview-план меняется + `PreviewRevision` растёт | «seed 200 gives a different signature» (трасса уже различается) |
| Seed.MissingAppliedDependencyAndHealing | B | preview с отсутствующим receipt невыбранного меша: план есть, ошибки нет, ключ зависимости наблюдается; отказ и healing — на `MHBuildAppliedCompositeGraph`; подписи «до/после» — proof-план | «clears derived signature», «failed rebuild shows a visible diagnostic» (перестроение больше не падает) |
| Seed.ShearBeforeComponentMutation | A | — (соседние `TestNull(GetResolvedPlan())` несут смысл) | две проверки пустой подписи |
| Seed.MinimalNoneTransformTopologyUpdates | A | «signature seed field» → `Plan->Seed == 200` | — |
| Seed.ConstantVisualsRefreshTrace | A | подпись → `Seed == 200` + `PreviewRevision` растёт; сравнение с shared resolver → shadow parity | — |
| Seed.FrozenSeed42AppliedAssetParity | A | ратифицированная подпись и преимидж — на proof-плане applied graph; actor vs proof — shadow parity; все ратифицированные decisions/draws/leaves остаются на акторе | равенство/неравенство подписей для сидов 100/200 (трассы уже проверяются) |
| AppliedAdmission.InvalidRootReceiptBlocksPlanAndBreak | B | helper `ExpectRejected(bPreviewRejects=true)`: preview отказывает через identity admission корня (реестр), Break — через proof | — |
| AppliedAdmission.UnselectedAbstractActorBlocksClosure | B | `bPreviewRejects=false`: preview строится (0 листьев), Break отказывает с proof-диагностикой, токен актора — в ошибке applied graph | «plan null» для невыбранной опции |
| AppliedAdmission.EditPlacementBasisAndProspectivePlan | A | подписи/closure «basis move» и «Cancel» → shadow parity + `Closure.Resources.IsEmpty()`; «authored Edit меняет подпись» → parity false + authored TRS = submitted local | «prospective closure includes root», «prospective root receipt hashes edited source» (preview не строит closure) |
| PlacementDependencyView | B | отсутствующий **выбранный** меш → заглушка + warning, план есть | «cannot expose a fresh plan» |
| LevelOperations | B | «unselected nested option in closure» → на `Dependencies` applied graph; preview closure пуст | — |
| CompactResolvedState.LazyDebugPlan | A | резидентный план вместо ленивой копии: инспекция не арендует копию, release не освобождает, тот же объект; подписи пусты и зеркалятся compact state | «debug plan resident after inspection / freed after release» |
| Outliner.StaleRebuildSkip | A | freshness по `PreviewRevision` + сиды; ось «revision invalidates freshness» | три оси подписей |
| Perf.EndpointCounters | C | cold pass также инвалидирует реестр рецептов; «≥ unique endpoint keys» → root + **selected** меши (невыбранные не резолвятся) | — |
| Perf.InstrumentationCounters | C | «BuildAppliedGraph captured» → `== 0` в preview (стадия остаётся для proof) | — |
| Registry.IdentityAdmission | — | без правок: identity admission корня вернула lookups/admissions = 2 | — |
| DefinitionPool.* (7 тестов, isolated) | C/A | helper `DefinitionMetricsCommonAssertions`: applied graph = 0; `DefinitionPlanParity`: shadow parity + «no closure»; кэш рецептов: SharedGraph100 miss=1/hit=99, Drag miss=1/hit=1, TargetedReimport miss=1/hit=2 (**найдена и закрыта дыра**: нотификация реимпорта композита не инвалидировала рецепт — `InvalidateComposite`), CacheHit miss=0/hit=12; EndpointWeakGc/EndpointReimport: подпись/closure → proof-план (`DefinitionProofClosureHash`) и parity | равенства подписей/преимиджей/closure с actor-планом |

Новые тесты: `Recipe.ActorPreviewNoProofOnLoad`, `Recipe.ActorMoveNoLayout`,
`Recipe.ActorReimportViaRecipeDependents`.

## 7. Изменённые файлы

- `Source/MimirCompositeEditor/Public|Private/Composite/MHCompositeActor.{h,cpp}`
- `Source/MimirCompositeEditor/Public|Private/Composite/MHMaterializeLayout.{h,cpp}`
- `Source/MimirCompositeEditor/Public|Private/Composite/MHCompiledRecipe.{h,cpp}`
- `Source/MimirCompositeEditor/Private/Composite/MHCompositeLevelSubsystem.cpp`
- `Source/MimirCompositeEditor/Private/Composite/MHCompositePlacementEvents.cpp`
- `Source/MimirCompositeEditor/Public|Private/UI/MHCompositeOutlinerModel.{h,cpp}`
- тесты: `MHCompositeActorRecipePreviewTest.cpp` (new), `MHCompositeSeedTest.cpp`,
  `MHCompositeAppliedPlanAdmissionTest.cpp`, `MHCompositePlacementTest.cpp`,
  `MHCompositeLevelOperationsTest.cpp`, `MHCompositeDefinitionMetricsTest.cpp`,
  `MHCompositeISMMaterializationTest.cpp`, `MHCompositeOutlinerStaleRebuildTest.cpp`,
  `MHStaticMeshImporterTest.cpp`
- `docs/receipts/recipe_r2b_actor_preview.md`, `docs/RECIPE_EXECUTION_STATUS.md`

## 8. Вопросы Lead

- OPEN-R2B-1 (из R2b-1) — гейт удалений R2b-3.
- **OPEN-R2B-2.** Резидентный план на актор: M0 измерял `CompactResolvedState`
  как экономию памяти (draws/decisions не резидентны). R2b-2 держит полный
  preview-план (decisions, draws, nodes, leaves) — это ADR §2.10 «LastPlacements».
  Оценка: ~1–2 KB на лист (333 листа ≈ 0.5 MB на актор). Если owner считает это
  чрезмерным для портфолио с сотнями акторов — R2b-3 сжимает план до листьев +
  решений (draws не нужны preview). Решение за Lead; не блокирует.
