# R1 (Recipe Model v2) — ожидание компиляции только выбранных мешей

Статус: **READY FOR REVIEW**. Переходный срез (KICKOFF §5 R1): closure по-прежнему
загружает все варианты (их receipt'ы нужны proof-хэшам до фазового разделения
R2a), но ждёт компиляцию только тех мешей, которые выбрал сид. Полное снятие
ожидания — R4 (заглушки, async).

## 1. База и границы

- ветка: `recipe/r1-selected-wait`; база `origin/main` `eba46ec` (после merge R0b #65);
- host исполнителя `E:\MimirComposite_R_M0_20260902`;
- Engine, `golden/`, resolver, runtime-мост не изменялись; тесты не удалялись.

## 2. Acceptance (KICKOFF v2 §5, строка R1)

| # | Критерий | Результат |
|---|---|---|
| 1 | closure не ждёт компиляцию | `FinishCompilation` удалён из построителя замыкания (`AdmitDeferredMeshes` — только admission receipt + hash) |
| 2 | ожидание — только выбранные меши | placement compiler после `LoadEndpoints` собирает уникальные меши выбранных листьев и ждёт только их (`PlanViewWaitSelectedMeshes`), обе ветви: полная компиляция и incremental reseed |
| 3 | red-assert `all_option_unique_meshes ≫ waited_meshes == selected_unique_meshes` | новый счётчик `waited_meshes` в `MH_PERF_MAPLOAD`; тест `Mimir.V5.Composite.Perf.SelectedMeshWait`: 2 опции → `waited_meshes == 1 == selected` |
| 4 | сущность `FinalizeDeferredMeshes` (16 §7.2) удалена | переименована в `AdmitDeferredMeshes` после снятия ожидания; 0 вхождений в `ue/` |
| 5 | гейты §9 | §4 |

## 3. Red-first

RED — коммит `d8ec5b0`: `waited_meshes` считается на существующем шаге ожидания
(весь deferred-набор closure = все опции). Лог
`E:\MimirComposite_R_M0_20260902\Saved\Logs\R1_RED_TEST.log`, строки 1093–1104:

```text
Test Completed. Result={Fail} Name={SelectedMeshWait}
MH_PERF_SELECTED_WAIT all_option_unique_meshes=2 selected_unique_meshes=1 waited_meshes=2 wait_static_mesh_compilation_ms=0.000
Expected 'only the selected mesh reaches the compilation wait' to be 1, but it was 2.
Expected 'unselected options never reach the compilation wait' to be true.
```

GREEN — коммит `96f793c`. Лог `...\R1_GREEN_TEST.log`, строки 1096–1178:
`Result={Success}` для `SelectedMeshWait`, `EndpointCounters`, `InstrumentationCounters`:

```text
MH_PERF_SELECTED_WAIT all_option_unique_meshes=2 selected_unique_meshes=1 waited_meshes=1 wait_static_mesh_compilation_ms=0.000
```

## 4. Гейты

| Gate | Результат |
|---|---|
| non-unity/no-PCH editor build (`-NoEngineChanges -WarningsAsErrors`) | RED и GREEN — `Result: Succeeded` (`R1_RED_BUILD_NONUNITY.log`, `R1_GREEN_BUILD_NONUNITY.log`) |
| полный NullRHI `Automation RunTests Mimir`, generic host (`HostProject.uproject`) | **176/176 Success**, 0 failed (`R1_FULL.log`); +1 тест, удалений нет |
| то же под именем `MimirCompositeV5S6.uproject` | 174 Success, 2 Fail — pre-existing `AssetCheck`-провалы (см. `recipe_r0a.md` §4, воспроизведены на `origin/main`) (`R1_FULL_ISOLATED.log`) |
| `BuildPlugin -StrictIncludes -DisableUnity -NoPCH -NoSharedPCH` | `ExitCode=0 (Success)` (`R1_STRICT_UAT.log`) |
| guarded force-unity | 12/12 actions, `Result: Succeeded` (`R1_FORCE_UNITY_UBT.log`) |
| `git diff --check` / `python tools/check_normative_docs.py` | PASS / `normative docs: OK` |

## 5. Изменённые файлы

- `Public/Performance/MHPerformanceTrace.h`, `Private/Performance/MHPerformanceTrace.cpp`
  (`WaitedMeshes`, `MHRecordMapLoadWaitedMesh`, поле `waited_meshes`);
- `Private/Composite/MHCompositeResolvedPlan.cpp` (ожидание снято; `AdmitDeferredMeshes`);
- `Private/Composite/MHCompositePlacementCompiler.cpp` (`PlanViewWaitSelectedMeshes`);
- тест: `MimirCompositeTests/Private/MHStaticMeshImporterTest.cpp` (`Perf.SelectedMeshWait`);
- `docs/receipts/recipe_r1.md`.

## 6. Вопросы Lead

Нет новых. `wait_static_mesh_compilation_ms` теперь измеряет только выбранные
меши; целевое `== 0` (R4) остаётся в программе.
