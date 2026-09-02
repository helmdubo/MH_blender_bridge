# S0 — инкрементальный Project Resource Index

Статус: **READY FOR REVIEW**. Full scan переведён на один snapshot-проход;
неизменные `ok`-кандидаты переиспользуют сохранённый fingerprint и dependency
edges без чтения payload, а изменённый payload проходит прежний read/hash/parse.

## 1. База и границы

- ветка: `source/s0-incremental-index`; red-коммит `46f7187`, контрактный
  коммит `d3bb037`;
- актуальный `origin/main` `33969ec` влит в рабочую ветку отдельным merge-коммитом
  `60d84e3`; реализация — `f1a88a6`;
- отдельный worktree:
  `E:\GITHUB\Mimirhead_UE5Exporter\MH_blender_bridge_s0_executor`;
- собственный module-free host:
  `E:\MimirComposite_S0_External_20260902\HostProject.uproject`, Engine
  `D:\PersonalProjects\UE5\UE_5.7`; plugin подключён junction'ом к этому
  worktree;
- основной checkout и его локальный `main` не переключались и не пушились;
- Engine, resolver/runtime, Blender-аддон, `golden/`, `reference/`, SQLite
  schema/tag, hash-формат и receipt-форматы не изменялись; red-тест не изменён.

## 2. Acceptance

| # | Критерий | Результат |
|---|---|---|
| 1 | Один snapshot-проход | GREEN: `scan_passes=1`; повторная энумерация и второй проход сравнения удалены, pre/post `(size, mtime)` guard одного читаемого файла сохранён |
| 2 | Неизменный `ok`-кандидат не читается и не хэшируется | GREEN: lookup по `(path,size,mtime,parse_status=ok)` восстанавливает `raw_hash`, identity, diagnostic и `Dependencies`; unchanged: `hashed_files=0`, `reused_fingerprints=3` |
| 3 | Изменённый payload хэшируется один раз | GREEN: `hashed_files=1`, `reused_fingerprints=2` |
| 4 | Проекция и resolver outcomes равны fresh rebuild | GREEN: assertions red-теста прошли; fresh rebuild хэширует 3/3 и даёт тот же normalized dump/outcomes |
| 5 | `mh.SourceIndex.VerifyHashes` | default `0`; при `1` cached candidates проходят hash/parse, mismatch при равных `(size,mtime)` помечается derived diagnostic `MH_W_SOURCE_INDEX_STALE_FINGERPRINT`, новая строка заменяет старый fingerprint |
| 6 | Форматы не меняются | `mh.project_index:4`, schema, raw-hash и receipt-форматы сохранены |

## 3. Red-first и perf-квитанция

RED на merge-HEAD `60d84e3`: лог
`E:\MimirComposite_S0_External_20260902\Saved\Logs\S0_RED_TEST.log`, строки
1082–1097:

```text
1083: MH_PERF_STARTUP_SCAN ... enumerated_files=3 ... scan_passes=2 hashed_files=6 ... total_ms=15.084
1087: Test Completed. Result={Fail} Name={IncrementalScanSkipsUnchangedHashes}
1093: Expected 'unchanged rescan is one snapshot pass' to be 1, but it was 2.
1094: Expected 'unchanged rescan hashes nothing' to be 0, but it was 6.
1097: Expected 'changed rescan hashes exactly the changed payload' to be 1, but it was 6.
```

GREEN на реализации `f1a88a6`: лог
`E:\MimirComposite_S0_External_20260902\Saved\Logs\S0_GREEN_TEST.log`, строки
1076–1081:

```text
1076: MH_PERF_STARTUP_SCAN ... scan_passes=1 hashed_files=3 reused_fingerprints=0 ... total_ms=15.716
1077: MH_PERF_STARTUP_SCAN ... scan_passes=1 hashed_files=0 reused_fingerprints=3 ... total_ms=15.139
1078: MH_PERF_STARTUP_SCAN ... scan_passes=1 hashed_files=1 reused_fingerprints=2 ... total_ms=14.973
1081: Test Completed. Result={Success} Name={IncrementalScanSkipsUnchangedHashes}
```

На одном Source Root из трёх payload повторный запуск изменился с RED
`hashed_files=6` (`2N`, `total_ms=15.084`) на GREEN `hashed_files=0`,
`reused_fingerprints=3` (`total_ms=15.139`). Cold GREEN читает каждый payload
ровно один раз (`hashed_files=3`); после изменения одного payload — ровно один
hash.

## 4. Гейты

| Gate | Результат |
|---|---|
| non-unity/no-PCH editor build (`-NoEngineChanges -WarningsAsErrors`) | RED и GREEN `Result: Succeeded`: `S0_RED_BUILD_NONUNITY.log:139`, `S0_GREEN_BUILD_NONUNITY.log:36`; после force-unity восстановлены non-unity binaries, `S0_FINAL_BUILD_NONUNITY.log:24` — `Result: Succeeded` |
| `Mimir.V5.Source.Index.IncrementalScanSkipsUnchangedHashes` | Success (`S0_GREEN_TEST.log:1081`) |
| `Mimir.V5.Composite.Perf.InstrumentationCounters` | Success (`S0_GREEN_INSTRUMENTATION.log:1109`); startup report: `scan_passes=1 hashed_files=0 reused_fingerprints=3` (`:1095`) |
| полный NullRHI `Automation RunTests Mimir` | **177/177 Success, 0 failed** (`S0_FULL_SUITE_RETRY.log`; S0 test `:4575`, последняя completion-строка `:4642`) |
| guarded force-unity | `Result: Succeeded` (`S0_FORCE_UNITY.log:31`) |
| `BuildPlugin -StrictIncludes -DisableUnity -NoPCH -NoSharedPCH` | `BUILD SUCCESSFUL` (`S0_STRICT_INCLUDES.log:226`), package `E:\MimirComposite_S0_Strict_20260902` |
| `git diff --check` | PASS |
| `python tools/check_normative_docs.py` | `normative docs: OK` |

Первый полный прогон `S0_FULL_SUITE.log` дал 176 Success / 1 Fail в
существующем `Mimir.V4.BulkImport.CrashBetweenPassesRetries` (`:1359`, не был
изменён). Отдельный повтор прошёл (`S0_BLOCKER_CRASH_RETRY.log:1103`), затем
полный повтор прошёл 177/177; acceptance использует успешный полный повтор.

Дополнительный smoke с `mh.SourceIndex.VerifyHashes=1` не является gate:
console принял значение, но попытки были остановлены до начала automation,
поскольку UE остался в `FlushAsyncLoading(406..469)`. Ни тесты, ни код ради
этого наблюдения не менялись; обязательные default-path acceptance и compile
гейты выше завершены.

## 5. Изменённые файлы

- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Index/MHProjectResourceIndex.cpp`
- `ue/MimirComposite/Source/MimirCompositeEditor/Public/Performance/MHPerformanceTrace.h`
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Performance/MHPerformanceTrace.cpp`
- `ue/MimirComposite/Source/MimirCompositeTests/Private/MHStaticMeshImporterTest.cpp`
  — единственная разрешённая строка: `scan_passes == 2` → `== 1`
- `docs/10_source_protocol_v5_plan.md` — один нормативный абзац S0 в §3
- `docs/receipts/source_s0.md`
- `docs/RECIPE_EXECUTION_STATUS.md`

Red-тест
`ue/MimirComposite/Source/MimirCompositeTests/Private/MHProjectResourceIndexTest.cpp`
не изменён.

Удалённые тесты: **нет**.

## 6. OPEN-вопросы

Нет.

## 7. Трекер

Строка S0 переведена из `READY` в `IN REVIEW`; merge и перевод в `MERGED`
остаётся за Lead после проверки PR.
