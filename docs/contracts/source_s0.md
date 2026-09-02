> Status: NORMATIVE · Architecture version: Recipe Model v2.1 · Supersedes: — · Контракт среза S0 (линия Source) для внешнего исполнителя (пишет и принимает близнец)

# Контракт S0 — инкрементальный индекс: `(size, mtime)` как фильтр, один проход снапшота

Основание: KICKOFF §6 (S0), `docs/16_recipe_model.md` §0 (Source-плоскость),
`docs/10_source_protocol_v5_plan.md` §3 (индекс — чистая проекция; формат
`mh.project_index:4`, hash-домен и таблицы **не меняются**).

## Что уже есть в ветке (не переписывать)

Ветка `source/s0-incremental-index` от `origin/main`. Red-коммит содержит тест
`Mimir.V5.Source.Index.IncrementalScanSkipsUnchangedHashes`
(`MHProjectResourceIndexTest.cpp`): фикстура Source Root с ≥3 payload'ами;
`mh.PerfTrace 1`; первый `FullScan` (свежая БД) → отчёт A; второй `FullScan`
без изменений файлов → отчёт B; затем изменение одного payload (байты и mtime)
→ третий скан → отчёт C. Ожидания: `B.hashed_files == 0`, `B.scan_passes == 1`,
`C.hashed_files == 1`, нормализованный дамп индекса после B и C равен дампу
свежей БД после полного пересканирования (существующий acceptance «удаление
.sqlite → идентичный индекс», 10 §3), resolver outcomes по каждому ключу
совпадают.

RED-коммит `46f7187`; лог `E:\MimirComposite_R_M0_20260902\Saved\Logs\S0_RED_TEST.log`,
строки 1083–1095 (проверки идентичности проекции и resolver outcomes на этом
коммите **проходят** — падают только три инкрементальных assert'а):

```text
Result={Fail} Name={IncrementalScanSkipsUnchangedHashes}
cold: enumerated_files=3 scan_passes=2 hashed_files=6
unchanged: enumerated_files=3 scan_passes=2 hashed_files=6
Expected 'unchanged rescan is one snapshot pass' to be 1, but it was 2.
Expected 'unchanged rescan hashes nothing' to be 0, but it was 6.
changed: enumerated_files=3 scan_passes=2 hashed_files=6
Expected 'changed rescan hashes exactly the changed payload' to be 1, but it was 6.
```

## Задача

1. `FMHProjectResourceIndex::FImpl::ScanFullSnapshot` (`MHProjectResourceIndex.cpp`):
   один проход. Для каждого пути: читать `(size, mtime)` через `IFileManager`;
   если в открытой БД есть строка `ResourceCandidates` с тем же `path`,
   `size`, `mtime` и `parse_status == ok` — переиспользовать `raw_hash`,
   `kind/name`, диагностику и **не читать байты** (`hashed_files` не растёт);
   иначе — прежний путь: чтение, `MHRawPayloadHash`, parse. Двойная
   энумерация и повторный проход сравнения удаляются; защита от гонки
   «файл изменился во время скана» сохраняется на уровне одного файла
   (сравнение `(size, mtime)` до и после чтения — уже есть в `ScanOnePath`).
2. Флаг подтверждения: cvar `mh.SourceIndex.VerifyHashes` (0 по умолчанию):
   при `1` хэш считается всегда и сверяется с сохранённым; расхождение при
   равных `(size, mtime)` → `MH_W_SOURCE_INDEX_STALE_FINGERPRINT` в Diagnostics
   (derived, перевычислимо) и обновление строки. Никаких новых таблиц и
   полей: `(size, mtime)` уже хранятся.
3. Формат индекса не меняется: `mh.project_index:4`, таблицы, статусы,
   рёбра, rebuild-identity. Если для реализации нужна миграция формата —
   STOP + OPEN (16 §9), не изобретать.
4. `MH_PERF_STARTUP_SCAN`: `scan_passes` = 1 на полный скан; добавить
   `reused_fingerprints` (число строк, переиспользованных по `(size, mtime)`).
5. Существующий assert `manual scan uses two snapshot passes == 2` в
   `Mimir.V5.Composite.Perf.InstrumentationCounters` (`MHStaticMeshImporterTest.cpp`)
   документировал прежнее поведение (квитанция M0 §3.3): заменить на `== 1`
   отдельной строкой квитанции. Другие тесты не менять; мешают — STOP + OPEN.

## Закрытый список файлов

- `Private/Index/MHProjectResourceIndex.cpp`
- `Public/Index/MHProjectResourceIndex.h` (только если нужен test-API; формат — нет)
- `Public/Performance/MHPerformanceTrace.h`, `Private/Performance/MHPerformanceTrace.cpp`
  (`reused_fingerprints`)
- `Private/Index/*` — cvar (можно в `MHProjectResourceIndex.cpp`)
- тесты: `MHStaticMeshImporterTest.cpp` (только строка `scan_passes`)
- документы: `docs/receipts/source_s0.md`, `docs/RECIPE_EXECUTION_STATUS.md`,
  `docs/10_source_protocol_v5_plan.md` §3 — один абзац «Инкрементальный скан (S0)».

## Запрещено

- парсинг FBX в скане сверх нынешнего (это S1); `FullScan` на targeted reimport (S2);
- изменение hash-домена, формата `.sqlite`, receipt'ов, тегов;
- любые «истории»/tombstones в индексе (10 §3: чистая проекция);
- изменение composite/preview-кода.

## Acceptance

1. Red-тест зелёный; `InstrumentationCounters` зелёный с `scan_passes == 1`.
2. `MH_PERF_STARTUP_SCAN` до/после на собственном host (одинаковый Source Root,
   второй запуск): `hashed_files` 2N → 0, `total_ms` — в квитанции.
3. Полный NullRHI suite — 0 failed; гейты KICKOFF §9.
4. Квитанция `docs/receipts/source_s0.md`; тракер S0 → `IN REVIEW`/`MERGED`.
5. PR в `main`; merge после проверки близнеца.

## Host исполнителя

См. `docs/contracts/recipe_r1_1.md` §«Host исполнителя».

## Возврат PR #73 (близнец, 2026-09-02): racy fingerprint

**Находка.** На реализации `f1a88a6` тест `Mimir.V4.BulkImport.CrashBetweenPassesRetries`
падает детерминированно (3/3 на host'е близнеца; на `main` до S0 — 4/4 Success):
`Expected 'injected pass boundary was observed' to be true`. Причина: в этом
конвейере mtime кандидата хранится с точностью до **секунды** (в БД после
провала: `mtime=639239457290000000`, кратно 10⁷ тикам). Тест переписывает PNG
того же размера в ту же секунду → `(size, mtime)` совпадают → S0
переиспользует старый fingerprint (в строке остаётся хэш прежнего payload), и
импорт считает `NO_CHANGE`. Это дыра фильтра `(size, mtime)`, а не теста.

**Норма.** «Свежий» кандидат не переиспользуется: если `now − mtime` меньше
окна `RacyWindow` (2 секунды; константа с комментарием, не cvar), файл
читается и хэшируется как изменённый. Всё остальное правило S0 без изменений.
Аналог — racy-git.

**Red-тест уже в ветке** (коммит `5a17389`, близнец):
`Mimir.V5.Source.Index.IncrementalScanRehashesSameSecondChange` — payload той же
длины переписывается сразу после первого скана; ожидание: повторный скан видит
новый хэш и `hashed_files ≥ 1`. RED-лог `E:\MimirComposite_R_M0_20260902\Saved\Logs\S0_RED2_TEST.log`, строки 1080–1087:

```text
Result={Fail} Name={IncrementalScanRehashesSameSecondChange}
first: hashed_files=2 reused=0
same-second: hashed_files=0 reused=2
Expected 'same-second rescan sees the rewritten payload' to be "blake3-160:1d4d…", but it was "blake3-160:28ff…".
Expected 'same-second rescan hashes the fresh payload' to be true.
```

**Дополнение acceptance:**
1. `IncrementalScanRehashesSameSecondChange` — Success.
2. `Mimir.V4.BulkImport.CrashBetweenPassesRetries` — Success **3 из 3** отдельных
   прогонов (лог каждого в квитанции).
3. `IncrementalScanSkipsUnchangedHashes` остаётся Success. **OPEN-S0-1 закрыт
   близнецом (2026-09-02):** паузы в тесте не было (ошибка контракта); фикстура
   теста теперь состаривает payload'ы на 10 с через `SetTimeStamp` до холодного
   скана (коммит близнеца в ветке), поэтому «unchanged» проход переиспользует
   fingerprint детерминированно, а последующая перезапись остаётся свежей и
   хэшируется. Норма без изменений; тесты исполнитель по-прежнему не меняет.
4. Smoke `mh.SourceIndex.VerifyHashes=1` запускать через
   `-dpcvars=mh.SourceIndex.VerifyHashes=1` (cvar перед `Automation RunTests`
   в `-ExecCmds` подвешивает harness — воспроизведено близнецом и с
   `mh.PerfTrace 0`, к S0 не относится). Ожидание: `hashed_files == enumerated_files`
   в каждом проходе.
5. Полный suite 178/178 (+1 новый тест).

Закрытый список файлов не расширяется. Квитанцию дополнить разделом
«Возврат 1: racy fingerprint» с red/green строками нового теста и 3 прогонами
`CrashBetweenPassesRetries`.
