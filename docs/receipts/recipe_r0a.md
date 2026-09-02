# R0a (Recipe Model v2) — реестр прототипов endpoint'ов с identity-admission

Статус: **READY FOR REVIEW**. Первый из двух срезов R0: механизм резолва
заменён реестром на горячем пути; R0b переименовывает оставшиеся call-site'ы
(Break, runtime snapshot, Outliner) и удаляет символы-фасады.

## 1. База и границы

- ветка: `recipe/r0a-endpoint-registry`; база `origin/main` `739e3a4`
  (после merge D0a #62 и M0 #63);
- host исполнителя `E:\MimirComposite_R_M0_20260902` (см. `recipe_m0.md` §1);
  добавлен `MimirCompositeV5S6.uproject` (копия `HostProject.uproject`),
  чтобы 17 «isolated host» тестов definition pool / parity выполнялись, а не
  `NOT RUN`;
- Engine, `golden/`, `reference/`, Blender-аддон, wire-грамматика, resolver
  (`MimirCompositeRuntime`), runtime-мост не изменялись; тесты не удалялись;
  единственный изменённый существующий тест — M0-тест `Perf.EndpointCounters`
  (§3: заявленное в `recipe_m0.md` следствие R0, asserts ужесточены до цели R0).

Разбиение R0 → R0a/R0b — правило KICKOFF §5 «≤6 изменяемых файлов»: замена
двух функций затрагивает 8 файлов call-site'ов; R0a меняет 5 (§6), R0b — 6.

## 2. Acceptance (KICKOFF v2 §5, строка R0; часть R0a)

| # | Критерий | Результат |
|---|---|---|
| 1 | `UMHEndpointPrototypeRegistry` с identity-admission (16 §2.2, §2.4) | есть: `Composite/MHEndpointPrototypeRegistry.{h,cpp}`; резолв только по каноническому пути; admission — структурный receipt (`UMHStaticMeshImportData` / `UMHMaterialSourceData` / `UMHTextureSourceData` / поля `UMHCompositeAsset`), `LogicalName`, канонические хэши; один раз на ключ за сессию + при `Invalidate` |
| 2 | `MHLoadAppliedResource` и `MHResolveCompositeDefinitionEndpoint` заменены | реализации делегируют реестру (фасады; символы удаляются в R0b); tag-скан Asset Registry на каждый резолв и `FAssetData(&Object)` удалены |
| 3 | red-assert: 2 актора одного ассета → `registry_lookups == uniqueKeys` | GREEN: см. §3 |
| 4 | red-assert: `FAssetData(&Object) == 0` в preview | GREEN: `live_receipt_tag_reads == 0` |
| 5 | red-assert: `GetAssets == 0` в preview | **STOP → OPEN-R-7** (16 §9): существующий тест `AppliedAdmission.DuplicateRootClaimBlocksPlanAndBreak` требует обнаружения дубликата через tag-запрос; fail-closed правило — один запрос на admission ключа; red-assert R0a: `asset_registry_tag_queries == uniqueKeys` |
| 6 | новый `PrototypeRegistryIdentityAdmissionTest`; `AppliedPlanAdmissionTest` остаётся | `Mimir.V5.Composite.Registry.IdentityAdmission` добавлен; все четыре `AppliedAdmission.*` остаются и зелёные |

## 3. Red-first

Тест `Mimir.V5.Composite.Registry.IdentityAdmission`
(`MimirCompositeTests/Private/MHEndpointPrototypeRegistryTest.cpp`).
Часть A — контракт реестра (Ready/hit/Invalid, receipt отсутствует или чужой,
`Invalidate` → Revision++ и повторный резолв); часть B — два размещения одного
ассета (root + 1 mesh = 2 уникальных ключа) через `RebuildComposite`.

RED — коммит `3695329` (реестр и тест есть, горячий путь прежний). Лог
`E:\MimirComposite_R_M0_20260902\Saved\Logs\R0A_RED_TEST.log`, строки 1113–1122:

```text
Test Completed. Result={Fail} Name={IdentityAdmission}
MH_PERF_ENDPOINTS registry two placements: unique_keys=2 registry_lookups=4 asset_registry_tag_queries=4 package_loads_sync=0 identity_admissions=3 live_receipt_tag_reads=3 endpoint_hits=1
Expected 'two placements resolve each unique key once' to be 2, but it was 4.
Expected 'two placements admit each unique key once' to be 2, but it was 3.
Expected 'preview reads no live receipt tags' to be 0, but it was 3.
Expected 'duplicate-claim probe runs once per admission' to be 2, but it was 4.
```

GREEN — коммит `1787660` (+ `feaf953`, см. ниже). Лог
`...\R0A_GREEN_TEST.log`, строки 1086–1232: `Result={Success}` для
`Registry.IdentityAdmission` и всех четырёх `AppliedAdmission.*`:

```text
MH_PERF_ENDPOINTS registry two placements: unique_keys=2 registry_lookups=2 asset_registry_tag_queries=2 package_loads_sync=0 identity_admissions=2 live_receipt_tag_reads=0 endpoint_hits=3
```

Тот же прогон показал заявленное в `recipe_m0.md` §3 следствие: M0-тест
`Perf.EndpointCounters` считал «холодным» проход с инвалидированным definition
cache, но не реестром, поэтому после R0a все резолвы стали hit'ами
(`registry_lookups=0`). Коммит `feaf953` добавляет в холодный проход
`InvalidateAll()` реестра, коммит `6a39379` заменяет assert «tag reads ==
admissions» на цель R0 «tag reads == 0» (cold и warm) — ровно то, что
`recipe_m0.md` §3 обещал сделать в R0. Тот же коммит возвращает definition
endpoint metrics их прежний смысл (только mesh-endpoint'ы): реестр считал в
`EndpointResolves` и root-композит, и isolated-тесты `DefinitionPool.
SharedGraphAcross100Placements` / `DragPreviewAndDropShareGraph` («один
distinct endpoint») давали 2 вместо 1.

## 4. Гейты

| Gate | Результат |
|---|---|
| non-unity/no-PCH editor build (`-NoEngineChanges -WarningsAsErrors`) | RED, GREEN, GREEN2, GREEN3 — `Result: Succeeded` (`R0A_*_BUILD_NONUNITY.log`) |
| `Registry.IdentityAdmission`, `Perf.*`, `AppliedAdmission.*` | Success (`R0A_GREEN_TEST.log`, полный suite ниже) |
| полный NullRHI `Automation RunTests Mimir`, generic host (`HostProject.uproject`) | **175/175 Success**, 0 failed, 17 host-guard `NOT RUN` (`R0A_FULL.log`); +1 тест (`Registry.IdentityAdmission`), удалений нет |
| то же под именем `MimirCompositeV5S6.uproject` (isolated-тесты выполняются, `NOT RUN` = 6) | 173 Success, 2 Fail — `Lifecycle.NoBuildBeforeRegistration`, `Seed.AppearanceMigration` (`R0A_FULL_ISOLATED.log`): не asserts, а движковый `AssetCheck: Error … Static Mesh Asset has no Source Models` при save/reload синтетической фикстуры; **воспроизводится на нетронутом `origin/main` `739e3a4`** в отдельном host'е `E:\MimirComposite_R_BASE_20260902` (`BASE_ISOLATED_TWO.log`) — pre-existing, к срезу не относится; все `DefinitionPool.*` (11) под этим именем зелёные |
| `BuildPlugin -StrictIncludes -DisableUnity -NoPCH -NoSharedPCH` | `BUILD SUCCESSFUL` (`R0A_STRICT_UAT.log`, пакет `E:\MimirComposite_R_R0A_Strict2_20260902`) |
| guarded force-unity (`-NoEngineChanges -ForceUnity -DisableAdaptiveUnity -NoPCH -NoSharedPCH -WarningsAsErrors`) | 10/10 actions, `Result: Succeeded` (`R0A_FORCE_UNITY_UBT.log`) |
| `git diff --check` | PASS |
| `python tools/check_normative_docs.py` | `normative docs: OK` |

## 5. Реализация

- Реестр: `Resolve(Key)` — hit при `Ready` и живом weak-ptr; мёртвый weak
  (GC) → `DeadEndpointReload` + re-admission; `Invalid` не залипает
  (повторный резолв, чтобы починка receipt в памяти лечилась без
  нотификации — это закрепляют `AppliedAdmission.InvalidRootReceipt*`).
  `Invalidate(Key)` — из `MHNotifyGeneratedResourceChanged` и из событий
  Asset Registry `OnAssetsAdded/OnAssetsRemoved` по тегам **события**
  (`MH.Kind`/`MH.LogicalName` из `FAssetData` payload'а, не живого объекта).
  Метрики: M0 `FMHEndpointResolveMetrics` (lookup = admission, tag-query,
  sync load, identity admission) и прежние definition endpoint metrics
  (`EndpointResolves/Hits/Stores/DeadEndpointReloads`) — их закрепляют
  `DefinitionPool.EndpointWeakGcRecovery` / `EndpointReimportInvalidatesEntry`.
- `MHAdmitEndpointIdentity` — чистая проверка полей; вызывается реестром при
  admission и построителем замыкания (`FAppliedPlanBuilder`) на живых объектах
  при каждом построении — это proof-часть, которая уходит из горячего пути в
  R2b. `FinishCompilation` в `FinalizeDeferredMeshes` оставлен как legacy-wait
  (R1 удаляет его своим red-тестом).
- Что удалено из `MHCompositeResolvedPlan.cpp`: `AppliedPlanReceipt`
  (`FAssetData(&Object)`, шесть тегов), `AppliedPlanObjectPath`, tag-скан в
  `MHLoadAppliedResource`, include'ы AssetRegistry. Из
  `MHCompositeDefinitionSubsystem.cpp`: `MatchesDefinitionEndpointIdentity`.
- Не проверяется `ImporterVersion` receipt'а при admission (прежний путь его
  тоже не проверял); ввод новой причины отказа — отдельное решение (OPEN при
  необходимости в R1).

## 6. Изменённые файлы

- `Source/MimirCompositeEditor/Public/Composite/MHEndpointPrototypeRegistry.h` (новый)
- `Source/MimirCompositeEditor/Private/Composite/MHEndpointPrototypeRegistry.cpp` (новый)
- `Source/MimirCompositeEditor/Private/Composite/MHCompositeResolvedPlan.cpp`
- `Source/MimirCompositeEditor/Private/Composite/MHCompositeDefinitionSubsystem.cpp`
- `Source/MimirCompositeEditor/Private/Composite/MHCompositePlacementEvents.cpp`
- тест: `Source/MimirCompositeTests/Private/MHEndpointPrototypeRegistryTest.cpp` (новый)
- тест: `Source/MimirCompositeTests/Private/MHStaticMeshImporterTest.cpp` (M0-тест: холодный проход инвалидирует реестр; asserts tag reads → 0)
- документы: `docs/16_recipe_model.md` (OPEN-R-7, §7.2), `docs/receipts/recipe_r0a.md`

## 7. Вопросы Lead

- **OPEN-R-7** (16 §9) — duplicate claim в preview; ответ нужен до R2c.
- R0b (следующий): `MHCompositeLevelSubsystem` (Break), `MHCompositeRuntimeBridge`
  (snapshot), `MHCompositeOutlinerModel`, `MHCompositePlacementCompiler` →
  прямые вызовы реестра; удаление `MHLoadAppliedResource`,
  `MHResolveCompositeDefinitionEndpoint`, `FMHCompositeDefinitionEntry::Endpoints`.
