# R0c — preview без tag-запросов, duplicate claim в proof-плоскости

Статус: **READY FOR REVIEW**. Tag-проверка duplicate claim удалена из
preview-реестра и выполняется в proof-точках Break/runtime snapshot; старый
applied-admission тест удалён после зелёной замены.

## 1. База и границы

- ветка: `recipe/r0c-proof-duplicate-claim`;
- исходный red-коммит: `367820f`; перед реализацией в ветку влит
  `origin/main` `598b049`, merge-коммит `3632d7b`;
- implementation-коммит исполнителя: `7811eab`; первая STOP-квитанция:
  `d184809`;
- `OPEN-R0C-1` закрыт близнецом 2026-09-02, расширенный red-коммит `4f8e069`:
  `Perf.EndpointCounters` теперь нормативно требует ноль preview tag-запросов;
  исполнитель этот тест не менял;
- собственный module-free host:
  `E:\MimirComposite_R0C_External_20260902`, stock UE 5.7.4:
  `D:\PersonalProjects\UE5\UE_5.7`; plugin подключён junction'ом к этой ветке;
- Engine, resolver/runtime module (`MimirCompositeRuntime`), `golden/`,
  `reference/`, Blender-аддон и форматы индекса/receipt/хэшей не изменялись;
- при финализации удалённые refs `main` и
  `recipe/r0c-proof-duplicate-claim` уже указывали на `4f8e069` вследствие
  внешнего push. Исполнитель не пушил в `main`; поэтому PR содержит только
  финальную квитанцию и перевод трекера, а implementation уже присутствует в
  целевой ветке.

## 2. Red-first

Нетронутый post-merge RED HEAD сначала собран non-unity/no-PCH:
`E:\MimirComposite_R0C_External_20260902\Saved\Logs\R0C_RED_BUILD_NONUNITY.log:139`,
`Result: Succeeded`. Первым действием после сборки был
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

Дополнительный red для закрытого `OPEN-R0C-1` — полный прогон до расширения
red-коммита:
`E:\MimirComposite_R0C_External_20260902\Saved\Logs\R0C_FULL_TEST.log`.

```text
3916: Test Completed. Result={Fail} Name={EndpointCounters}
3925: Expected 'current resolve path queries the Asset Registry by tags once per lookup' to be 4, but it was 0.
```

## 3. Green

После `4f8e069` фильтр `Mimir.V5.Composite.Perf` зелёный:
`E:\MimirComposite_R0C_External_20260902\Saved\Logs\R0C_POSTOPEN_PERF.log`.

```text
1096: Test Completed. Result={Success} Name={EndpointCounters}
1102: MH_PERF_ENDPOINTS cold: unique_keys=3 registry_lookups=4 asset_registry_tag_queries=0 package_loads_sync=0 identity_admissions=4 live_receipt_tag_reads=0; warm: registry_lookups=0 asset_registry_tag_queries=0 package_loads_sync=0 identity_admissions=0 live_receipt_tag_reads=0
1143: Test Completed. Result={Success} Name={InstrumentationCounters}
1172: Test Completed. Result={Success} Name={SelectedMeshWait}
```

Фильтр `Mimir.V5.Composite.Registry` зелёный:
`E:\MimirComposite_R0C_External_20260902\Saved\Logs\R0C_POSTOPEN_REGISTRY.log`.

```text
1080: Test Completed. Result={Success} Name={DuplicateClaimIsProofPlane}
1092: Test Completed. Result={Success} Name={IdentityAdmission}
1096: MH_PERF_ENDPOINTS registry two placements: unique_keys=2 registry_lookups=2 asset_registry_tag_queries=0 package_loads_sync=0 identity_admissions=2 live_receipt_tag_reads=0 endpoint_hits=2
```

До разрешённого удаления старого теста replacement был отдельно подтверждён
зелёным в `R0C_GREEN_REPLACEMENT_TEST.log:1079`. После удаления три оставшихся
`AppliedAdmission.*` зелёные; обязательный structural-admission тест:
`R0C_APPLIED_ADMISSION_TEST.log:1093`, `InvalidRootReceiptBlocksPlanAndBreak`,
`Result={Success}`.

## 4. Гейты

| Gate | Результат |
|---|---|
| non-unity/no-PCH (`-NoEngineChanges -WarningsAsErrors`) | PASS — `R0C_POSTOPEN_BUILD_NONUNITY.log:44`, `Result: Succeeded` |
| `Mimir.V5.Composite.Perf` | PASS — 3/3 Success, `R0C_POSTOPEN_PERF.log:1096,1143,1172` |
| `Mimir.V5.Composite.Registry` | PASS — 2/2 Success, `R0C_POSTOPEN_REGISTRY.log:1080,1092` |
| полный NullRHI `Automation RunTests Mimir` | PASS — **176/176 Success, 0 failed**, `R0C_POSTOPEN_FULL.log`; `EndpointCounters:3879`, replacement `DuplicateClaimIsProofPlane:3972`, последний завершённый тест `PlacementProfileFreshness:4624` |
| лексический ноль `MHValidateAppliedCompositeRoot` в `ue/` | PASS — 0 совпадений |
| guarded force-unity (`-ForceUnity -DisableAdaptiveUnity`) | PASS — 14/14 actions, `R0C_POSTOPEN_FORCEUNITY.log:31`, `Result: Succeeded` |
| `BuildPlugin -StrictIncludes -DisableUnity -NoPCH -NoSharedPCH` | PASS — `R0C_POSTOPEN_STRICT.log:226`, `BUILD SUCCESSFUL`; package `E:\MimirComposite_R0C_Strict_PostOpen_20260902` |
| `git diff --check` | PASS |
| `python tools/check_normative_docs.py` | PASS — `normative docs: OK` |

## 5. Реализация

- `UMHEndpointPrototypeRegistry::Admit` больше не делает `GetAssets`/tag-filter:
  preview резолвит только канонический путь через `FindObject` → `LoadObject`.
- `MHCheckGeneratedAssetClaims` выполняет duplicate/path admission в
  proof-плоскости и вызывается для root и ресурсов плана в Build, Break и
  runtime snapshot до materialization/bindings.
- `MHValidateAppliedCompositeRoot` удалён; definition subsystem выполняет
  structural identity admission и проверяет, что canonical registry resolve
  возвращает тот же root.
- строка шестой removed-entity в `docs/16_recipe_model.md` переведена с `R2c`
  на `R0c`.

## 6. Изменённые файлы

Implementation-коммит `7811eab` меняет разрешённые контрактом файлы:

- `docs/16_recipe_model.md`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHEndpointPrototypeRegistry.cpp`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Public/Composite/MHCompositeResolvedPlan.h`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositeResolvedPlan.cpp`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositeDefinitionSubsystem.cpp`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositeLevelSubsystem.cpp`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositeRuntimeBridge.cpp`;
- `ue/MimirComposite/Source/MimirCompositeTests/Private/MHCompositeAppliedPlanAdmissionTest.cpp` — только разрешённое удаление одного теста.

Документальный коммит меняет разрешённые:

- `docs/receipts/recipe_r0c.md`;
- `docs/RECIPE_EXECUTION_STATUS.md` — только строка R0c → `IN REVIEW`.

`4f8e069` близнеца меняет контракт и
`MimirCompositeTests/Private/MHStaticMeshImporterTest.cpp`; исполнитель эти
файлы не редактировал.

## 7. Удалённые тесты

| Удалённый тест | Зелёная замена | Доказательство до удаления |
|---|---|---|
| `Mimir.V5.Composite.AppliedAdmission.DuplicateRootClaimBlocksPlanAndBreak` | `Mimir.V5.Composite.Registry.DuplicateClaimIsProofPlane` | `R0C_GREEN_REPLACEMENT_TEST.log:1079`, Success |

Другие тесты не удалялись. Red-тесты исполнителем не изменялись.

## 8. OPEN-вопросы и трекер

- **OPEN-R0C-1 закрыт близнецом 2026-09-02, red-коммит `4f8e069`.**
- Незакрытых OPEN-вопросов нет.
- Строка трекера R0c переведена в `IN REVIEW`; merge остаётся за близнецом.
