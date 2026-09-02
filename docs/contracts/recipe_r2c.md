> Status: NORMATIVE · Architecture version: Recipe Model v2.1 · Supersedes: — · Контракт среза R2c для внешнего исполнителя (близнец, 2026-09-03)

# Контракт R2c — точки выхода proof-плоскости и background proof cache

Основание: KICKOFF §5 (R2c), `docs/16_recipe_model.md` §0 (три плоскости), §2.4
(два уровня admission: source freshness — только proof), §2.6 (точки выхода),
§4 (протокол обновлений), §9 OPEN-R-7 (duplicate claim — proof-плоскость),
OPEN-R2B-1 (гейт удалений R2b-3 = preflight-тест этого среза).

Контекст после R2b (merged #84, #86): preview-плоскость актора строится из
скомпилированного рецепта и **никогда** не строит closure/подписи; единственная
точка выхода, уже делающая proof синхронно, — `BreakComposites` (R2b-2).
Cook/runtime-мост (`MHRuntimeBridgePreSave`, `MHValidateRuntimeCompositeWorld`,
`MHBuildRuntimeCompositeInput`) строит applied graph сам, но только в cook и без
проверки свежести источника. Обычное сохранение карты proof не читает вообще.

## Что уже есть в ветке (не переписывать)

Ветка `recipe/r2c-exit-points` от `origin/main` (после #87). Red-коммит:

- `Public/Composite/MHProofCache.h` — контракт API (менять только по STOP+OPEN):
  `EMHProofState {Unknown, ProofPending, Fresh, Stale, Missing}`,
  `FMHProofResult {State, Diagnostic, Plan}`, `FMHProofAuditRow`,
  `UMHProofCacheSubsystem` (editor subsystem): `GetProofState`, `RequestProof`,
  `FlushPendingProofs`, `BuildProofNow`, `AuditWorld`, `InvalidateAll`,
  `SetSourceHashProviderForTests`;
- `Private/Composite/MHProofCache.cpp` — fail-closed заглушки
  (`MH_E_NOT_IMPLEMENTED`);
- тест `Mimir.V5.Composite.Proof.BuildPreflightFullClosure`
  (`MimirCompositeTests/Private/MHProofCacheTest.cpp`) — **это и есть
  `BuildPreflightFullClosureTest` из KICKOFF §5 R2b/R2c**; после его зелени
  открывается R2b-3.

RED — коммит `94938d7`; лог
`E:\MimirComposite_R_M0_20260902\Saved\Logs\R2C_RED2_TEST.log`, строка 1077
`Result={Fail} Name={BuildPreflightFullClosure}`; падают ровно утверждения на
заглушке (`request schedules a deferred proof`, `flushed proof is Fresh`,
`stale proof is Stale: MH_E_NOT_IMPLEMENTED…`, `missing proof is Missing…`,
`audit lists the placement`); инварианты preview («preview leaves the proof
Unknown», «preview survives a missing unselected receipt») и отказы
`MHValidateRuntimeCompositeWorld`/`MHBuildRuntimeCompositeInput` при
отсутствующем receipt уже зелёные.

## Задача

1. **`UMHProofCacheSubsystem`** (`MHProofCache.cpp`):
   - ключ proof: `(root asset, RecipeRevision реестра рецептов, Seed,
     AppearanceSeed, generation ProjectIndex, ImporterVersion, revision
     endpoint-реестра)`; любой сдвиг ключа → состояние `Unknown` для этого
     placement (кэш не переиспользуется через ключ);
   - `BuildProofNow` = `MHBuildAppliedCompositeGraph` (receipts, identity,
     duplicate claims через `MHCheckGeneratedAssetClaims` — как в
     `MHBuildRuntimeCompositeInput`) → `MHResolveCompositePlan` (Layout →
     Appearance → Proof) → `MHValidateResolvedPlacementTransforms` →
     **freshness**: для каждого ресурса closure сравнить receipt-хэш
     (`Graph.RawHashes[key]`) с текущим хэшем источника; источник хэша —
     `ProjectIndex` (`FMHProjectResourceIndex::Resolve(key).RawHash` при
     `Resolved`), в тестах — provider из `SetSourceHashProviderForTests`;
     ключ неизвестен индексу/provider'у → не Stale. Состояния: applied graph не
     собрался → `Missing` (диагностика applied-плоскости, напр.
     `MH_E_UNRESOLVED_COMPOSITE_REFERENCE: … <key>`); хэш отличается →
     `Stale`, `Diagnostic = "MH_E_STALE_SOURCE: <key> receipt <hash> differs from
     source <hash>"` (код зарегистрировать в `MHDiagnosticRegistry` по правилам
     реестра); иначе `Fresh`. Возврат `true` только для `Fresh`; результат
     кэшируется под ключом во всех трёх случаях;
   - `RequestProof` — ставит placement в очередь и возвращает `ProofPending`
     (или кэшированное состояние, если ключ не менялся); очередь исполняется
     **на game thread вне действия пользователя** (`FTSTicker`, ≤1 placement за
     тик или бюджет по времени) — UObject'ы вне game thread не трогать;
     `FlushPendingProofs` исполняет очередь целиком синхронно;
   - `AuditWorld` — только чтение кэша по всем `AMHCompositeActor` мира
     (`MHRuntimeBridgeFindPlacements` или аналог), **ноль** сборок applied graph;
   - `InvalidateAll` — очистка кэша; вызывать из `MHNotifyGeneratedResourceChanged`
     (любой ключ — proof затронутых placement'ов устарел; допустимо инвалидировать
     всё) и при пересоздании/full scan индекса.
2. **PreSaveWorld (не cook)**: в `MHStartupRuntimeCompositeBridge` (или новый
   хук в `MHProofCache.cpp`) на `FEditorDelegates::PreSaveWorldWithContext` при
   `!Context.IsCooking()`: `AuditWorld` → для каждого не-`Fresh` placement —
   **warning** в Message Log «Mimir» (`MH_W_PROOF_<STATE>`: текст состояния +
   диагностика) и `RequestProof` для `Unknown`; **никакого** синхронного proof и
   никакого отказа сохранения (§2.6 п. 1).
3. **Синхронные точки выхода** (§2.6 п. 2–4) переводятся на `BuildProofNow`:
   `MHValidateRuntimeCompositeWorld` / `MHRuntimeBridgePreflight` (build
   preflight, cook PreSave), `MHBuildRuntimeCompositeInput` (snapshot),
   `BreakComposites` (R2b-2 — заменить локальную сборку proof на
   `BuildProofNow`), экспорт композита (`MHSourceToolMenus`/level subsystem,
   если строит applied graph). Stale → **error** с `MH_E_STALE_SOURCE`
   (preflight/snapshot/export отказывают; Break — тоже, §2.6 п. 4).
4. Cook-путь (`MHRuntimeBridgePreSave` при `IsCooking`) остаётся синхронным
   (явное действие), но идёт через `BuildProofNow`, чтобы stale receipt
   блокировал cook той же диагностикой.
5. Docs: `docs/16_recipe_model.md` §2.6 — привести к реализованным именам
   (`UMHProofCacheSubsystem`, состояния, `MH_E_STALE_SOURCE`, `MH_W_PROOF_*`);
   §9 OPEN-R-7 — статус «реализовано в proof-плоскости (R2c)»; тракер R2c →
   `IN REVIEW`, после merge → `MERGED #<PR>`.

## Закрытый список файлов

- `Private/Composite/MHProofCache.cpp` (реализация)
- `Public/Composite/MHProofCache.h` — только добавление приватных членов/хуков;
  публичный контракт не менять без STOP+OPEN
- `Private/Composite/MHCompositeRuntimeBridge.cpp` (preflight/snapshot/PreSave
  через `BuildProofNow`; non-cook PreSaveWorld warning)
- `Private/Composite/MHCompositeLevelSubsystem.cpp` (Break через `BuildProofNow`;
  экспорт, если применимо)
- `Private/Composite/MHCompositePlacementEvents.cpp` (`InvalidateAll` при
  нотификации)
- `Private/Diagnostics/*` — регистрация `MH_E_STALE_SOURCE`, `MH_W_PROOF_*`
- документы: `docs/16_recipe_model.md` (§2.6, §9), `docs/RECIPE_EXECUTION_STATUS.md`,
  `docs/receipts/recipe_r2c.md`

Тесты не менять. Мешает существующий тест — STOP + OPEN в квитанции, не чинить
тест. `MHCompositeActor.*`, `MHCompiledRecipe.*`, `MHMaterializeLayout.*`,
resolver — не трогать.

## Запрещено

- любое чтение proof cache, `MHBuildAppliedCompositeGraph`, closure или подписей
  из preview-пути (`AMHCompositeActor::RebuildPlacement`, `UpdatePlacementBasis`,
  Outliner);
- синхронный proof в `PreSaveWorld` вне cook; отказ сохранения карты;
- сравнение receipt с Source Root напрямую по файлам (только через ProjectIndex /
  provider);
- `IAssetRegistry::GetAssets` с tag-фильтром вне `MHCheckGeneratedAssetClaims`;
- proof в потоках (UObject-доступ только game thread);
- параллельный старый путь под флагом.

## Acceptance

1. `Mimir.V5.Composite.Proof.BuildPreflightFullClosure` — Success (все пять
   блоков: Unknown после preview; ProofPending → Fresh с closure/подписями и
   паритетом с preview; Stale через provider с `MH_E_STALE_SOURCE`; Missing при
   отсутствующем receipt невыбранного endpoint'а с отказом preflight и
   snapshot; `AuditWorld` без сборки).
2. Новый тест исполнителя `Mimir.V5.Composite.Proof.SaveWarnsWithoutProof`:
   `FEditorDelegates::PreSaveWorldWithContext.Broadcast(World, non-cook
   context)` на мире с placement в состоянии `Unknown` → `BuildAppliedGraph.Calls
   == 0`, состояние стало `ProofPending`, Message Log «Mimir» получил warning
   (проверять через `FMessageLog`-listing или счётчик в subsystem'е).
3. Новый тест исполнителя `Mimir.V5.Composite.Proof.StaleSourceBlocksCookAndSnapshot`:
   provider даёт другой хэш → `MHValidateRuntimeCompositeWorld` false с
   `MH_E_STALE_SOURCE`, `MHBuildRuntimeCompositeInput` false; preview актора без
   ошибки.
4. Существующие `Mimir.V5.Runtime.*`, `Mimir.Audit.MainBaseline.BuildPreflight*`,
   `Mimir.V5.Composite.AppliedAdmission.*`, `Mimir.V5.Composite.Recipe.*` —
   без изменений и зелёные; число тестов не уменьшилось.
5. Полный NullRHI suite — 0 failed (известные pre-existing на isolated-хосте:
   `Lifecycle.NoBuildBeforeRegistration`, `Seed.AppearanceMigration`; flaky
   `Lifecycle.AppearanceCustomDataTransport` — см. `docs/receipts/recipe_r2b_materialize_layout.md` §8).
6. Гейты KICKOFF §9: non-unity/no-PCH, force-unity, `BuildPlugin -StrictIncludes`,
   `git diff --check`, `tools/check_normative_docs.py`.
7. Квитанция `docs/receipts/recipe_r2c.md` (red/green коммиты, строки логов,
   таблица acceptance, гейты, OPEN); PR в `main`; merge — после проверки
   близнеца (независимая сборка и прогон на его хосте).

## Host исполнителя и правила git

См. `docs/contracts/recipe_r1_1.md` §«Host исполнителя». Работать в отдельном
клоне/worktree на ветке `recipe/r2c-exit-points`; никогда не делать `git pull`
чужой ветки, стоя на `main`; никогда не пушить `main`; один PR среза.
