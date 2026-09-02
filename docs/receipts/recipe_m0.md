# M0 (Recipe Model v2) — счётчики резолва endpoint'ов

Статус: **READY FOR REVIEW**. Чистая инструментация: порядок и результат
операций резолва не изменены; production-решения по счётчикам не принимаются.

## 1. База и границы

- ветка: `recipe/m0-endpoint-counters`; база `origin/main` `70342e2`
  (после merge `codex/m0-perf-instrumentation`, PR #60);
- host исполнителя: `E:\MimirComposite_R_M0_20260902\HostProject.uproject`
  (создан `tools/setup_s6_runtime_host.ps1`, junction `Plugins\MimirComposite`
  → worktree; проектный C++-модуль шаблона удалён, так как сборка с
  `-Plugin=` запрещает ссылку Project → Plugin); Engine
  `D:\PersonalProjects\UE5\UE_5.7`, stock, не изменялся;
- audit-host Lead и portfolio-проект owner не открывались;
- `golden/`, `reference/`, Blender-аддон, wire-грамматика, resolver, runtime-мост
  не изменялись; ни один test expectation не менялся.

Ограничения owner: 6 acceptance (§2), 5 изменяемых файлов + 1 тестовый (§6).

## 2. Acceptance (KICKOFF v2 §5, строка M0)

| # | Критерий | Результат |
|---|---|---|
| 1 | `registry_lookups` в `MH_PERF_MAPLOAD` | есть: число вызовов резолва endpoint'а по ключу (сегодня — `MHLoadAppliedResource`; после R0 — lookups реестра) |
| 2 | `package_loads` | есть как `package_loads_sync`: синхронные загрузки, при которых объект не был резидентным до `GetAsset`/`LoadObject` (red-assert R4: `== 0` на cold DDC) |
| 3 | `identity_admissions` | есть: число валидаций receipt живого объекта (сегодня — `AppliedPlanReceipt`; после R0 — identity-admission реестра) |
| 4 | пробы запрещённого в preview-плоскости (16 §2.2) | `asset_registry_tag_queries` (`GetAssets` с tag-фильтром) и `live_receipt_tag_reads` (`FAssetData(&Object)`) — red-asserts R0: оба `== 0` |
| 5 | red-first | RED на `0b79105`, GREEN на `3cdc639` (§3) |
| 6 | гейты §9 | §4 |

## 3. Red-first

Тест `Mimir.V5.Composite.Perf.EndpointCounters`
(`MimirCompositeTests/Private/MHStaticMeshImporterTest.cpp`): random-композит с
двумя mesh-опциями, два `AMHCompositeActor` одного ассета, seed 7. Три прогона:
`mh.PerfTrace 0` (отчёта нет, счётчики 0), `mh.PerfTrace 1` холодный
(definition cache инвалидирован) и тёплый (второй актор).

RED — коммит `0b79105` (структура счётчиков, дельты в отчёте, тест; точек
вызова нет). Лог `E:\MimirComposite_R_M0_20260902\Saved\Logs\M0R_RED_TEST.log`,
строки 1102–1116:

```text
Test Completed. Result={Fail} Name={EndpointCounters}
Expected 'cold build resolves at least every unique endpoint key' to be true.
Expected 'cold build admits at least every unique endpoint key' to be true.
Expected 'warm build resolves fewer endpoints than cold' to be true.
Expected 'warm build admits fewer endpoints than cold' to be true.
MH_PERF_ENDPOINTS cold: unique_keys=3 registry_lookups=0 asset_registry_tag_queries=0 package_loads_sync=0 identity_admissions=0 live_receipt_tag_reads=0; warm: registry_lookups=0 ...
```

GREEN — коммит `3cdc639` (точки вызова в `MHLoadAppliedResource` и
`AppliedPlanReceipt`). Лог `...\Saved\Logs\M0R_GREEN_TEST.log`, строки
1089–1142: `Result={Success}` для `EndpointCounters` и для прежнего
`InstrumentationCounters`.

```text
MH_PERF_ENDPOINTS cold: unique_keys=3 registry_lookups=5 asset_registry_tag_queries=5 package_loads_sync=0 identity_admissions=4 live_receipt_tag_reads=4; warm: registry_lookups=1 asset_registry_tag_queries=1 package_loads_sync=0 identity_admissions=1 live_receipt_tag_reads=1
```

Чтение чисел (это и есть baseline для R0): при трёх уникальных ключах
(root composite + 2 mesh-опции) текущий путь делает 5 резолвов и 5 запросов
Asset Registry по тегам (root резолвится дважды — валидация root и обход
closure; материал меша — ещё один ключ), 4 валидации receipt через
`FAssetData(&Object)`. Тёплый актор того же ассета всё равно стоит 1 резолв +
1 admission (`MHValidateAppliedCompositeRoot` перед cache hit). R0 обязан дать
`registry_lookups == uniqueKeys`, `asset_registry_tag_queries == 0`,
`live_receipt_tag_reads == 0`.

## 4. Гейты

| Gate | Результат |
|---|---|
| non-unity/no-PCH editor build (`-DisableUnity -NoPCH -NoSharedPCH -NoEngineChanges -WarningsAsErrors`) | RED и GREEN коммиты: `Result: Succeeded`, 0 warnings (`M0R_RED_BUILD_NONUNITY.log`, `M0R_GREEN_BUILD_NONUNITY.log`) |
| `Mimir.V5.Composite.Perf.*` на GREEN | 2/2 Success (`M0R_GREEN_TEST.log`) |
| полный NullRHI `Automation RunTests Mimir` с `-MHGoldenRoot=<repo>/golden` | **174/174 Success**, 0 failed (`M0R_FULL.log`; 17 host-guard кейсов информационно `NOT RUN`, как в M0); число тестов выросло на 1 (`EndpointCounters`), ни один не удалён |
| `BuildPlugin -StrictIncludes -DisableUnity -NoPCH -NoSharedPCH` | `BUILD SUCCESSFUL`, editor + Development game + Shipping game (`M0R_STRICT_UAT.log`, пакет `E:\MimirComposite_R_M0_Strict_20260902`) |
| guarded force-unity (`-NoEngineChanges -ForceUnity -DisableAdaptiveUnity -NoPCH -NoSharedPCH -WarningsAsErrors`) | 14/14 actions, `Result: Succeeded`, 0 warnings (`M0R_FORCE_UNITY_UBT.log`) |
| `git diff --check` | PASS |
| документальный CI `tools/check_normative_docs.py` | не применим: скрипт живёт в ветке D0a (PR #62), в базе среза его нет |

## 5. Реализация

- `MHCompositePlacementMetrics.{h,cpp}`: `FMHEndpointResolveMetrics`
  (`RegistryLookups`, `AssetRegistryTagQueries`, `PackageLoadsSync`,
  `IdentityAdmissions`, `LiveReceiptTagReads`) — сессионные счётчики того же
  класса, что definition/mutation metrics; reset/get/record API.
- `MHPerformanceTrace.{h,cpp}`: пять полей в `FMHMapLoadPerfReport`, дельты
  before/after в `FMHMapLoadInitialBuildScope::Complete`, пять полей в строке
  `MH_PERF_MAPLOAD` после `selected_meshes_compiling`.
- `MHCompositeResolvedPlan.cpp`: `MHLoadAppliedResource` записывает lookup,
  tag-query и sync-load (объект не резидентен до `GetAsset`/`LoadObject`);
  `AppliedPlanReceipt` — admission и live tag read. Поток управления и
  возвращаемые значения не изменены.
- Отчёт `MH_PERF_REIMPORT` не расширялся: R3 опирается на уже существующие
  `notified_actors`/`actor_rebuild_ms_total`.

## 6. Изменённые файлы

- `Source/MimirCompositeEditor/Public/Composite/MHCompositePlacementMetrics.h`
- `Source/MimirCompositeEditor/Private/Composite/MHCompositePlacementMetrics.cpp`
- `Source/MimirCompositeEditor/Public/Performance/MHPerformanceTrace.h`
- `Source/MimirCompositeEditor/Private/Performance/MHPerformanceTrace.cpp`
- `Source/MimirCompositeEditor/Private/Composite/MHCompositeResolvedPlan.cpp`
- тест: `Source/MimirCompositeTests/Private/MHStaticMeshImporterTest.cpp`
- `docs/receipts/recipe_m0.md`

## 7. Вопросы Lead

Открытых архитектурных вопросов нет. Замечание для R0: тёплый актор одного
ассета сегодня стоит 1 lookup + 1 admission из-за валидации root перед cache
hit; R0 переносит её в identity-admission реестра (один раз на ключ за сессию),
иначе `registry_lookups == uniqueKeys` для двух акторов недостижимо.
