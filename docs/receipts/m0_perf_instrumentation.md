# M0 — perf-инструментация загрузки карты, startup-скана и targeted reimport

Статус: **READY FOR REVIEW**. Оптимизации не выполнялись; срез только
измеряет существующее поведение.

## 1. База и границы

- ветка: `codex/m0-perf-instrumentation`;
- база: `origin/main` `813dd70`;
- stock Unreal Engine 5.7.4;
- собственные host'ы исполнителя:
  - GREEN и build-гейты:
    `E:\MimirComposite_M0_20260901\HostProject`;
  - RED:
    `E:\MimirComposite_M0_RED_20260901\HostProject`;
- audit-host Lead и portfolio-проект owner не открывались и не изменялись;
- Engine не изменялся, fork Engine не создавался;
- изменения ограничены `ue/MimirComposite` и этой квитанцией;
- `golden/`, `reference/`, Blender-addon, wire-грамматика, resolver,
  closure, plan, подписи, enum/binary и реестры E/W не изменялись;
- ни `FinishCompilation`, ни all-options traversal, ни `FullScan`, ни
  notification/rebuild path не оптимизировались и не переставлялись.

Коммиты реализации до квитанции:

1. `160698f` — `Add red-first performance instrumentation coverage`;
2. `062ad90` — `Expose live Mimir performance traces`;
3. `cc52e6d` — `Harden performance trace overhead and verbosity`.

## 2. Red-first

Automation-тест:
`Mimir.V5.Composite.Perf.InstrumentationCounters`.

Тест первым коммитом добавил random-композит с двумя различными mesh-опциями,
из которых seed выбирает ровно одну, а также проверки текущего полного скана
при targeted reimport и cvar-гейта.

RED снят на точном коммите `160698f`, до production-инструментации. Сборка
RED-host прошла, сам тест закономерно завершился `Result={Fail}`:

```text
Expected 'all-options unique meshes' to be 2, but it was 0.
Expected 'selected unique meshes' to be 1, but it was 0.
Expected 'all-options exceeds selected meshes' to be true.
Expected 'trace one emits one map-load report' to be 1, but it was 0.
Expected 'manual scan records one full scan' to be 1, but it was 0.
Expected 'manual scan uses two snapshot passes' to be 2, but it was 0.
Expected 'trace one emits one scan report' to be 1, but it was 0.
Expected 'targeted reimport records current full scan' to be 1, but it was 0.
Expected 'trace one emits one reimport report' to be 1, but it was 0.
```

Лог:
`E:\MimirComposite_M0_RED_20260901\HostProject\Saved\Logs\M0_RED.log`,
строки 1105–1121.

На финальном коде тот же тест — `Result={Success}`. Дополнительно он
проверяет, что при `mh.PerfTrace 0` initial build, полный source scan и
targeted reimport продолжают выполняться, но не создают ни одного отчёта.

GREEN-лог:
`E:\MimirComposite_M0_20260901\HostProject\Saved\Logs\M0_FOCUS_FINAL.log`,
строки 1099–1114.

## 3. Реализация

### 3.1 Cvar и test API

Добавлен `mh.PerfTrace`:

- `0` — отчётность выключена, новых строк нет;
- `1` — только агрегаты;
- `2` — агрегаты и отдельные `*_VERBOSE`-строки с пятью самыми дорогими
  акторами/ресурсами и именами all-option mesh.

При `0` не создаются scope-аккумуляторы, не снимаются дополнительные cycle
timestamps и не строятся именные коллекции. Существующие session-only метрики
остаются без изменения семантики. Новые агрегаты доступны тестам через
`MHGet*PerfReportForTests()` и не сериализуются.

### 3.2 `MH_PERF_MAPLOAD`

Scope расположен только вокруг существующего initial build в
`PostRegisterAllComponents`. После завершения same-frame пачки один deferred
ticker печатает единый агрегат; перестраивание вне initial-build в этот отчёт
не попадает.

Переиспользованы семь существующих `EMHPlacementStage`, definition-cache и
mutation metrics. Cache hit/miss добавлены к той же session-only структуре.
Reuse считается пересечением указателей previous/final вида, включая
существующий orphan-union MH-tagged instance components. ISM buckets и
instances читаются из уже полученного final view актора.

Определение уникальности для ключевой пары счётчиков: полный
`FMHResourceKey`, то есть `(EMHResourceKind::StaticMesh, LogicalName)`.
Одинаковый mesh в нескольких узлах/вложенных композитах считается один раз.
All-options множество собирается во время уже исполняемого обхода immutable
definition graph; selected множество — из уже построенных `Plan.Leaves`.
Дополнительная загрузка ресурсов или обход мира не выполняются.

`all_option_meshes_compiling` фиксирует mesh, для которых существующий
`FinalizeDeferredMeshes` уже обнаружил `IsCompiling()` перед штатным барьером;
selected compiling — пересечение этого множества с selected mesh keys.

### 3.3 `MH_PERF_STARTUP_SCAN`

`MHScanSourcesOperation` получает только метку причины (`startup`, `manual`,
`commandlet`); выбор analysis services и поведение скана не изменены.

Инструментация встроена в два существующих `EnumerateKnownPaths`, два
`ScanOnePath` прохода и SQLite-транзакцию. Поэтому:

- `enumerated_files` — размер уникального inventory одного прохода;
- `enumerated_bytes` — сумма размеров признанных файлов первого прохода;
- `scan_passes` — фактическое число проходов;
- `hashed_files` и `parsed_*` — фактические операции, поэтому при стабильном
  двухпроходном snapshot они вдвое больше уникального inventory;
- `io_hash_ms` не включает parser;
- `parse_ms` измеряет parser по типам;
- `sqlite_ms` включает открытие/чтение прежних ключей и полную запись
  транзакции.

`full_scan_count_delta` увеличивается только после успешного существующего
`FullScan`; production-решения этот счётчик не читают.

### 3.4 `MH_PERF_REIMPORT`

Один RAII-scope добавлен в существующий
`UMHSourceImporter::ReimportStaticMesh`. Он только размечает:

1. создание default analysis services;
2. существующий import/build;
3. единый batch compilation wait;
4. сохранение пакетов;
5. projection + notifications.

Центральная существующая notification-воронка сообщает активному scope
уникальные resource keys и суммарное число/время реально вызванных
`Actor->RebuildComposite()`. При неактивном scope rebuild выполняется прежней
веткой без timestamp. `incremental_paths` намеренно остаётся нулём: M0 не
переводит targeted reimport на incremental services.

## 4. GREEN и гейты

| Gate | Результат |
|---|---|
| `Mimir.V5.Composite.Perf.InstrumentationCounters` | **1/1 Success**, 0 failed |
| `Mimir.V5.Composite` на generic own host | **71/71 Success**, 0 failed |
| полный NullRHI `Mimir` с `-MHGoldenRoot=<repo>/golden` | **172/172 Success**, 0 failed |
| non-unity/no-PCH editor build | **114/114 actions**, `Result: Succeeded` |
| guarded force-unity | **17/17 actions**, `Result: Succeeded` |
| `BuildPlugin -StrictIncludes -DisableUnity -NoPCH -NoSharedPCH` | **114 editor + 11 Development game + 11 Shipping game actions**, `BUILD SUCCESSFUL` |
| `git diff --check` | PASS |

Команда guarded lane включала ровно
`-NoEngineChanges -ForceUnity -DisableAdaptiveUnity -NoPCH -NoSharedPCH -WarningsAsErrors`.

Полный suite запускался на generic own host. В нём 17 существующих
host-guard кейсов завершились Success с информационным `NOT RUN`: map/parity/
cottage lanes требуют специально оснащённый `MimirCompositeV5S6` host.
Итоговые 172 результата и отсутствие fail/fatal проверены по строкам
`Test Completed`, а не только по exit code.

Логи:

- полный suite:
  `E:\MimirComposite_M0_20260901\HostProject\Saved\Logs\M0_FULL.log`;
- guarded force-unity:
  `E:\MimirComposite_M0_20260901\HostProject\Saved\Logs\M0_FORCE_UNITY_UBT.log`;
- StrictIncludes:
  `E:\MimirComposite_M0_20260901\HostProject\Saved\Logs\M0_STRICT_UAT.log`.

Ни один существующий test expectation не менялся. Production-пути не
принимают решений по новым метрикам; порядок и результат операций относительно
`813dd70` не изменены.

## 5. Собственный baseline инструментации

Это синтетические числа собственного host, доказывающие читаемость отчётов;
они не заменяют portfolio baseline owner.

### 5.1 Reopen сохранённой карты с одним размещённым композитом

```text
MH_PERF_MAPLOAD composite_actors=1 root_composites_unique=1 definition_cache_hits=1 definition_cache_misses=0 all_option_composites=1 all_option_unique_meshes=1 selected_unique_meshes=1 all_option_meshes_compiling=0 selected_meshes_compiling=0 build_applied_graph_ms=0.000 resolve_composite_plan_ms=0.078 load_endpoints_ms=0.005 wait_static_mesh_compilation_ms=0.000 compile_placement_ms=0.210 register_components_ms=0.020 destroy_retired_components_ms=0.000 components_created=4 components_reused=0 components_destroyed=0 ism_buckets=1 ism_instances=3 total_ms=9.931
```

Лог:
`E:\MimirComposite_MaterialDoc_Build_20260901\HostProject\Saved\Logs\M0_MAPLOAD_BASELINE.log`,
строка 1131. Reopen действительно прошёл production lifecycle и напечатал
отчёт. Последующая save-validation этого старого синтетического fixture
отдельно отвергла его пустой `UStaticMesh` без SourceModels; поэтому этот
прогон используется только как smoke отчётности, не как Automation gate.

### 5.2 Startup scan синтетического Source Root

```text
MH_PERF_STARTUP_SCAN trigger=startup enumerated_files=3 enumerated_bytes=47602 scan_passes=2 hashed_files=6 parsed_fbx=4 parsed_material=2 parsed_composite=0 parsed_profile=0 enumeration_ms=0.509 io_hash_ms=0.602 parse_ms=38.471 sqlite_ms=14.901 full_scan_count_delta=1 total_ms=113.828
```

### 5.3 Targeted reimport managed static mesh

```text
MH_PERF_REIMPORT resource_key=static_mesh:targeted_mesh_d12803ce4366d404e2af009563e07945 full_scan_count_delta=1 incremental_paths=0 analysis_services_ms=121.281 import_build_ms=11.875 compile_wait_ms=0.206 save_packages_ms=14.613 projection_ms=127.466 notified_resource_keys=1 notified_actors=1 actor_rebuild_ms_total=49.875 total_ms=275.667
```

Строки 5.2–5.3 находятся в
`E:\MimirComposite_M0_20260901\HostProject\Saved\Logs\M0_FOCUS_FINAL.log`,
строки 1100 и 1108.

Контрольная random-фикстура в том же логе дала ключевое доказательство:

```text
all_option_unique_meshes=2 selected_unique_meshes=1
```

## 6. Полевой протокол owner после merge

Перед каждым прогоном в консоли: `mh.PerfTrace 1`. Сохранять лог целиком.

1. Cold DDC: открыть portfolio-карту и дождаться первого кадра плюс окончания
   нотификаций. Сохранить `MH_PERF_MAPLOAD` и `MH_PERF_STARTUP_SCAN`.
2. Warm DDC: штатно закрыть и снова открыть ту же карту. Сохранить те же две
   строки.
3. Warm без startup scan: повторить пункт 2 с существующей настройкой скана
   off; если отдельной настройки нет — временно очистить Source Root и после
   замера восстановить. Разница с пунктом 2 — цена startup scan.
4. Targeted reimport одного крупного managed mesh: сохранить полный
   `MH_PERF_REIMPORT`.
5. Для поиска конкретных дорогих ресурсов повторить нужную операцию с
   `mh.PerfTrace 2`; вернуть `mh.PerfTrace 0` после замера.

Главные решения следующих срезов должны опираться на полевые отношения
`all_option_unique_meshes / selected_unique_meshes`, долю
`wait_static_mesh_compilation_ms / total_ms`, цену startup scan и
`actor_rebuild_ms_total`, а не на синтетические числа выше.

## 7. Изменённые файлы

- `Source/MimirCompositeEditor/Public/Performance/MHPerformanceTrace.h`;
- `Source/MimirCompositeEditor/Private/Performance/MHPerformanceTrace.cpp`;
- `Source/MimirCompositeEditor/Public/Composite/MHCompositePlacementMetrics.h`;
- `Source/MimirCompositeEditor/Private/Composite/MHCompositePlacementMetrics.cpp`;
- `Source/MimirCompositeEditor/Private/Composite/MHCompositeActor.cpp`;
- `Source/MimirCompositeEditor/Private/Composite/MHCompositeDefinitionSubsystem.cpp`;
- `Source/MimirCompositeEditor/Private/Composite/MHCompositeResolvedPlan.cpp`;
- `Source/MimirCompositeEditor/Private/Composite/MHCompositePlacementEvents.cpp`;
- `Source/MimirCompositeEditor/Public/Diagnostics/MHSourceOperations.h`;
- `Source/MimirCompositeEditor/Private/Diagnostics/MHSourceOperations.cpp`;
- `Source/MimirCompositeEditor/Private/Index/MHProjectResourceIndex.cpp`;
- `Source/MimirCompositeEditor/Private/Source/MHSourceImporter.cpp`;
- `Source/MimirCompositeTests/Private/MHStaticMeshImporterTest.cpp`;
- `docs/receipts/m0_perf_instrumentation.md`.

## 8. Вопросы Lead

Открытых архитектурных вопросов нет. Временное fail-closed допущение для
границы map-load пачки явно зафиксировано: агрегируются initial builds,
завершившиеся до следующего core ticker tick; более поздняя регистрация
начинает новый отчёт. Это не меняет lifecycle и не удерживает акторы или
ресурсы.
