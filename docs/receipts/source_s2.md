# S2 — targeted reimport без FullScan

Статус: **STOP — OPEN-S2-2**. `OPEN-S2-1` закрыт близнецом в `e1dd4f3`, guard
`GetGeneration() == 0` реализован, а контрактный red-тест зелёный. Однако
`Mimir.V4.StaticMesh.TargetedReimport.*` остаются 2/4: generation SQLite не
различает последовательно создаваемые fixture Source Root. Ветка не переведена
в `IN REVIEW`, PR не создан.

## 1. База и host

- ветка: `source/s2-targeted-reimport-upsert`; база и red-коммит контракта:
  `5980d35` (`2978373` — red test);
- targeted-upsert checkpoint: `2b50105`; guard из ответа близнеца:
  `2064483` (контрактное закрытие `e1dd4f3`);
- отдельный worktree:
  `E:\GITHUB\Mimirhead_UE5Exporter\MH_blender_bridge_s2_executor`;
- собственный module-free host:
  `E:\MimirComposite_S2_External_20260902\HostProject.uproject`; stock UE 5.7.4:
  `D:\PersonalProjects\UE5\UE_5.7`; plugin подключён junction к worktree;
- основной checkout `main`, audit-host Lead, portfolio-проект owner, Engine,
  resolver/runtime module, `golden/`, `reference/` и Blender-аддон не
  изменялись; `main` не пушился.

## 2. Red-first

Нетронутый HEAD сначала собран non-unity/no-PCH:
`E:\MimirComposite_S2_External_20260902\Saved\Logs\S2_BASE_BUILD_NONUNITY.log:139`
— `Result: Succeeded`. Первым тестовым запуском после сборки был
`Mimir.V5.Composite.Perf.InstrumentationCounters`:
`E:\MimirComposite_S2_External_20260902\Saved\Logs\S2_RED_TEST.log`.

```text
1109: MH_PERF_REIMPORT ... full_scan_count_delta=1 incremental_paths=0 analysis_services_ms=100.967 ...
1115: Test Completed. Result={Fail} Name={InstrumentationCounters}
1126: Expected 'targeted reimport performs no full scan' to be 0, but it was 1.
1127: Expected 'targeted reimport upserts exactly its own source path' to be 1, but it was 0.
```

## 3. Целевой green

`UMHSourceImporter::ReimportStaticMesh` передаёт уже нормализованный и
проверенный `ReceiptSourcePath` в
`MHCreateIncrementalSourceAnalysisServices`. `incremental_paths` увеличивается
только после успешного открытия/обновления индекса и только когда сервис не
использовал разрешённый fallback full scan при пересоздании БД.

Сборка:
`E:\MimirComposite_S2_External_20260902\Saved\Logs\S2_GREEN_BUILD_NONUNITY.log:21`
— `Result: Succeeded`. Green-лог:
`E:\MimirComposite_S2_External_20260902\Saved\Logs\S2_GREEN_TEST.log`.

```text
1104: MH_PERF_REIMPORT ... full_scan_count_delta=0 incremental_paths=1 analysis_services_ms=22.200 ...
1110: Test Completed. Result={Success} Name={InstrumentationCounters}
```

На собственном host `analysis_services_ms` уменьшился с `100.967` до
`22.200` мс (4.55x). Это фактический результат этого запуска, не полевой
portfolio-замер.

После ответа близнеца guard собран:
`S2_POSTOPEN_BUILD_NONUNITY.log:20`, `Result: Succeeded`. Повторный perf-тест
остаётся зелёным (`S2_POSTOPEN_PERF.log`):

```text
1104: MH_PERF_REIMPORT ... full_scan_count_delta=0 incremental_paths=1 analysis_services_ms=22.909 ...
1110: Test Completed. Result={Success} Name={InstrumentationCounters}
```

## 4. Регрессия существующих targeted-тестов

После реализации отдельный запуск
`Automation RunTests Mimir.V4.StaticMesh.TargetedReimport` дал 2 Success и
2 Fail:
`E:\MimirComposite_S2_External_20260902\Saved\Logs\S2_TARGETED_REIMPORT.log`.

```text
1088: Test Completed. Result={Success} Name={Admission}
1105: MH_E_UNRESOLVED_MATERIAL_REFERENCE ... no source payload for material:targeted_mat_...
1110: Test Completed. Result={Fail} Name={ForceAndNotify}
1154: Test Completed. Result={Success} Name={MissingSourceDoesNotMutate}
1175: MH_E_UNRESOLVED_MATERIAL_REFERENCE ... no source payload for material:targeted_mat_...
1179: Test Completed. Result={Fail} Name={SequentialMultiSelection}
```

Контрольный прогон на том же host с исходным `ReimportStaticMesh` из
`5980d35` (после успешной пересборки
`S2_BASELINE_RECHECK_BUILD.log:19`) подтверждает, что это регрессия S2, а не
флап: `S2_TARGETED_REIMPORT_BASELINE.log:1088,1128,1149,1187` — **4/4
Success**.

Причина: fixture создаёт `.material`, managed material и mesh прямым вызовом
импортёра, но не проецирует source tree в Project Index. При валидной
существующей SQLite `bUsedFullScan == false`; upsert только собственного FBX
добавляет mesh-candidate и его slot-edge, однако material-candidate в индексе
отсутствует. Resolver поэтому корректно блокирует импорт. Прежний full scan
находил оба payload и скрывал эту предпосылку fixture.

### Возврат 2: guard generation==0 недостаточен

`OPEN-S2-1` закрыт близнецом в `e1dd4f3`: incremental service теперь делает
full scan при `bRecreated || Index->GetGeneration() == 0`. На уже
использовавшейся БД результат не изменился:
`S2_POSTOPEN_TARGETED.log:1088,1110,1154,1179` — 2/4.

Для исключения прежнего состояния `ProjectIndex.sqlite` был перемещён в
recoverable backup внутри собственного host, после чего тест повторён на новой
БД. `S2_POSTOPEN_TARGETED_FRESHDB.log:1088,1110,1154,1179` также дал 2/4.
Лог объясняет почему: тест `Admission` первым прямым импортом вызывает
`MHRefreshGeneratedAssetProjection`; на новой БД этот путь выполняет full scan
(`ProjectIndex.sqlite` открыт в строке 1086) и оставляет generation ненулевой.
Fixture закрывает индекс, но не удаляет SQLite. Следующий `ForceAndNotify`
создаёт другой Source Root, открывает ту же БД (строка 1104), видит generation
от предыдущего root и не попадает в guard; материал нового root остаётся
неиндексированным (строка 1105). То же повторяется для
`SequentialMultiSelection`.

## 5. Гейты

| Gate | Результат |
|---|---|
| non-unity/no-PCH, RED HEAD | PASS — `S2_BASE_BUILD_NONUNITY.log:139`, `Result: Succeeded` |
| `Perf.InstrumentationCounters`, RED | ожидаемый FAIL — `S2_RED_TEST.log:1115,1126-1127` |
| non-unity/no-PCH после реализации | PASS — `S2_GREEN_BUILD_NONUNITY.log:21`, `Result: Succeeded` |
| `Perf.InstrumentationCounters`, GREEN | PASS — `S2_GREEN_TEST.log:1104,1110` |
| `Mimir.V4.StaticMesh.TargetedReimport.*` | **FAIL / STOP** — 2/4 Success; `S2_TARGETED_REIMPORT.log:1088,1110,1154,1179` |
| baseline тех же четырёх тестов на `5980d35` | PASS — 4/4 Success; `S2_TARGETED_REIMPORT_BASELINE.log:1088,1128,1149,1187` |
| non-unity/no-PCH после guard `generation==0` | PASS — `S2_POSTOPEN_BUILD_NONUNITY.log:20`, `Result: Succeeded` |
| `Perf.InstrumentationCounters` после guard | PASS — `S2_POSTOPEN_PERF.log:1104,1110` |
| `TargetedReimport.*` после guard, reused DB | **FAIL / STOP** — 2/4; `S2_POSTOPEN_TARGETED.log:1088,1110,1154,1179` |
| `TargetedReimport.*` после guard, новая DB | **FAIL / STOP** — 2/4; `S2_POSTOPEN_TARGETED_FRESHDB.log:1088,1110,1154,1179` |
| полный NullRHI `Automation RunTests Mimir` | NOT RUN — остановка по `OPEN-S2-2` |
| guarded force-unity | NOT RUN — остановка по `OPEN-S2-2` |
| `BuildPlugin -StrictIncludes -DisableUnity -NoPCH -NoSharedPCH` | NOT RUN — остановка по `OPEN-S2-2` |
| `git diff --check` | PASS |
| `python tools/check_normative_docs.py` | PASS — `normative docs: OK` |

## 6. Изменённые файлы

Ровно файлы из закрытого списка контракта:

- `Private/Source/MHSourceImporter.cpp` — только `ReimportStaticMesh`;
- `Private/Source/MHSourceComposition.cpp` — только контрактный guard в
  `MHCreateIncrementalSourceAnalysisServices`;
- `docs/receipts/source_s2.md` — эта STOP-квитанция.

`MHStaticMeshImporterTest.cpp`, `MHPerformanceTrace.*`, другие production-файлы
и нормативные документы не изменялись. Удалённых тестов нет.

## 7. OPEN-вопросы

### OPEN-S2-1 — закрыт близнецом

Закрыт 2026-09-02 контрактным коммитом `e1dd4f3`: full scan при
`bRecreated || GetGeneration() == 0`. Реализация — `2064483`. Проверка выявила
отдельный конфликт ниже; решение `OPEN-S2-1` не переинтерпретируется.

### OPEN-S2-2 — generation не привязана к Source Root

- **Контекст:** после первого полного скана generation остаётся ненулевой в
  общей `Saved/MimirBridge/ProjectIndex.sqlite`. Следующая fixture меняет
  Source Root, но `MHRefreshGeneratedAssetProjection` открывает ту же валидную
  БД и делает только `ReplaceGeneratedAssets`; incremental service видит
  `GetGeneration() > 0` и upsert одного FBX, хотя текущий root никогда не был
  просканирован. Это воспроизведено и на новой БД, без изменения тестов.
- **Вопрос:** каким нормативным способом отличать «generation относится к
  текущему Source Root» без запрещённого изменения schema/index format:
  близнец расширяет контракт на root-aware lifetime/guard в composition layer,
  либо предоставляет другое согласованное правило и закрытый список?
- **Временное fail-closed правило:** тесты не менять; дополнительные source
  paths не выводить из живых UObject/receipt; новый root-aware механизм не
  изобретать;
  полный suite и последующие build-гейты считать не пройденными; S2 не
  переводить в `IN REVIEW`, PR не открывать.
- **Статус:** **OPEN — нужен ответ владельца/близнеца контракта.**

## 8. Строка трекера

Не изменена: `S2 | READY`. Переход в `IN REVIEW` запрещён до закрытия
`OPEN-S2-2` и зелёного `TargetedReimport.*`/полного suite.
