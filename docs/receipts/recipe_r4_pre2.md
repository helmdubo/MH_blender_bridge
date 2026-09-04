# R4-pre-2 — appearance при Break и предупреждения Build

Статус: **READY FOR REVIEW**. Это квитанция исполнения, не приёмка близнеца или owner.

## База и границы

- Ветка исполнителя: `codex/recipe-r4-pre2-build-break-preserve`, создана от
  `origin/recipe/r4-pre2-build-break-preserve` (`401d73f`). Red/API: `511d0ba`;
  база исходной ветки: `ad6d206`. R3a в эту ветку не включён.
- Реализация: `9221626`; после валидации менялись только квитанция/tracker.
- Checkout: `E:/GITHUB/Mimirhead_UE5Exporter/MH_blender_bridge_r4pre2_executor`.
- Свежий host создан `tools/setup_s6_runtime_host.ps1`:
  `E:/MimirComposite_R4Pre2_20260905/MimirCompositeV5S6.uproject`.
  Plugin junction указывает только в checkout этого среза.
- Installed Engine: `D:/PersonalProjects/UE5/UE_5.7`, **5.7.4 CL 51494982**.
  Portfolio/audit-host, Engine и другие checkout не изменялись.
- Изменены два `.cpp`, эта квитанция и только строка R4-pre-2 в tracker.
  Публичный API и acceptance-тесты идентичны red-коммиту; новые диагностики
  не добавлены. Edit-сессия, rebuild-all, resolver, transport, proof,
  actor, placement compiler и реестры не изменялись.

## Реализация

Break выбирает верхний слой по `Plan.Nodes`, как прежде. Однократный lookup
`Plan.Leaves` по `OwningResolvedNodeIndex` связывает mesh node с его appearance;
он не разворачивает вложенные композиты и не строит proof. Spec копирует четыре
канала и configured base index. World-матрица берётся у того же leaf.
Композитные дети и Actor-листья не получают mesh appearance transport.

После `SetStaticMesh` новый компонент получает каналы через неизменённый
`MHApplyLeafAppearanceCustomData`. Кроме transient значений записываются
`SetDefaultCustomPrimitiveDataFloatArray`: в UE 5.7 `CustomPrimitiveDataInternal`
помечен Transient, а регистрация сбрасывает его из сериализуемых defaults.
Это сохраняет результат за пределами первого кадра. Component `Modify()`
работает внутри существующей транзакции Break; Undo regression зелёный.
Acceptance проверяет текущие каналы, отдельный save/reload тест не добавлялся.

Единственная реализация сборки документа Build находится в
`MHPreflightBuildComposite`. В неё перенесена существующая проверка общего
ULevel. `BuildComposite` вызывает preflight до публикации, сохраняет его
warnings и продолжает прежние source/proof/target проверки. Прежняя сборка
узлов внутри `BuildComposite` удалена; Edit-путь не затронут.

| Состояние | Предупреждение как реализовано |
| --- | --- |
| Дочерний `AMHCompositeActor` | Путь, `child composite seeds`, оба значения, невозможность записать их и reroll subtree под новым parent |
| Ненулевой `OverrideMaterials[i]` | Отдельная строка с путём актора, `material override in slot i`, путём материала и `is dropped` |
| Непустые текущие CPD | Путь актора, `custom primitive data`, число floats и `is dropped` |
| Actor-лист отличается от defaults | Одна строка с путём и `instance properties differing from class defaults ... are dropped` |

Последняя строка использует `FProperty::Identical_InContainer`: `CPF_Edit`
без `CPF_DisableEditOnInstance/Template`, включая superclass и все элементы
фиксированных массивов, на акторе и root component. Actor CDO берётся без
создания; для Blueprint-root, отсутствующего на CDO или имеющего другой класс,
используется существующий archetype. Сравнение намеренно широкое по таблице
§2 контракта: отдельного исключения для EditAnywhere transform/label или
эквивалентных subobject pointers не вводилось. Эта строка покрыта разбором кода,
а не отдельным новым acceptance-тестом.

`ExecuteBuildComposite` вызывает чистый preflight и `NotifyOperation` на
странице «Build MH Composite» перед вызовом публикующего `BuildComposite`.
При warnings публикация продолжается, нового диалога подтверждения нет.
Итоговое сообщение операции также содержит предупреждения. UI не запускался
интерактивно; порядок вызовов проверен по коду.

## Red-first и результаты

Все пути evidence ниже — относительно `E:/MimirComposite_R4Pre2_20260905`.

Исходный `401d73f` собран без unity/PCH до первой правки реализации:
`R4P2_RED_BUILD.log`, 134 actions, `Result: Succeeded`.
Первый прогон с console `mh.PerfTrace 1` некорректно закрепил приоритет cvar:
два perf-теста не смогли установить ноль через SetByCode. Это ошибка команды
исполнителя, не регресс кода; исходный `RedReport`/`R4P2_RED_TEST.log` сохранён.
Повтор без console override использует собственные переключения perf-тестов.

Корректный RED: `RedCleanReport/index.json`, 8 тестов — 6 Success, ровно 2 Fail:

- `R4P2_RED_CLEAN_TEST.log:762`: отсутствуют CPD appearance channels после Break;
- `R4P2_RED_CLEAN_TEST.log:808`: отсутствует warning о seed; строки 809–810:
  отсутствуют material override и custom primitive data warnings.

| Гейт | Результат | Evidence |
| --- | --- | --- |
| Non-unity/no-PCH/shared-PCH, warnings as errors | PASS, 5 actions, 20.57 s | `R4P2_GREEN_BUILD.log` |
| Focused Break/Build/LevelOperations/AppliedAdmission/perf + фактический BuildPreflight regression | 13 Success, 0 Fail, 0 notRun | `GreenFocusedReport/index.json`, `R4P2_GREEN_FOCUSED_TEST.log` |
| Full NullRHI `Mimir.` | 198 reported Success, 0 Fail, 36.12 s; три условных lane не исполнялись, см. ниже | `FullReport/index.json`, `R4P2_FULL_TEST.log` |
| RecipeShadowParity + RecipeShadowParityApplied | PASS | `R4P2_FULL_TEST.log:3861`, `:3867` |
| Guarded force-unity, adaptive off | PASS, 14 actions, 113.75 s | `R4P2_FORCE_UNITY_BUILD.log` |
| BuildPlugin `-StrictIncludes` | PASS, Editor Development + UnrealGame Development/Shipping, 10m57s | `R4P2_STRICT_BUILD.log:161,191,220,225` |
| `git diff --check`, `python tools/check_normative_docs.py` | PASS | команды из checkout |
| Test/public API diff против `511d0ba` | Пустой | `git diff 511d0ba -- ue/MimirComposite/Source/MimirCompositeTests ue/MimirComposite/Source/MimirCompositeEditor/Public/Composite/MHCompositeLevelSubsystem.h` |

В этой ветке нет теста с буквальным префиксом
`Mimir.V5.Composite.BuildPreflight`. Реальный существующий regression —
`Mimir.Audit.MainBaseline.BuildPreflightRejectsBeforeMutation`; он включён в
focused gate и полный suite. Тесты не переименовывались.

Full JSON сообщает 135 Success + 63 Success с warnings и `notRun: 0`, но лог
отдельно сообщает `NOT RUN` для
`Mimir.Audit.MainBaseline.LoadedPlacementClickSelectsActor`,
`Mimir.Audit.MainBaseline.RenderedNativeHitProxy` (нужен RHI и явный smoke flag)
и `Mimir.V5.Composite.ISM.CottageMetrics` (реальный cottage отсутствует).
Эти три условных lane не заявляются как выполненные проверки. Перед full run
артефакты focused runs из Content/MH и Saved перенесены в `BeforeFullSuite`
внутри собственного host, не удалены.

Perf до/после: `R4P2_RED_CLEAN_TEST.log:828–829,876` и
`R4P2_GREEN_FOCUSED_TEST.log:913–914,961` содержат `MH_PERF_MAPLOAD` /
`MH_PERF_REIMPORT`. В обоих map-load примерах tag queries, live receipt tag
reads, sync loads и compile waits равны нулю; reimport `full_scan_count_delta=0`.
Это smoke evidence счётчиков, не статистический benchmark ускорения.

Пакет: `E:/MimirComposite_R4Pre2_Strict_20260905`. Все **185** файлов Source
пакета и его strict host совпали с checkout по SHA-256;
`final-package-parity.json` сохраняет полный список. Пакет не устанавливался
в portfolio. Временная UBT-конфигурация отключала UBA и ограничивала сборку
двумя действиями; `finally` восстановил точные исходные байты.
`strict-config-restoration.json`: `Restored: true`, `BuildExitCode: 0`,
одинаковый before/after SHA-256
`85382F0E3B2029D752E3CC42EF17F2C1727E1700D55D2451F050C474BA3772FB`.

Команды воспроизведения: `Build.bat MimirCompositeV5S6Editor Win64 Development
-Project=<host.uproject> -NoEngineChanges -NoHotReloadFromIDE -NoUBA
-MaxParallelActions=2 -DisableUnity -NoPCH -NoSharedPCH -WarningsAsErrors`.
Для force-unity заменить `-DisableUnity` на `-ForceUnity -DisableAdaptiveUnity`.
`UnrealEditor-Cmd.exe <host.uproject> -nullrhi -unattended -nop4 -nosplash
-nosound -NoAssetRegistryCache -MHGoldenRoot=<checkout>/golden
"-ExecCmds=Automation RunTests Mimir." "-TestExit=Automation Test Queue Empty"
-ReportExportPath=<report-directory>`.

## OPEN и приёмка

OPEN по R4-pre-2: нет. Исполнитель не выполнял интерактивных шагов и не
менял проект owner. Merge — близнец после независимой проверки. Полевое
подтверждение appearance-материала и Message Log остаётся owner после merge,
как указано в acceptance 7. Контекст random-поддеревьев R4-pre-3 вне среза.
