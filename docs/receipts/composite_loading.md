# Composite loading A/B и Save — 2026-09-07

Статус: READY FOR FIELD TEST. Owner принял реализацию;
полевой результат и merge пока не подтверждены.

## Изменение

Normal placement сохраняет подготовленный выбранный план до завершения загрузки
мешей. Первый вид появляется без кубов, обновление сохраняет предыдущий вид.
Завершение загрузки отделено от реимпорта и сгруппировано на следующий editor
tick. Готовые meshes удерживаются handles/UPROPERTY до передачи компонентам.
Outliner показывает Loading. Смена определения, движение, уничтожение актора
и закрытие Edit учитывают незавершённые запросы.

`SelectedMeshDependencies` хранит editor-only hard references выбранных мешей,
отсортированные и без повторов. Изменившийся успешный commit помечает пакет для
сохранения, в том числе если mesh завершил загрузку уже после Save. Неизменные
hints не загрязняют карту. Это не snapshot плана и не proof freshness.
Невыбранные варианты не становятся зависимостями карты. Shared ISM pool остаётся
transient; Editor Edit использует прежнюю SMC projection.

Обычный Save Map больше не вызывает `RequestProof`. Кэш только читается,
warning остаётся; явный proof, cook/preflight и snapshot сохраняют проверки.
В owner-логе 14 Unknown placements были поставлены в очередь во время Save;
затем шли паузы по 3–15 секунд. Код выполнял один синхронный full-closure proof
за editor tick. SQL входил в этот путь, но его доля отдельно не замерена.

## Red и сценарии

Логи под `E:/temp/MH_CEI1_host_20260907/`.

- `LOADING_RED.log`: две cold dependencies вызвали Resolve=3/Compile=3,
  четыре pool instances с кубами до готовности и два MHNotify вместо нуля.
  Update терял прежний handle; Save оставлял queued proof (Flush строил graph).
- `LOADING_B_RED3.log`: все три B-сценария отклонены из-за отсутствующего
  persistent property; зафиксированы baseline `MH_PERF_MAPLOAD` и
  `MH_PERF_REIMPORT`. Проверка «LoadPackage не регистрирует world» оказалась
  неверной для UE 5.7 и заменена чтением map linker imports.
- Замена R4: прежний `ColdEndpointLoadsWithoutSyncLoad` заменён
  `ColdSelectedEndpointsMaterializeOnceWhenAllReady`; добавлены retain/move,
  stale swap/destroy, сохранение selected-only refs и native map roundtrip.
- R2c `SaveWarnsWithoutProof` теперь проверяет два placements: Unknown после
  Save, ноль графов после Flush, успешный явный proof.
- Edit: `ColdDraftMeshRefreshesOnLoadReady` проверяет настоящий cold endpoint
  после SetNodeResource и late completion после Cancel без восстановления
  закрытой projection. Авторский transform не меняется.
- `LOADING_PROJECTION_RED.log`: 7 сценариев A/B/Save прошли; единственный
  отказ — меш projection не обновлялся после ready без новой edit-команды.
- Первый полный прогон: 285/288. Два прежних assertions считали только
  compiler endpoint pass; теперь измеряются readiness admission и compiler
  binding (два CPU scope на warm placement), time threshold сохранён.
  Третий отказ был утечкой world из нового roundtrip-теста в следующий
  migration-тест; teardown добавлен до освобождения fixture mesh assets.

## Границы

Проверено: `LOADING_FULL_FINAL.log` / `LoadingFullFinalReport`: **288/288**,
включая оба RecipeShadowParity. Guarded non-unity/no-PCH
`LOADING_GREEN3_BUILD.log`, strict `LOADING_STRICT_FINAL.log`,
force-unity adaptive-off `LOADING_FORCE_UNITY.log` — SUCCESS.
Perf-after `LOADING_PERF_AFTER.log`: 2/2; preview — ноль applied graph,
sync mesh package loads, compilation wait и Asset Registry tag queries;
targeted reimport — full scan delta 0, notified actor 1, parent recompile 0.
Baseline — `LOADING_B_RED3.log`. Различаются seed-selected варианты и фоновые
условия, поэтому эти traces подтверждают путь/счётчики, а не процент ускорения.

Первый RHI-прогон: 56/57. Старый тест подсветки загружал stock Cube и сразу
требовал render proxy; после cold-load/GC тестов Cube мог ещё компилироваться.
Только в RHI fixture добавлен `FinishCompilation`, production не менялся.
Финальный RHI: `LOADING_RHI_FINAL.log` / `LoadingRHIFinalReport` — **59/59**,
D3D12 / RTX 3070; оба ShadowParity, Game View visibility, whole-node highlight,
Save/Cancel/Esc и новые loading/persistence сценарии прошли на strict DLLs.
Сборка RHI fixture: `LOADING_RHI_FIX_BUILD.log`; финальный unity:
`LOADING_FORCE_UNITY_FINAL.log` — SUCCESS. `git diff --check` и
`python tools/check_normative_docs.py` — OK.
Повтор полного suite на финальной unity-сборке после правки RHI fixture:
`LOADING_FINAL_UNITY.log` / `LoadingFinalUnityReport` — **288/288**.

Ready UObject не гарантирует, что shaders и все texture mips готовы.
Низкоуровневый helper и Edit могут кратко использовать существующий loading
placeholder; normal placed composite больше не материализует куб на каждый
лист. Failed async package load получает Invalid с диагностикой до явной
инвалидации; Missing и настоящие ошибки не скрываются как Loading.
Синтетические traces не являются замером времени открытия portfolio-сцены.

## Полевая проверка

1. Открыть существующую карту: вместо кубов новый cold placement ожидает
   выбранные меши; Outliner показывает загрузку, камера остаётся под контролем.
2. После окончания загрузки сохранить карту один раз, затем открыть её снова:
   сохранённые mesh references участвуют в обычной загрузке зависимостей UE.
3. Повторить Save карты/BuildData: MH не должен запускать затем очередь
   full-closure proof. Если пауза останется, нужен свежий лог с временем Save;
   этот срез не утверждает, что устранил все возможные stall других подсистем.
4. Открыть гаражный подкомпозит, выбрать узел кликом, переместить; Cancel/Esc,
   затем повторить с Save. Проверить G/Game View и сохранение контроля камеры.
