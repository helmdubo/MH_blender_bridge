# S2 — targeted reimport без FullScan

Статус: **READY FOR REVIEW**. Targeted reimport обновляет в Project Index только
собственный source-путь; fallback full scan ограничен пересозданием БД и
индексом без завершённой generation. Индекс привязан к физическому Source Root,
поэтому строки разных деревьев больше не смешиваются.

## 1. База и host

- ветка: `source/s2-targeted-reimport-upsert`; база `origin/main` `dae980b`;
  исходный red-коммит `2978373`, контрактный checkpoint `5980d35`;
- реализация targeted upsert: `2b50105`; guard `generation == 0`: `2064483`;
  привязка БД к Source Root: `637c526`;
- отдельный worktree:
  `E:\GITHUB\Mimirhead_UE5Exporter\MH_blender_bridge_s2_executor`;
- собственный module-free host:
  `E:\MimirComposite_S2_External_20260902\HostProject.uproject`; stock UE 5.7.4:
  `D:\PersonalProjects\UE5\UE_5.7`; plugin подключён junction к worktree;
- основной checkout `main`, Engine, resolver/runtime module, `golden/`,
  `reference/` и Blender-аддон не изменялись; `main` не пушился.

## 2. Red-first

Первый HEAD среза собран non-unity/no-PCH:
`E:\MimirComposite_S2_External_20260902\Saved\Logs\S2_BASE_BUILD_NONUNITY.log:139`
— `Result: Succeeded`. Первым тестом после сборки был
`Mimir.V5.Composite.Perf.InstrumentationCounters`; лог
`S2_RED_TEST.log`:

```text
1109: MH_PERF_REIMPORT ... full_scan_count_delta=1 incremental_paths=0 analysis_services_ms=100.967 ...
1115: Test Completed. Result={Fail} Name={InstrumentationCounters}
1126: Expected 'targeted reimport performs no full scan' to be 0, but it was 1.
1127: Expected 'targeted reimport upserts exactly its own source path' to be 1, but it was 0.
```

После контрактного red-коммита `24f2c8f` обновлённый HEAD также сначала собран
non-unity/no-PCH (`S2_RED2_BUILD_NONUNITY.log:19`, `Result: Succeeded`) и первым
запущен нетронутый тест
`Mimir.V5.Source.Index.SourceRootMismatchRecreates`; `S2_RED2_TEST.log`:

```text
1079: Test Completed. Result={Fail} Name={SourceRootMismatchRecreates}
1081: Expected 'a different Source Root recreates the database' to be true.
1082: Expected 'recreated index starts at generation zero' to be 0, but it was 1.
```

## 3. Реализация и целевой green

`UMHSourceImporter::ReimportStaticMesh` передаёт свой нормализованный
`ReceiptSourcePath` в `MHCreateIncrementalSourceAnalysisServices` и учитывает
`incremental_paths=1` только для реально выполненного targeted upsert. Сервис
делает full scan при `bRecreated || Index->GetGeneration() == 0`.

`FMHProjectResourceIndex::Open` записывает в новую БД `Meta.source_root` в
физической канонической форме. Отсутствующий ключ старой БД или несовпадение с
текущим Source Root проваливает существующую проверку Meta и ведёт по прежнему
пути удаления/создания с `bOutRecreated=true`. Таблицы, tag
`mh.project_index:4`, hash-домен, normalized dump и `ScanFullSnapshot` не
менялись; миграции нет.

Финальная non-unity/no-PCH сборка:
`S2_GREEN2_BUILD_NONUNITY.log:19` — `Result: Succeeded`. Новый тест зелёный:

```text
S2_GREEN2_TEST.log:1080: Test Completed. Result={Success} Name={SourceRootMismatchRecreates}
```

Perf-тест после первой targeted-реализации зафиксировал:

```text
S2_GREEN_TEST.log:1104: MH_PERF_REIMPORT ... full_scan_count_delta=0 incremental_paths=1 analysis_services_ms=22.200 ...
S2_GREEN_TEST.log:1110: Test Completed. Result={Success} Name={InstrumentationCounters}
```

На собственном host первый сопоставимый red/green изменился с `100.967` до
`22.200` мс (4.55x); контрактный red близнеца — `96.610` мс. После финальной
root-binding реализации отдельный прогон остаётся функционально зелёным:
`S2_GREEN2_PERF.log:1105,1111` — `full_scan_count_delta=0`,
`incremental_paths=1`, `analysis_services_ms=31.229`, `Result={Success}`.
Значения времени — отдельные локальные прогоны, не полевой portfolio-замер.

## 4. Возвраты и закрытые OPEN

### OPEN-S2-1 — закрыт близнецом

Закрыт 2026-09-02 контрактным коммитом `e1dd4f3`: full scan разрешён при
`bRecreated || GetGeneration() == 0`, потому что прямые fixture-импорты могут
открыть индекс без завершённого full scan. Реализация — `2064483`; этот guard
сохранён вторым рубежом после root identity.

### OPEN-S2-2 — закрыт близнецом

Закрыт 2026-09-02 red-коммитом `24f2c8f` и нормативным коммитом `93b159c`:
Project Index принадлежит одному физическому Source Root через
`Meta.source_root`; отсутствующий или другой root пересоздаёт файл. Реализация
— `637c526`. Нетронутый red-тест теперь зелёный, а четыре targeted-теста
проходят и на переиспользованной, и на свежей БД.

## 5. Гейты

| Gate | Результат |
|---|---|
| non-unity/no-PCH, исходный RED HEAD | PASS — `S2_BASE_BUILD_NONUNITY.log:139`, `Result: Succeeded` |
| `Perf.InstrumentationCounters`, RED | ожидаемый FAIL — `S2_RED_TEST.log:1115,1126-1127` |
| non-unity/no-PCH, OPEN-S2-2 RED HEAD | PASS — `S2_RED2_BUILD_NONUNITY.log:19`, `Result: Succeeded` |
| `SourceRootMismatchRecreates`, RED | ожидаемый FAIL — `S2_RED2_TEST.log:1079,1081-1082` |
| финальный non-unity/no-PCH | PASS — `S2_GREEN2_BUILD_NONUNITY.log:19`, `Result: Succeeded` |
| `SourceRootMismatchRecreates`, GREEN | PASS — `S2_GREEN2_TEST.log:1080` |
| `Perf.InstrumentationCounters` | PASS — `S2_GREEN2_PERF.log:1105,1111`; `full_scan_count_delta=0`, `incremental_paths=1` |
| `Mimir.V4.StaticMesh.TargetedReimport.*`, reused DB | PASS — 4/4; `S2_GREEN2_TARGETED_REUSED.log:1089,1128,1148,1187` |
| `Mimir.V4.StaticMesh.TargetedReimport.*`, fresh DB | PASS — 4/4; `S2_GREEN2_TARGETED_FRESH.log:1088,1127,1147,1186` |
| `Mimir.V4.ProjectIndex.*` | PASS — 12/12; `S2_GREEN2_PROJECTINDEX.log:1088-1175` |
| полный NullRHI `Automation RunTests Mimir` | PASS — **179/179 Success**, 0 failed; `S2_GREEN2_FULL.log:1287-4729`; ключевые строки `3489,3987,4665`, generic-host guard `NOT RUN` = 17 |
| guarded force-unity | PASS — 14/14; `S2_GREEN2_FORCEUNITY.log:25-31`, `Result: Succeeded` |
| `BuildPlugin -StrictIncludes -DisableUnity -NoPCH -NoSharedPCH` | PASS — `S2_GREEN2_STRICT.log:226,228`, `BUILD SUCCESSFUL`, ExitCode 0; пакет `E:\MimirComposite_S2_Strict_20260902` |
| `git diff --check` | PASS |
| `python tools/check_normative_docs.py` | PASS — `normative docs: OK` |

## 6. Изменённые файлы

Изменения исполнителя — ровно разрешённые контрактом файлы:

- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Source/MHSourceImporter.cpp`
  — targeted `ReimportStaticMesh`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Source/MHSourceComposition.cpp`
  — guard пересозданной/нулевой generation;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Index/MHProjectResourceIndex.cpp`
  — только создание/чтение `Meta.source_root` и физическая канонизация для
  `Open`;
- `docs/10_source_protocol_v5_plan.md` — две нормативные фразы S2 в §3;
- `docs/receipts/source_s2.md` — эта квитанция;
- `docs/RECIPE_EXECUTION_STATUS.md` — строка S2 → `IN REVIEW`.

Red-тесты `MHStaticMeshImporterTest.cpp` и `MHProjectResourceIndexTest.cpp`
добавлены близнецом до реализации и исполнителем не менялись. Удалённых тестов
нет.

## 7. OPEN-вопросы

Нет. `OPEN-S2-1` и `OPEN-S2-2` закрыты близнецом и реализованы без
переинтерпретации контрактов.

## 8. Строка трекера

`S2 | IN REVIEW | targeted reimport без FullScan`.
