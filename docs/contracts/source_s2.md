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
  `full_scan_count_delta == 0`, `incremental_paths == 1`.

RED-коммит `2978373`; лог `E:\MimirComposite_R_M0_20260902\Saved\Logs\S2_RED_TEST.log`,
строки 1105–1123:

```text
MH_PERF_REIMPORT resource_key=static_mesh:targeted_mesh_… full_scan_count_delta=1 incremental_paths=0 analysis_services_ms=96.610 …
Result={Fail} Name={InstrumentationCounters}
Expected 'targeted reimport performs no full scan' to be 0, but it was 1.
Expected 'targeted reimport upserts exactly its own source path' to be 1, but it was 0.
```

Baseline для acceptance-пункта 4: `analysis_services_ms=96.610` при трёх payload'ах
в Source Root фикстуры.

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

## Ответ на OPEN-S2-1 (близнец, 2026-09-02): guard «индекс без full scan»

Причина провала `TargetedReimport.*` (2/4): их фикстуры импортируют меш, минуя
скан, поэтому индекс никогда не проходил full scan (`GetGeneration() == 0`), и
upsert одного FBX упирается в неиндексированный `.material`. В редакторе этого
не бывает: startup-скан (U0c) заполняет индекс до любого targeted reimport.
Нормативное решение — fail-closed guard, а не проекция зависимостей и не правка
тестов:

- в `MHCreateIncrementalSourceAnalysisServices` (`Private/Source/MHSourceComposition.cpp`)
  условие full scan расширяется: `bRecreated || Index->GetGeneration() == 0`
  (индекс ещё не имеет ни одного завершённого full scan); в этом случае
  `bOutUsedFullScan = true`, `full_scan_count_delta = 1` честно. Иначе —
  `UpsertPaths`, как в задаче 1–2.
- Закрытый список расширяется на один файл: `Private/Source/MHSourceComposition.cpp`
  (только эта функция).
- Acceptance без изменений: `Perf.InstrumentationCounters` перед reimport делает
  явный full scan (`MHScanSourcesOperation`), поэтому там `full_scan_count_delta=0
  incremental_paths=1`; `TargetedReimport.*` — 4/4 Success без правок тестов
  (их первый reimport выполнит один full scan по guard'у).
- Квитанция: раздел «OPEN-S2-1 — закрыт близнецом», строка `MH_PERF_REIMPORT`
  с `analysis_services_ms` до/после (RED 96.6 мс → GREEN 22.2 мс по вашему логу).

OPEN-S2-1: закрыт.


## Ответ на OPEN-S2-2 (близнец, 2026-09-02): индекс привязан к Source Root

Находка исполнителя верна и важнее фикстур: `ProjectIndex.sqlite` не хранит
identity своего Source Root, поэтому при смене корня (в тестах — каждая
фикстура, в редакторе — правка `Project Settings → Source Root`) строки разных
деревьев молча смешиваются, а guard OPEN-S2-1 по `generation` не срабатывает.
По 10 §3 «коррупция или любая аномалия при открытии → файл удаляется и индекс
перестраивается полностью»; смена Source Root — такая аномалия.

**Норма.** `FMHProjectResourceIndex::Open` записывает в `Meta` ключ
`source_root` = физическая каноническая форма корня (10 §13.7, та же
канонизация, что у reader paths) при создании файла и проверяет его при
открытии: ключ отсутствует (старые файлы) или отличается → recreate
(`bOutRecreated = true`, дальше — существующий full scan при пересоздании).
Таблицы, тег `mh.project_index:4`, hash-домен не меняются: `Meta` — key/value,
новый ключ — метаданные файла, не формат строк; старый `.sqlite` пересоздаётся
один раз, миграции нет (10 §3).

**Red-тест уже в ветке** (`Mimir.V5.Source.Index.SourceRootMismatchRecreates`,
коммит близнеца `24f2c8f`; RED-лог `E:\MimirComposite_R_M0_20260902\Saved\Logs\S2_RED2_TEST.log`, строки 1079–1082: `Result={Fail} Name={SourceRootMismatchRecreates}`, `Expected 'a different Source Root recreates the database' to be true`, `Expected 'recreated index starts at generation zero' to be 0, but it was 1`): один `.sqlite`, два корня → при втором
открытии `bRecreated == true`, `GetGeneration() == 0`, строки первого корня
недоступны; повторное открытие с тем же корнем — `bRecreated == false`.

**Закрытый список** расширяется на `Private/Index/MHProjectResourceIndex.cpp`
(только `Open`/создание Meta и чтение Meta; `ScanFullSnapshot` и S0-логику не
трогать). Guard OPEN-S2-1 (`GetGeneration() == 0`) остаётся как второй
fail-closed рубеж.

**Acceptance** дополняется: новый тест Success; `TargetedReimport.*` 4/4 на
переиспользованной и на свежей БД; `Mimir.V4.ProjectIndex.*` без изменений
(normalized dump не включает `Meta`). В `docs/10` §3 — одно предложение в абзаце
S0: «индекс привязан к физическому Source Root (`Meta.source_root`); смена корня
— пересоздание файла (S2)».

OPEN-S2-2: закрыт.
