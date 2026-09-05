> Status: NORMATIVE · Architecture version: Recipe Model v2.1 · Supersedes: — (created in D0b)

# RECIPE_EXECUTION_STATUS — фактическая точка продолжения

Архитектурный порядок задаёт `KICKOFF_PROMPT.md` (§5) и `docs/16_recipe_model.md`
(§8). Этот документ задаёт **фактическую** точку продолжения программы: какие
срезы реально смержены в `origin/main`, какой срез следующий, и какие срезы
разблокированы для параллельной работы. Перед началом любого среза исполнитель
читает эту таблицу и начинает **только** срез со статусом NEXT/READY.

| Срез | Статус | PR / примечание |
|---|---|---|
| D0a | MERGED | #62 |
| M0 | MERGED | #63 |
| R0a | MERGED | #64 |
| R0b | MERGED | #65 |
| R1 | MERGED | #66 (acceptance по формулировке v2; П6 закрывает R1.1) |
| D0b | MERGED | #67 |
| R1.1 | MERGED | #69 (внешний исполнитель, приёмка близнеца); квитанция `docs/receipts/recipe_r1_1.md` |
| R0c | MERGED | #71 (внешний исполнитель, приёмка близнеца); реализация `7811eab` попала в `main` fast-forward-push'ем до PR, проверена близнецом постфактум (полный suite 176/176 на host близнеца); квитанция `docs/receipts/recipe_r0c.md` |
| S0 | MERGED | #73 (внешний исполнитель, приёмка близнеца; возврат 1 — racy fingerprint); квитанция `docs/receipts/source_s0.md` |
| S1 | CLOSED (без кода) | OPEN-S-1 закрыт owner'ом, вариант c: S0 достаточен; полевой замер холодного скана — по протоколу M0 §6 |
| S2 | MERGED | #77 (внешний исполнитель, приёмка близнеца; возвраты OPEN-S2-1/S2-2 закрыты нормативно); квитанция `docs/receipts/source_s2.md`. Линия S закрыта: S0 #73, S1 CLOSED (вариант c), S2 #77 |
| R2a | MERGED (близнец) | два PR: **R2a-1** фазовое разделение resolver'а — #79 (`docs/receipts/recipe_r2a_phases.md`); **R2a-2** `FMHCompiledRecipe` + реестр + `RecipeShadowParity` как CI-гейт — #81 (`docs/receipts/recipe_r2a_compiled_recipe.md`). OPEN-R2A-1 закрыт owner 2026-09-02 (docs/16 §9). **Полевой тест owner 2026-09-02 на портфолио (main `4782082`) пройден** — `docs/receipts/field_recipe_r2a_20260902.md`. Preview-путь ещё не production → R2b |
| R2b | MERGED (близнец) | три PR: **R2b-1** `MHMaterializeLayout` — #84; **R2b-2** актор на рецептах — #86; **R2b-3** удаления proof-состояния актора и definition-кэша — #90 (owner: удаления одобрены 2026-09-03; квитанция `docs/receipts/recipe_r2b_proof_state_deletions.md` §6). ADR docs/16: `Status: NORMATIVE` (D0a: «PROPOSED → NORMATIVE после R2b») |
| R2c | MERGED | #88 (внешний исполнитель, приёмка близнеца; возвраты OPEN-R2C-1…4 закрыты нормативно в `docs/contracts/recipe_r2c.md`); квитанция `docs/receipts/recipe_r2c.md`; `Proof.BuildPreflightFullClosure` зелёный → гейт R2b-3 открыт |
| R3a | MERGED (внешний исполнитель, приёмка близнеца) | #102 (`2cc04c4`): пять хэшей/ревизий интерфейса меша + `FMHEndpointInterfaceDelta` в реестре endpoint'ов, снимки Ready переживают `InvalidateAll`, пути материалов только в `MaterialBindingHash` (OPEN-R3A-1 → вариант 1); близнец: 197/197 на голове ветки; квитанция `docs/receipts/recipe_r3a.md` |
| R3b | MERGED (внешний исполнитель, приёмка близнеца) | #108 (`c51f0cc`): reconcile по `FMHEndpointInterfaceDelta` вместо полного rebuild (payload/bounds → refresh, descriptor → миграция своего бакета, collision → recreate physics, binding → материалы), child-рецепт без перекомпиляции родителя, счётчики `RecipesRecompiled/ParentRecipesRecompiled/BucketsRefreshed/BucketsMigrated`; DECIDED-R3B-1/2 приняты, OPEN-R3B-1 закрыт (fc08ca1); близнец: 202/202, strict/force-unity; benchmark 100 placements — не записан, за owner; квитанция `docs/receipts/recipe_r3b.md` |
| RS-1 | MERGED (внешний агент, ресёрч; owner залил в `main` коммитом `28e898d`) | `docs/reference_notes/dagor_composite_build_break_20260903.md` — daEditor: «Split composites» снимает один слой, undo хранит записи `(asset, tm, seeds)`, «Export as composit» = файл на диск без изменения сцены; сводка для R4-pre в §6 |
| R4-pre | MERGED (внешний исполнитель, приёмка близнеца) | #93 (`e8256b6`): Break = один слой рецепта в preview-плоскости (без proof/tag-запросов), дети-композиты остаются `AMHCompositeActor` с сидами родителя, plan-view компоненты не транзакционны, `PostEditUndo` восстанавливает из записи; близнец: 194/194 на generic-хосте; owner на портфолио: дубли после Undo не воспроизводятся (2026-09-03); квитанция `docs/receipts/recipe_r4_pre.md`; OPEN-R4P-1 открыт (fail-closed: сиды родителя) |
| R4-pre-2 | MERGED (внешний исполнитель, приёмка близнеца) | #103 (`c754036`): Break переносит appearance-каналы на `AStaticMeshActor` (default + transient custom primitive data), Build предупреждает о непредставимом состоянии через чистый `MHPreflightBuildComposite` и продолжает; близнец: 198/198 на generic-хосте; квитанция `docs/receipts/recipe_r4_pre2.md`; полевое подтверждение — owner на портфолио |
| R4 | MERGED (близнец) | async-загрузка выбранных endpoint'ов (`FStreamableManager`, состояние `Loading`), placeholder `UMHCompositeSettings::PlaceholderMesh` (default `/Engine/BasicShapes/Cube`), интерактивный путь без `FinishCompilation`; первая admission по завершении идёт через reimport-протокол R3b (`bFirstAdmission` → rebuild с placeholder на реальный меш); red `Mimir.V5.Composite.Async.ColdEndpointLoadsWithoutSyncLoad`, `Perf.SelectedMeshWait` → R4-ожидания; полный suite 205/0, strict/force-unity; квитанция `docs/receipts/recipe_r4.md` |
| R5a | MERGED (близнец) | #111 (`c8495ad`): `UMHInstancePoolSubsystem` как самостоятельный сервис: бакет `{ULevel, дескриптор}` на transient `AMHInstancePoolActor`, swap-remove с двумя картами, generation-хэндлы, `ReverseLookup`, owner-операции Hide/Show/Move/Remove, bulk-скоуп; red `Mimir.V5.Composite.Pool.HandleStability/OwnerOperations`; квитанция `docs/receipts/recipe_r5a.md`. Материализация актора/Outliner/Undo ещё не на пуле |
| R5b-0 | MERGED (близнец) | #113 (`dddae77`): reconcile реимпорта на уровне пула: `UMHInstancePoolSubsystem::ReconcileMesh(Mesh, Delta)` (payload/bounds → refresh, descriptor → миграция бакета с сохранением хэндлов и скрытых слотов, collision → physics, binding → overrides), `GetBucketComponents`; red `Mimir.V5.Composite.Pool.ReconcileMesh`; квитанция `docs/receipts/recipe_r5b0.md` |
| R5b-1a | MERGED (близнец) | #115 (`590a5f9`): пул не переиспользует дрейфнувший бакет: `FindOrCreateBucket` сверяет живой компонент с дескриптором и мигрирует через `MigrateBucket`; переносит в пул инвариант `ISM.BucketPolicyRejectsMutatedReuse`; red `Mimir.V5.Composite.Pool.DriftedBucketIsRetired`; квитанция `docs/receipts/recipe_r5b1a.md` |
| R5b-1 | MERGED (близнец) | #116 (`1eda77d`): размещения материализуют static-листья через пул: строки с `FMHInstanceHandle`, компилятор (полный путь `RemoveOwner`+`Add`, reseed/basis/appearance через хэндлы, ISM на акторе нет), актор (`RemoveOwner` при Clear/Destroy/Undo → OPEN-R-1 закрыт, видимость → `SetOwnerEditorVisibility`, `ReconcileEndpoint` принимает мигрированные пулом бакеты), уведомления → `ReconcileMesh` один раз на мир, Outliner через `ReverseLookup`; red `Mimir.V5.Composite.Pool.PlacementMaterializesThroughPool/UndoRestoresPooledPlacement/ReimportMigratesSharedBucketOnce`; квитанция `docs/receipts/recipe_r5b1.md` |
| R5b-2 | NEXT (близнец) | selection-seam вьюпорта: клик по инстансу пула выделяет owner-композит (hit proxy / typed elements через `ReverseLookup`), а не pool-актор | перевод компилятора размещений, Outliner (`ReverseLookup`) и `PostEditUndo` (OPEN-R-1) на пул; `ReconcileEndpoint` актора делегирует меш-дельты пулу |
| R6 | PLANNED | — |
| R7 | PLANNED | — |
| R8 | OPTIONAL | — |

Обновление этой таблицы — обязательная строка acceptance каждого среза.

Правило исполнения (owner 2026-09-02): срезы, которые близнец не делает сам, выполняет внешний исполнитель по контракту из `docs/contracts/`; близнец пишет контракт (ветка + red-тест уже в ветке) и проверяет результат. Каждый срез — отдельная ветка и PR.

Правило DECIDED/STOP (owner 2026-09-05): исполнитель решает на месте и фиксирует `DECIDED-<срез>-N` в контракте, если решение внутри закрытого списка файлов, не трогает тесты/публичный API/resolver/норматив/коды диагностик и при противоречии контракта следует red-тесту; ревьювер принимает или откатывает на ревью. `STOP + OPEN` — только для остального (KICKOFF §7).
