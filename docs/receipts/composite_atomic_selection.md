# Composite atomic secondary selection — receipt, 2026-09-07

История исполнения, не норматив. Ветка `codex/composite-atomic-selection`,
база `d18e34b`. Owner принял native click interaction и inline Outliner,
запросил atomic object selection и подтвердил: трансформы узлов только в Edit.

## Изменение

Pool highlight predicate сравнивает точный SelectedPlacementLeafPath вместо
prefix всего SelectedPlacementOccurrencePath. Пустой выбранный leaf означает
выбор всего root; пустой occurrence при непустом leaf не расширяет подсветку
на весь root. Путь occurrence по-прежнему задаёт containing definition для
Edit, а точный leaf задаёт authored-node preselection.

Click routing, native toggle, Edit backend, draft transforms, inline Outliner
и renderer storage не менялись. Pool SetOwnerSelected и AddInstanceToComponent
используют один predicate, поэтому выбранный объект сохраняет семантику при
повторном назначении подсветки и добавлении pool slots.

Граница проверки: pooled StaticMesh leaves. Actor endpoints через
UChildActorComponent имеют отдельный stock Component selection route;
этот срез не заявляет паритет их Edit routing/derived-component manipulation.
Native owner Actor handle сохранён: вне Edit его штатный widget относится
к размещению root; authored-node transform доступен после открытия Edit.

## Проверки

Собственные host UE5.7.4 CL51494982 / BuildId47537391; логи
`E:/temp/MH_CEI1_host_20260907/`.

- `ATOMIC_RED_BUILD.log` SUCCESS. `ATOMIC_RED.log`: оба selection-теста RED
  на прежнем production predicate — ошибочно подсвечены остальные объекты
  того же explicit/random occurrence. Поведение проверяется по реальным
  ISM selected-instance bits, включая полный native click notification цикл.
- Добавлен переход между двумя отдельными объектами одного subcomposite:
  точный leaf меняется, occurrence для Edit сохраняется.
- Добавлен прямой mesh root-определения: один объект подсвечен, Edit открывает
  root и предварительно выбирает его authored node.
- Прежние тесты/count не удалялись; whole-occurrence expectations изменены
  по прямому решению owner. Existing root/nested Edit, sibling occurrences,
  random occurrence, deselection, rebuild и same-bucket owner isolation остаются.

- StrictIncludes non-unity/no-PCH: `ATOMIC_STRICT.log` SUCCESS, 77 actions,
  337.65 s total.
- Guarded ownhost non-unity/no-PCH: `ATOMIC_GUARDED.log` SUCCESS, 79 actions,
  338.83 s total.
- ForceUnity / DisableAdaptiveUnity: `ATOMIC_UNITY.log` SUCCESS, 15 actions,
  87.09 s total.
- Полный NullRHI Mimir/golden: `AtomicFullReport/index.json`, **292 passed,
  0 failed** (192 success + 100 success-with-warnings). Обе проверки
  RecipeShadowParity и RecipeShadowParityApplied — Success.
- D3D12 offscreen smoke: `AtomicRHIReport/index.json`, **64 passed, 0 failed**
  (22 success + 42 success-with-warnings), включая оба selection-теста.

- Before/after PerfTrace: `AtomicBeforeReport` и `AtomicAfterReport`, по **2/2**.
  Map load: 0.146 → 0.135 ms; sync package loads, registry lookups,
  identity admissions и новые components остаются 0; reused components — 2.
  Targeted reimport: 134.210/127.479 → 145.624/131.014 ms; full scans,
  recipe recompiles и actor rebuild ms остаются 0, migration counts — 1/0.
  Это одиночные контрольные замеры, не статистический performance benchmark.
- `python tools/check_normative_docs.py` и `git diff --check`: PASS.

Статус: READY FOR FIELD TEST. Ручная проверка владельцем ещё предстоит.

Полевой сценарий: double LMB на cottage → один mesh выделен; single LMB/RMB
по соседнему mesh того же подкомпозита → выделен только он; Edit открывает
содержащий subcomposite с выбранным узлом. Double LMB возвращает root selection.

Пакет/manifest/installation receipt: `E:/temp/MH_atomic_selection_20260907/`.
