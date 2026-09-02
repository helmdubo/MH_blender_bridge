# R1.1 (Recipe Model v2.1) — selected compiling mesh wait sets

## 1. База и границы

- ветка: `recipe/r1-1-selected-wait-sets`; база `origin/main` `8597057`;
- red-коммит: `bfb4d4d`; контракт и ссылка на owner red-log: `d15f5b0`;
- коммит реализации: `8524a25`;
- host исполнителя: `E:\MimirComposite_R1_1_External_20260902\HostProject.uproject`;
  создан `tools/setup_s6_runtime_host.ps1`, шаблонный `Source/` удалён,
  `HostProject.uproject` module-free, `Plugins/MimirComposite` подключён junction'ом
  к этому checkout;
- Engine: stock UE 5.7.4, `D:\PersonalProjects\UE5\UE_5.7`; Engine не изменялся;
- audit-host Lead и portfolio-проект owner не открывались;
- resolver, runtime-мост, `golden/`, `reference/`, Blender-аддон, wire-форматы и
  red-тест `MHStaticMeshImporterTest.cpp` не изменялись.

## 2. Red-first

Тест: `Mimir.V5.Composite.Perf.SelectedMeshWait`.

Собственная non-unity/no-PCH сборка RED HEAD прошла: лог
`E:\MimirComposite_R1_1_External_20260902\Saved\Logs\R1_1_RED_BUILD_NONUNITY.log`,
строка 139: `Result: Succeeded`.

RED-лог:
`E:\MimirComposite_R1_1_External_20260902\Saved\Logs\R1_1_RED_TEST.log`.

```text
1098: Test Completed. Result={Fail} Name={SelectedMeshWait}
1108: waited=[static_mesh:targeted_mesh_..._second] selected_compiling=[] unselected=[static_mesh:targeted_mesh_...]
1109: Expected 'waited_mesh_set == selected_compiling_mesh_set' to be true.
```

Строка 1093 `MH_PERF_MAPLOAD`: `selected_meshes_compiling=0 waited_meshes=1`.

GREEN-лог:
`E:\MimirComposite_R1_1_External_20260902\Saved\Logs\R1_1_GREEN_TEST.log`.

```text
1092: Test Completed. Result={Success} Name={SelectedMeshWait}
1099: waited=[] selected_compiling=[] unselected=[static_mesh:targeted_mesh_...]
```

Строка 1087 `MH_PERF_MAPLOAD`: `selected_meshes_compiling=0 waited_meshes=0`.

## 3. Реализация

- `PlanViewWaitSelectedMeshes` записывает ключ в waited-set только внутри
  `Pair.Value != nullptr && Pair.Value->IsCompiling()`, непосредственно перед
  добавлением того же меша в массив `FinishCompilation`;
- комментарий `FMHMapLoadPerfReport::WaitedMeshes` фиксирует, что счётчик означает
  уникальные выбранные меши, реально переданные `FinishCompilation` во время
  компиляции;
- тестовый API, test expectations и production-пути не менялись.

## 4. Гейты

| Gate | Результат |
|---|---|
| non-unity/no-PCH editor build, `-NoEngineChanges -WarningsAsErrors` | PASS — `R1_1_GREEN_BUILD_NONUNITY.log`, строка 35: `Result: Succeeded` |
| focused `Mimir.V5.Composite.Perf.SelectedMeshWait` | PASS — `R1_1_GREEN_TEST.log`, строка 1092 |
| `Mimir.V5.Composite.Perf` | PASS — 3/3 Success, 0 failed; `R1_1_PERF_TEST.log`, строки 1095, 1142, 1171 |
| полный NullRHI `Automation RunTests Mimir`, generic host | PASS — 176/176 `Test Completed` Success, 0 failed; `R1_1_FULL_TEST.log`, строки 1278–4666; R1.1 на строке 4005 |
| guarded force-unity, adaptive unity off, no-PCH | PASS — `R1_1_FORCE_UNITY_BUILD.log`, строка 31: `Result: Succeeded` |
| `BuildPlugin -StrictIncludes -DisableUnity -NoPCH -NoSharedPCH` | PASS — `R1_1_STRICT_INCLUDES.log`, строка 226: `BUILD SUCCESSFUL`; package `E:\MimirComposite_R1_1_Strict_20260902` |
| `git diff --check` | PASS |
| `python tools/check_normative_docs.py` | PASS — `normative docs: OK` |

Полный suite содержит 17 явных generic-host guard сообщений `NOT RUN`; все 176
запущенных тестов завершились `Result={Success}`. Реализация не меняет тестовые
файлы, поэтому число тестов не уменьшилось.

## 5. Изменённые файлы

| Файл | Изменение |
|---|---|
| `ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositePlacementCompiler.cpp` | waited-set записывается только для реально ожидаемого compiling mesh |
| `ue/MimirComposite/Source/MimirCompositeEditor/Public/Performance/MHPerformanceTrace.h` | уточнён комментарий `WaitedMeshes` |
| `docs/receipts/recipe_r1_1.md` | эта квитанция |
| `docs/RECIPE_EXECUTION_STATUS.md` | строка R1.1 переведена в `IN REVIEW` |

Все файлы входят в закрытый список контракта.

## 6. Удалённые тесты

Нет. Red-тест не изменялся и не удалялся.

## 7. OPEN-вопросы

Нет.

## 8. Трекер

`R1.1 | IN REVIEW` — PR в `main`, merge выполняет только owner после проверки.
