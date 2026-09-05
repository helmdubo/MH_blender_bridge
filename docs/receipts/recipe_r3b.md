# R3b — STOP-квитанция OPEN-R3B-1

## База и статус

- Дата: 2026-09-05; ветка `codex/recipe-r3b-resource-reconcile` создана
  от `origin/recipe/r3b-resource-reconcile`, HEAD базы `2f4bda3`.
- Red-коммит близнеца: `e00d80a`; база среза: `a519e6a`.
- PR: [#108](https://github.com/helmdubo/MH_blender_bridge/pull/108), **draft / STOP**.
- `4ba0b62` содержит только раздел `OPEN-R3B-1` в контракте.
- До первой правки прочитаны контракт целиком, docs/16 §4 и §2.2,
  правило owner DECIDED/STOP. Реализация и тесты не менялись.
- Эта квитанция и строка R3b tracker фиксируют блокировку отдельным коммитом.
  Engine, установленный plugin, portfolio и main не менялись; merge — близнец.

## Причина STOP

`Mimir.V4.StaticMesh.TargetedReimport.ForceAndNotify` требует увеличивать
`GetPlacementRebuildCount()` после changed reimport и ещё раз после
unchanged force-reimport: `MHStaticMeshImporterTest.cpp:2222–2225` и
`2280–2283`, ожидания `InitialPlacementRebuilds + 1` и `+ 2`.

R3b §1–§2 требует для уже размещённого mesh обновлять/мигрировать бакеты
без rebuild, при пустой дельте не делать ничего. R3a сохраняет предыдущий
Ready-снимок между invalidation. Переписанный близнецом
`Recipe.ActorReimportViaRecipeDependents` уже следует этой норме, старый
ForceAndNotify — ещё нет.

Это противоречие двух замороженных требований обнаружено статически,
до реализации. Оно не выдаётся за runtime failure новой реализации.
Исправление требует правки теста близнецом и потому исключено из DECIDED
правилом owner. Подробности и два варианта: контракт, `OPEN-R3B-1`.

## Host и baseline RED

Свежий host создан `tools/setup_s6_runtime_host.ps1`:
`E:/MimirComposite_R3b_20260905/MimirCompositeV5S6.uproject`.
Plugin подключён junction к данному worktree. Engine:
`D:/PersonalProjects/UE5/UE_5.7`, stock UE 5.7.4, CL 51494982.
Сборка начата до обнаружения противоречия; после STOP завершается только
read-only проверка исходного кода базы. Интерактивных шагов нет.

```text
Build.bat MimirCompositeV5S6Editor Win64 Development
 -Project=E:/MimirComposite_R3b_20260905/MimirCompositeV5S6.uproject
 -NoEngineChanges -NoHotReloadFromIDE -NoUBA -MaxParallelActions=2
 -DisableUnity -NoPCH -NoSharedPCH -WarningsAsErrors -WaitMutex
```

Automation после завершения сборки:

```text
UnrealEditor-Cmd.exe E:/MimirComposite_R3b_20260905/MimirCompositeV5S6.uproject
 -nullrhi -unattended -nop4 -nosplash -nosound -NoAssetRegistryCache
 -MHGoldenRoot=E:/GITHUB/Mimirhead_UE5Exporter/MH_blender_bridge_r3b_executor/golden
 "-ExecCmds=Automation RunTests Mimir."
 "-TestExit=Automation Test Queue Empty"
 -ReportExportPath=E:/MimirComposite_R3b_20260905/RedReport
 -abslog=E:/MimirComposite_R3b_20260905/R3B_RED_TEST.log
 -stdout -FullStdOutLogOutput
```

Сборка: **Succeeded**, 136 actions, 746.07 s (Parallel executor 739.08 s).
Лог: `E:/MimirComposite_R3b_20260905/R3B_RED_BUILD.log`.

Полный RED: **202 результата**, 199 reported Success (135 обычных + 64
с предупреждениями), **3 Fail**, JSON NotRun=0; 34.88843 s. Отчёт:
`E:/MimirComposite_R3b_20260905/RedReport/index.json`; журнал:
`E:/MimirComposite_R3b_20260905/R3B_RED_TEST.log`.

| Проверка | Результат на неизменённой базе | Строка журнала |
|---|---|---|
| `Recipe.ActorReimportViaRecipeDependents` | Fail: notification при неизменном интерфейсе всё ещё rebuild | 3844–3846; assertion `MHCompositeActorRecipePreviewTest.cpp:187` |
| `Reconcile.DescriptorChangeMigratesOnlyAffectedBucket` | Fail: descriptor change всё ещё placement rebuild | 3898–3900; assertion `MHResourceReconcileTest.cpp:181` |
| `Reconcile.SameInterfaceReimportKeepsBuckets` | Fail: payload change всё ещё rebuild и вызывает layout (1 вместо 0) | 3918–3921; assertions `MHResourceReconcileTest.cpp:139–140` |
| `Reconcile.ChildRecipeReimportKeepsParentRecipe` | Success уже на базе | 3890 |
| `Mimir.V4.StaticMesh.TargetedReimport.ForceAndNotify` | Success: старые ожидания увеличения счётчика выполнены | 3066 |

Из трёх новых Reconcile-тестов child-тест уже зелёный на базе: отдельный
runtime RED для него не заявляется. Реализации GREEN нет. Успех старого
ForceAndNotify и падения новых ожиданий не заменяют логическое обоснование
OPEN, а показывают текущее несовпадение ожиданий на одном дереве исходников.

У процесса exit code 0, несмотря на три Fail: результат прочитан из JSON
и журнала. В full log дополнительно есть три условных **NOT RUN**, которые
JSON учитывает как Success: два real-RHI preview lane (строки 992, 999,
требуют `-MHPreviewRenderSmoke` без NullRHI) и `ISM.CottageMetrics` (строка
3485, cottage fixture отсутствует). Эти lane не заявляются выполненными.

После оформления документов: `python tools/check_normative_docs.py` →
`normative docs: OK`; `git diff --check` → PASS. Diff всего
`ue/MimirComposite/Source` относительно `2f4bda3` пустой; tracker меняет
только строку R3b.

## Границы проверки и продолжение

GREEN, миграция бакетов, новый telemetry и замер 100 placements **NOT RUN**:
реализация не начиналась из-за OPEN. Force Unity и StrictIncludes для новой
реализации также **NOT RUN**. Приёмка R3b не заявляется.

Таблица «дельта → действие» реализации отсутствует: реализация пока не
менялась; действующее требование остаётся в §2 контракта. DECIDED нет.

Рекомендуемое продолжение: близнец согласует два ожидания ForceAndNotify
с R3b, сохранив проверки импорта, receipt/геометрии, уведомлений, build/save
и mesh identity. После согласованного red-коммита исполнитель возобновляет
реализацию в этой же ветке и обновляет этот же PR.

Полевое подтверждение на портфолио остаётся owner-gate после merge;
этот STOP-checkpoint его не заменяет.
