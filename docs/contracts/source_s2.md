> Status: NORMATIVE · Architecture version: Recipe Model v2.1 · Supersedes: — · Контракт среза S2 (линия Source) для внешнего исполнителя (пишет и принимает близнец)

# Контракт S2 — targeted reimport без FullScan

Основание: KICKOFF §6 (S2), `docs/16_recipe_model.md` §0 (Source-плоскость),
`docs/10_source_protocol_v5_plan.md` §3 (граница S4/S6: `UpsertPaths` — точка
входа инкрементальных обновлений; full scan — явная команда и пересоздание БД).

## Что уже есть в ветке (не переписывать)

Ветка `source/s2-targeted-reimport-upsert` от `origin/main`. Red-коммит:

- `FMHReimportPerfScope::AddIncrementalPaths(Count)` — recorder для поля
  `incremental_paths` отчёта `MH_PERF_REIMPORT` (поле есть с M0, никогда не
  писалось);
- в `Mimir.V5.Composite.Perf.InstrumentationCounters` (`MHStaticMeshImporterTest.cpp`)
  два assert'а targeted reimport переведены на цель S2:
  `full_scan_count_delta == 0`, `incremental_paths == 1`. RED-лог: см. ниже.

## Задача

1. `UMHSourceImporter::ReimportStaticMesh` (`MHSourceImporter.cpp`, ~строка 1185):
   вместо `MHCreateDefaultSourceAnalysisServices` (→ `FullScan`) использовать
   существующий `MHCreateIncrementalSourceAnalysisServices(SourceRoot, {абсолютный
   путь source-файла меша из receipt}, Services, Update, bUsedFullScan, Error)`.
   Абсолютный путь строится из `ReceiptSourcePath`, как уже делает проверка
   существования файла выше по функции.
2. Если `bUsedFullScan == false` — `PerfScope.AddIncrementalPaths(1)`; если
   `true` (БД пересоздана при открытии) — ничего не добавлять: `full_scan_count_delta`
   честно покажет 1. Это единственный допустимый full scan внутри targeted reimport.
3. Остальные вызовы `MHCreateDefaultSourceAnalysisServices` (bulk import,
   commandlet, level subsystem, menus) **не трогать** — это явные команды.
4. Тракер: S2 → `IN REVIEW`, после merge → `MERGED #<PR>`.

## Закрытый список файлов

- `Private/Source/MHSourceImporter.cpp` (только `ReimportStaticMesh`)
- документы: `docs/receipts/source_s2.md`, `docs/RECIPE_EXECUTION_STATUS.md`,
  `docs/10_source_protocol_v5_plan.md` §3 — одно предложение в абзаце S0
  («targeted reimport делает `UpsertPaths` своего пути; full scan — только явная
  команда и пересоздание БД (S2)»).

Тесты и `MHPerformanceTrace.*` не менять. Мешает другой тест — STOP + OPEN.

## Запрещено

- `FullScan` в любом пути targeted reimport, кроме пересоздания БД;
- изменение формата индекса, receipt'ов, хэшей; изменение composite/preview-кода;
- параллельный старый путь под флагом.

## Acceptance

1. `Mimir.V5.Composite.Perf.InstrumentationCounters` — Success с
   `full_scan_count_delta=0 incremental_paths=1` в строке `MH_PERF_REIMPORT`.
2. `Mimir.V4.StaticMesh.TargetedReimport.*` — Success (существующие тесты
   targeted reimport не меняются).
3. Полный NullRHI suite — 0 failed; число тестов не уменьшилось.
4. `MH_PERF_REIMPORT` до/после на собственном host: `analysis_services_ms`
   в квитанции (ожидание — падение на порядок: M0 baseline 121 мс при 3 файлах).
5. Гейты KICKOFF §9 (non-unity/no-PCH, force-unity, StrictIncludes,
   `git diff --check`, `check_normative_docs.py`).
6. Квитанция `docs/receipts/source_s2.md`; PR в `main`, merge — после проверки близнеца.

## Host исполнителя и правила git

См. `docs/contracts/recipe_r1_1.md` §«Host исполнителя». Работать в отдельном
клоне/worktree; никогда не делать `git pull` чужой ветки, стоя на `main`;
`main` не пушить.
