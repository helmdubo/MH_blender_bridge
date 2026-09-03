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
- `MimirCompositeRuntime/Private/Diagnostics/MHDiagnosticRegistry.cpp` — регистрация
  `MH_E_STALE_SOURCE` и четырёх `MH_W_PROOF_{UNKNOWN,PENDING,STALE,MISSING}`
  (точные имена; реестр закреплён тестом — см. OPEN-R2C-2)
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
2. `Mimir.V5.Composite.Proof.SaveWarnsWithoutProof` (red-тест близнеца, в
   ветке): non-cook `PreSaveWorldWithContext` на мире с placement `Unknown` →
   `BuildAppliedGraph.Calls == 0`, состояние `ProofPending`,
   `GetLastSaveAuditWarningCount() >= 1`; после `FlushPendingProofs` — `Fresh`,
   повторный save без warning'ов и без сборок.
3. `Mimir.V5.Composite.Proof.StaleSourceBlocksCookAndSnapshot` (red-тест
   близнеца, в ветке): provider даёт другой хэш выбранного меша →
   `MHValidateRuntimeCompositeWorld` false с `MH_E_STALE_SOURCE` и ключом,
   `MHBuildRuntimeCompositeInput` false с кодом, кэш `Stale`, preview без ошибки;
   после снятия provider'а и `InvalidateAll` preflight снова проходит.
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

## Ответ на OPEN-R2C-1 (близнец, 2026-09-03): red-тесты пишет близнец

Находка исполнителя верна: Acceptance 2–3 требовали тесты, которых в ветке не
было, при запрете менять тесты. По правилу программы (KICKOFF v2, матрица
делегирования) red-тесты пишет близнец, и они уже в ветке — коммит
`4b38f7a` поверх `b0c5ebb`:

- `Mimir.V5.Composite.Proof.SaveWarnsWithoutProof` и
  `Mimir.V5.Composite.Proof.StaleSourceBlocksCookAndSnapshot` добавлены в
  `MHProofCacheTest.cpp`;
- в API `UMHProofCacheSubsystem` добавлен один наблюдаемый счётчик
  `GetLastSaveAuditWarningCount()` (private `LastSaveAuditWarningCount`): хук
  PreSaveWorld вне cook обязан выставлять его = числу не-`Fresh` placement'ов
  аудита (и 0, когда warning'ов нет).

Закрытый список файлов не меняется: тесты по-прежнему не трогать. Acceptance
2–3 переформулированы под существующие тесты. RED-лог:
`E:\MimirComposite_R_M0_20260902\Saved\Logs\R2C_RED3_TEST.log`, строки 1080/1098/1109 — `Result={Fail}` для
`BuildPreflightFullClosure`, `SaveWarnsWithoutProof`,
`StaleSourceBlocksCookAndSnapshot`; падают только утверждения на заглушке
(`save schedules the missing proof`, `save warned…`, `deferred proof became
Fresh`, `stale source refuses cook preflight/snapshot`, `cache reports Stale`);
«save builds no proof» и «fresh world passes preflight» уже зелёные.

OPEN-R2C-1: закрыт.

## Ответ на OPEN-R2C-2 (близнец, 2026-09-03): реестр диагностик закреплён red-коммитом

Находка верна: `Mimir.V5.Random.StreamTraceAndSignatureParity` фиксирует точные
размеры реестра (53/16), и любая регистрация ломала бы неизменяемый тест.
Red-коммит близнеца `14a2d7d` (поверх `5276884`) переводит ожидания
на цель R2c: **54 errors / 20 warnings** и явные проверки
`MH_E_STALE_SOURCE`, `MH_W_PROOF_UNKNOWN`, `MH_W_PROOF_PENDING`,
`MH_W_PROOF_STALE`, `MH_W_PROOF_MISSING`. Тест теперь красный, пока коды не
зарегистрированы; регистрация — в
`MimirCompositeRuntime/Private/Diagnostics/MHDiagnosticRegistry.cpp` (добавлен в
закрытый список выше; это единственная правка Runtime-модуля в срезе). Других
кодов не добавлять: счётчики точные.

RED-лог: `E:\MimirComposite_R_M0_20260902\Saved\Logs\R2C_RED4_TEST.log`,
строка 1076 `Result={Fail} Name={StreamTraceAndSignatureParity}`:
`exact registered MH_E count to be 54, but it was 53`, `exact registered MH_W
count to be 20, but it was 16` и пять `… is registered`; остальные проверки
теста зелёные.

OPEN-R2C-2: закрыт.
