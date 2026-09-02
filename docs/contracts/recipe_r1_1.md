> Status: NORMATIVE · Architecture version: Recipe Model v2.1 · Supersedes: — · Контракт среза R1.1 для внешнего исполнителя (пишет и принимает близнец)

# Контракт R1.1 — acceptance R1 по формуле D0b П6

## Что уже есть в ветке (не переписывать)

Ветка `recipe/r1-1-selected-wait-sets` от `origin/main` `8597057`. Коммит
`bfb4d4d` содержит **red-тест** и test-API отчёта:

- `FMHMapLoadPerfReport` получил три сортированных списка ключей
  (`WaitedMeshKeys`, `SelectedCompilingMeshKeys`, `UnselectedMeshKeys`),
  заполняемых при flush; в лог они не пишутся.
- `Mimir.V5.Composite.Perf.SelectedMeshWait` теперь утверждает:
  `waited_mesh_set == selected_compiling_mesh_set` и
  `waited_mesh_set ∩ unselected_mesh_set == ∅` (П6). Отношение
  `all_option/selected` остаётся метрикой, не условием.

RED на `bfb4d4d`: лог
`E:\MimirComposite_R_M0_20260902\Saved\Logs\R1_1_RED_TEST.log` (строки см. в
квитанции). Причина: `PlanViewWaitSelectedMeshes` записывает в `waited` каждый
выбранный меш, компилируется он или нет, а во множество `selected_compiling`
попадают только компилирующиеся.

## Задача

Сделать red-тест зелёным, изменив **семантику записи**, а не тест: в `waited`
попадают только те меши, которые реально переданы `FinishCompilation`
(т.е. `IsCompiling()` в момент ожидания). Счётчик `waited_meshes` в строке
`MH_PERF_MAPLOAD` при этом означает то же самое (число реально ожидавшихся
мешей) — обновить комментарий поля в `MHPerformanceTrace.h`.

## Закрытый список файлов

Разрешено менять только:

1. `ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositePlacementCompiler.cpp`
   (`PlanViewWaitSelectedMeshes`);
2. `ue/MimirComposite/Source/MimirCompositeEditor/Public/Performance/MHPerformanceTrace.h`
   (только комментарий к `WaitedMeshes`);
3. `docs/receipts/recipe_r1_1.md` (новая квитанция);
4. `docs/RECIPE_EXECUTION_STATUS.md` (строка R1.1 → `MERGED #<PR>` после merge,
   до merge — `IN REVIEW`).

Тест `MHStaticMeshImporterTest.cpp` не менять. Если он мешает — STOP и
OPEN-вопрос в квитанции, а не правка теста.

## Запрещено

- `FinishCompilation` вне `PlanViewWaitSelectedMeshes`; любой `FinishCompilation`
  в построителе замыкания (`MHCompositeResolvedPlan.cpp`);
- чтение Asset Registry тегов, `FAssetData(&Object)`, `GetAssets` в preview;
- параллельный «старый путь» под флагом;
- изменение resolver'а (`MimirCompositeRuntime`), `golden/`, Engine.

## Acceptance

1. `Mimir.V5.Composite.Perf.SelectedMeshWait` — `Result={Success}` (green-лог
   с номерами строк в квитанции).
2. `Mimir.V5.Composite.Perf.*` и полный NullRHI `Automation RunTests Mimir`
   на generic host — 0 failed; число тестов не уменьшилось.
3. Гейты KICKOFF §9: non-unity/no-PCH build, force-unity, `BuildPlugin
   -StrictIncludes`, `git diff --check`, `python tools/check_normative_docs.py`.
4. Квитанция `docs/receipts/recipe_r1_1.md`: база, red/green строки, гейты,
   изменённые файлы (ровно из списка выше), обновление тракера.
5. PR в `main` из этой ветки; merge — только после проверки близнеца.

## Host исполнителя

Собственный host по `tools/setup_s6_runtime_host.ps1` (удалить `Source/`
шаблона и завести module-free `HostProject.uproject`, см.
`docs/receipts/recipe_m0.md` §1). Audit-host Lead и portfolio-проект owner
не трогать. UBT-сборки и `BuildPlugin` не запускать одновременно (мьютекс).
