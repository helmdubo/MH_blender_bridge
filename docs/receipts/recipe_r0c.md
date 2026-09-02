# R0c — preview без tag-запросов, duplicate claim в proof-плоскости

Статус: **STOP — OPEN-R0C-1**. Реализация и целевые тесты зелёные, но полный
`Automation RunTests Mimir` блокируется существующим тестом вне закрытого списка
контракта. Ветка не переведена в `IN REVIEW`, PR не создан.

## 1. База и host

- ветка: `recipe/r0c-proof-duplicate-claim`;
- red-коммит контракта: `367820f`; подготовленная ветка до синхронизации:
  `fdd6312`;
- перед реализацией в ветку влит текущий `origin/main` `598b049` (R1.1 #69 и
  tracker #70), merge-коммит `3632d7b`; контракт, закрытые production/test-файлы
  и red-тесты R0c при слиянии не изменились;
- локальный implementation-checkpoint: `7811eab`;
- собственный module-free host:
  `E:\MimirComposite_R0C_External_20260902`, stock UE 5.7.4:
  `D:\PersonalProjects\UE5\UE_5.7`; plugin подключён junction'ом к этой ветке;
- Engine, resolver/runtime module (`MimirCompositeRuntime`), `golden/`,
  `reference/` и Blender-аддон не изменялись.

## 2. Red-first

Сначала собран нетронутый post-merge RED HEAD: лог
`E:\MimirComposite_R0C_External_20260902\Saved\Logs\R0C_RED_BUILD_NONUNITY.log`,
строка 139: `Result: Succeeded`. Первым действием после сборки был запуск
`Automation RunTests Mimir.V5.Composite.Registry`:
`E:\MimirComposite_R0C_External_20260902\Saved\Logs\R0C_RED_TEST.log`.

```text
1086: Test Completed. Result={Fail} Name={DuplicateClaimIsProofPlane}
1090: Expected 'preview resolves the canonical path despite a duplicate claim' to be not null.
1091: Expected 'preview made no tag queries' to be 0, but it was 1.
1092: Expected 'removing the duplicate claim heals Break' to be true.
1103: Test Completed. Result={Fail} Name={IdentityAdmission}
1108: MH_PERF_ENDPOINTS registry two placements: unique_keys=2 registry_lookups=2 asset_registry_tag_queries=2 package_loads_sync=0 identity_admissions=2 live_receipt_tag_reads=0 endpoint_hits=2
1109: Expected 'preview makes no Asset Registry tag queries' to be 0, but it was 2.
```

## 3. Целевой green

После реализации и до разрешённого удаления старого теста выполнен replacement:
`E:\MimirComposite_R0C_External_20260902\Saved\Logs\R0C_GREEN_REPLACEMENT_TEST.log`.

```text
1079: Test Completed. Result={Success} Name={DuplicateClaimIsProofPlane}
1091: Test Completed. Result={Success} Name={IdentityAdmission}
1095: MH_PERF_ENDPOINTS registry two placements: unique_keys=2 registry_lookups=2 asset_registry_tag_queries=0 package_loads_sync=0 identity_admissions=2 live_receipt_tag_reads=0 endpoint_hits=2
```

После удаления заменённого теста три оставшихся `AppliedAdmission.*` зелёные:
`E:\MimirComposite_R0C_External_20260902\Saved\Logs\R0C_APPLIED_ADMISSION_TEST.log`.
Обязательный structural-admission тест:

```text
1093: Test Completed. Result={Success} Name={InvalidRootReceiptBlocksPlanAndBreak}
```

## 4. Гейты

| Gate | Результат |
|---|---|
| non-unity/no-PCH, RED HEAD | PASS — `R0C_RED_BUILD_NONUNITY.log:139`, `Result: Succeeded` |
| non-unity/no-PCH после реализации и удаления старого теста | PASS — `R0C_FINAL_BUILD_NONUNITY.log:19`, `Result: Succeeded` |
| `Registry.DuplicateClaimIsProofPlane` + `Registry.IdentityAdmission` | PASS — `R0C_GREEN_REPLACEMENT_TEST.log:1079,1091`; метрики в строке 1095 |
| три оставшихся `AppliedAdmission.*` | PASS — `R0C_APPLIED_ADMISSION_TEST.log:1079,1093,1111` |
| лексический ноль `MHValidateAppliedCompositeRoot` в `ue/` | PASS — 0 совпадений |
| полный NullRHI `Automation RunTests Mimir` | **FAIL / STOP** — 176 завершённых тестов: 175 Success, 1 Fail; `R0C_FULL_TEST.log:3916` — `Perf.EndpointCounters`; строка 3925 — ожидалось 4 tag-запроса, получено 0 |
| guarded force-unity | NOT RUN — остановка по `OPEN-R0C-1` |
| `BuildPlugin -StrictIncludes -DisableUnity -NoPCH -NoSharedPCH` | NOT RUN — остановка по `OPEN-R0C-1` |
| `git diff --check` | PASS |
| `python tools/check_normative_docs.py` | PASS — `normative docs: OK` |

## 5. Изменённые файлы

Implementation-checkpoint `7811eab` меняет ровно разрешённые контрактом файлы:

- `docs/16_recipe_model.md` — только строка шестой removed-entity в §7.2;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHEndpointPrototypeRegistry.cpp`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Public/Composite/MHCompositeResolvedPlan.h`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositeResolvedPlan.cpp`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositeDefinitionSubsystem.cpp`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositeLevelSubsystem.cpp`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositeRuntimeBridge.cpp`;
- `ue/MimirComposite/Source/MimirCompositeTests/Private/MHCompositeAppliedPlanAdmissionTest.cpp` — только разрешённое удаление одного теста.

Этот receipt добавляет разрешённый `docs/receipts/recipe_r0c.md`.
`docs/RECIPE_EXECUTION_STATUS.md` не изменён: R0c остаётся `READY`, потому что
полный suite не зелёный и PR не открыт.

## 6. Удалённые тесты

| Удалённый тест | Зелёная замена | Доказательство до удаления |
|---|---|---|
| `Mimir.V5.Composite.AppliedAdmission.DuplicateRootClaimBlocksPlanAndBreak` | `Mimir.V5.Composite.Registry.DuplicateClaimIsProofPlane` | `R0C_GREEN_REPLACEMENT_TEST.log:1079`, Success |

Другие тесты не удалялись и не изменялись. Red-тесты
`MHEndpointPrototypeRegistryTest.cpp` не изменялись.

## 7. OPEN-вопросы

### OPEN-R0C-1 — устаревший perf-assert вне закрытого списка

- **Контекст:** контракт R0c требует `asset_registry_tag_queries=0` в preview и
  запрещает менять файлы вне закрытого списка. Полный suite падает только в
  `Mimir.V5.Composite.Perf.EndpointCounters`: лог
  `R0C_FULL_TEST.log:3916,3925`; существующий assert в
  `MimirCompositeTests/Private/MHStaticMeshImporterTest.cpp:1936-1939` требует
  старое правило `Cold.AssetRegistryTagQueries == Cold.RegistryLookups`.
  `MHStaticMeshImporterTest.cpp` отсутствует в закрытом списке; контракт прямо
  говорит: «мешает другой тест — STOP + OPEN в квитанции».
- **Вопрос:** владелец расширяет закрытый список R0c и red-коммит, разрешая
  заменить cold/warm perf-assert'ы на нормативный ноль, или предоставляет иное
  согласованное изменение контракта/теста?
- **Временное fail-closed правило:** `MHStaticMeshImporterTest.cpp` не менять;
  full-suite считать проваленным; не запускать следующие acceptance-гейты, не
  переводить R0c в `IN REVIEW`, не пушить и не открывать PR.
- **Статус:** **OPEN — нужен ответ владельца/близнеца контракта.**

## 8. Строка трекера

Не изменена: `R0c | READY`. Переход в `IN REVIEW` запрещён до закрытия
`OPEN-R0C-1` и зелёного полного suite.
