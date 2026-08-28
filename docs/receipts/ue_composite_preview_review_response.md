# Ответ на review ветки UE composite preview — 2026-08-28

Статус: **MERGE BLOCKED**. Это диагностический отчёт и запрос owner-решения,
не новый срез и не разрешение на merge. Production-код основной ветки в этом
прогоне не менялся. Во время диагностики новых commit/push, reset, rename,
merge или установки в owner-проект не было.

После диагностики owner явно запросил commit/push для внешнего ревью.
Этот отчёт и две уточнённые квитанции публикуются отдельным docs-only коммитом;
проверенный production snapshot остаётся `6aeac4b`. Номер среза и объём
исправлений этим запросом не ратифицированы, merge остаётся заблокирован.

## 1. История, ответственность и первоначальная мотивация

Проверено через live `git ls-remote` до публикации этого docs-only отчёта:

- `origin/main = 7b46df58f5293fc009975e0123e3b37024d10a84`;
- `origin/codex/fix-ue-composite-preview = 6aeac4bf9d541c1e0280f6f4440c295eb51060ab`;
- поверх main находятся ровно `21624a7` и `6aeac4b`; оба не смержены;
- S6 уже смержен в истории как `00776c6`, до этой ветки.

`21624a7` — **33 файла, +2486/-114**, не 19 файлов.
`6aeac4b` — **8 файлов, +798/-8**.
Совокупный diff двух коммитов от main — **35 файлов, +3280/-118**.
Полный текст `git log --stat` приведён в конце.

Мотивация не была инициативным hardening без пользовательского дефекта:
owner сообщил о пропавшей геометрии, невозможности клика, плоской иерархии,
лагах размещения/Build, потоке несвязанных импортных операций и FBX-осях;
отдельно попросил исправлять UE-плагин. Однако запрос на исправление этих
дефектов сам по себе не является ратификацией shared definition cache.

Формулировка «production-код пока не менял» была неточной без указания этапа:
на этапе определения владельца Break я не вносил новых production-правок,
но ветка уже содержала production-коммит `21624a7`. Это уточнено в
`ue_composite_break_freshness_fix.md`. Статус публикации актуализирован в обеих
квитанциях; старое «ветка локальная / push не выполнялся» больше не оставлено
как описание текущего состояния.

Для первоначального `21624a7` в сохранённых gates **не найден red-прогон
чистого main**. WIP failures и последующие 111/111 не подменяют такой прогон.
Сравнение S5 / 21624a7 из квитанции Break доказывало только регрессию, внесённую
самим preview-cache патчем. Ниже — новый прямой эксперимент на main.

## 2. Новый baseline-эксперимент: main против 6aeac4b

Созданы два detached worktree и два отдельных тестовых host. Production
Editor/Runtime source в каждом snapshot проверен `git diff --exit-code`
против соответствующего SHA; отличий нет. Диагностические изменения — только
новые тесты и RenderCore/RHI test-module dependencies на main.

- Main source: `E:/MimirComposite_ReviewMain_Source_20260828`.
- Fixed source: `E:/MimirComposite_ReviewFixed_Source_20260828`.
- Main host: `E:/MimirComposite_ReviewMain_Host_20260828`.
- Fixed host: `E:/MimirComposite_ReviewFixed_Host_20260828`.

В обоих snapshots **одинаковые байты** двух тестовых файлов:

| Файл | SHA256 |
|---|---|
| MHReviewBuildProbe.cpp | `CD2F0EBDF89198215F81D10A9DC4CBFE52B4A98C73F35FCC677B5F73FD758F3F` |
| MHReviewPreviewProbe.cpp | `0D7822CFD8FD4B5BF97C3D5AEB68B6C51EABB243B5C03BDA1709E6E45DA7B25F` |

Build probe выделяет fixture и первый test body из
`MHCompositeBuildPreflightTest.cpp` на `6aeac4b`, без двух тестов нового
lookup API; переименованы тест/helper identifiers для отсутствия unity-коллизий.
Preview probe использует fixture и RHI body из
`MHCompositePreviewRegressionTest.cpp`, без Cache header/cleanup и
cache-specific тестов; отдельно добавлен public-API hierarchy assertion.
Ожидания одинаковы на обеих production-базах.

Оба host собраны stock UE **5.7.4 CL51494982**, force-unity,
DisableAdaptiveUnity, NoPCH/NoSharedPCH, NoEngineChanges, NoUBA:

- main: 20 actions, PASS, 47.89 s (`build.log`);
- fixed: 20 actions, PASS, 54.24 s (`build.log`).

Это диагностические сборки, не новый полный StrictIncludes/cook acceptance.

Run: `UnrealEditor.exe <host>/MimirCompositeV5S6.uproject`,
`-RenderOffscreen -MHPreviewRenderSmoke -NoAssetRegistryCache -unattended`,
`-ExecCmds="Automation RunTests Mimir.Audit.MainBaseline"`,
`-TestExit="Automation Test Queue Empty"`, отдельные `-ReportExportPath`.
Это D3D12/RHI-прогон, **не** headless NOT RUN guard.

| Проверка | main 7b46df5 | 6aeac4b |
|---|---|---|
| BuildPreflightRejectsBeforeMutation | FAIL: после отказа появляются source, целевая папка, UObject package и UAsset | PASS |
| SemanticHierarchy | FAIL: parent листа = MHCompositeRoot | PASS: parent листа = MH_Node_1 |
| RenderedNativeHitProxy | FAIL: proxy скрыт при G=1, до и после rebuild/move/seed | PASS |

Отчёты:

- main `Reports/main-probe/index.json`: **0 passed / 3 failed**;
- fixed `Reports/fixed-probe/index.json`: **3 passed / 0 failed**
  (2 Success + 1 SuccessWithWarnings).

Логи: main `main-probe.log`; fixed `fixed-probe.log`.
TestExit завершил оба процесса с exit 0 даже при красном main; результат
проверялся по JSON и событиям Automation, а не по одному process exit.

Вырезка main:

```text
Expected 'preflight failure writes no source payloads' to be true.
Expected 'preflight failure creates no destination directory' to be false.
Expected 'preflight failure creates no UObject package' to be null.
Expected 'preflight failure creates no UAsset file' to be false.
Expected 'pass 1 actual proxy shown (G=1)' to be true.
Expected 'pass 3 actual proxy shown (G=1)' to be true.
hierarchy leaf-parent=MHCompositeRoot actor-root=MHCompositeRoot editor-only=1
```

**Важная граница доказательства:** на main и fixed нативный `HActor` найден
во всех четырёх passes (G off/on до и после изменений, viewport 357x339).
Поэтому этот тест подтверждает Game View visibility bug, но НЕ воспроизводит
owner-симптом «невидимо/нельзя выбрать и при G off». Save/reload, PostLoad и
реальная owner-сцена этим экспериментом не проверены. Общая производительность
большого композита не измерена; устранение всего лага не заявляется.

Следствие: «на main чинить нечего» опровергнуто. Но необходимость shared cache
из этих красных тестов **не следует**.

## 3. Два замечания об инвалидации — отдельные прогоны

Они проверены новым `MHReviewFeedbackProbe.cpp` только в fixed snapshot,
не production-правкой. SHA256:
`C960D42F48D6887B95B1DCADB36B54AF3D6877D5BE010F2C28B12DCF8F999AF1`.

Run: отдельный `UnrealEditor-Cmd.exe` процесс,
`-nullrhi -NoAssetRegistryCache`,
`-ExecCmds="Automation RunTests Mimir.Audit.CacheFeedback"`.
Отчёт `ReviewFixed_Host/Reports/feedback-probe/index.json`:
**1 passed / 1 failed**. Лог `feedback-probe.log`.
Startup/watcher importer приостановлен существующим test-only gate,
искусственные ресурсы живут в памяти; файлы source/Content здесь не пишутся.

### 3.1 Feedback scheduling: подтверждённый дефект

`PreviewQueueRefresh` сбрасывает handle в начале callback
(`MHCompositePreviewCache.cpp:57`). Invalidation внутри admission ставит ещё
один zero-delay ticker. UE выполняет добавленные из callback zero-delay
tickers в **том же Tick**, не обязательно на следующем кадре
(Engine `Containers/Ticker.cpp:93–95, 138–140`).

Ограниченный injection: hook сбора live tags корневого композита
(`Caller=Uncategorized`) порождает максимум три `OnAssetUpdatedOnDisk`.
`FAssetData` создан до hook, рекурсивного сбора tags нет. После одного Tick:

```text
FEEDBACK first_tick attempts=4 injections=3 cap=3
serial_before=5530 serial_after=5533 current=1 needs_refresh=0
Expected one Tick graph refreshes: 1; actual: 4.
FEEDBACK settled additional_ticks=0 attempts=4 injections=3
```

После исчерпания injection две тихие итерации не вызывают новых попыток.
Прогон конечный. Это доказывает усиление feedback в одном Tick; естественный
источник бесконечного повторения на каждом warm admission в owner-сцене
**не установлен**.

Подавлять саму invalidation при `bRebuildInProgress` небезопасно: изменение
receipt/claim может быть настоящим, и старый snapshot получит ложный fresh
lease. Возможное узкое исправление — сохранять serial/eviction, а ограничивать
повторное scheduling: один flush за Tick, pending-факт переносится дальше.
Production-реализация в этом review не изменялась; если cache сохраняется в
owner scope, этот тест должен стать зелёным до merge.

### 3.2 Новая missing K: гипотеза опровергнута тестом

`FAppliedPlanBuilder::Load` добавляет K в `Dependencies` **до**
`Lookup.Load` (`MHCompositeResolvedPlan.cpp:196–199`).
Failure не очищает набор; Actor сохраняет его через `Append`
(`MHCompositeActor.cpp:306–308`).

Тест вводит НОВУЮ mesh-ссылку, отсутствовавшую в прежнем плане, затем создаёт
managed K и посылает только `MHNotifyGeneratedResourceChanged(K)`.
Между созданием K и notify ни одного ticker вызова нет:

```text
RECOVERY failed: retained=1 current=0 seed=100
RECOVERY targeted_notify: ticks=0 current=1 seed=100 error=<empty>
```

Выбранный ресурс и SelectedDependencies уже содержат новую K; basis/Seed
сохранены. `AssetCreated` мог поставить global refresh в очередь, но она
не исполнялась и не могла замаскировать targeted recovery.

## 4. Что предлагаю owner

Не мержить нынешнюю ветку целиком и не стирать её доказательства reset'ом.

Реальные fixes отделимы от shared cache: rendering flag, semantic attachments,
Build preflight, idempotent assignment, nonblocking pending-mesh handling,
scope watcher-import и per-operation lookup не требуют persistent shared
definition cache между placements. Конкретный состав минимального среза
следует утвердить, а не автоматически включать все 33 файла.

**Рекомендация:** отдельный owner-approved срез исправлений подтверждённых
дефектов; shared definition cache оставить отдельным кандидатом после
инструментирования, как в переданном проекте документа 12.
Номер `V5-S6.0` и переименование ветки не присваиваются исполнителем
самостоятельно. Если owner предпочитает принять bundle вместе с cache,
нужны явный scope и его lifecycle acceptance, включая красный feedback test.

`docs/12_v5_s6_1_s6_2_slices.md` в текущем main отсутствует: проверен именно
переданный owner документ со статусом «кандидат owner freeze». Его секция
«Вне scope» действительно отдельно исключает shared definition cache.
Даже без неё зелёные тесты не заменяли бы отдельное согласование этой
архитектуры.

В основной ветке изменены только две квитанции и этот ответ.
Ни production-код, ни `reference/`, `golden/`, Blender, Engine или owner-проект не
изменялись. Python/Blender suite, StrictIncludes и packaged/cook повторно
не запускались; исторические счётчики не выданы за новые проверки.

## 5. Полный git log --stat проверенных двух коммитов

Команда: `git log --stat --format=fuller 7b46df5..6aeac4b`.

```text
commit 6aeac4bf9d541c1e0280f6f4440c295eb51060ab
Author:     helmdubo <56157514+helmdubo@users.noreply.github.com>
AuthorDate: Fri Aug 28 17:18:37 2026 +0400
Commit:     helmdubo <56157514+helmdubo@users.noreply.github.com>
CommitDate: Fri Aug 28 17:18:37 2026 +0400

    Fix deferred composite preview refresh and batch rebuild freshness

 docs/receipts/ue_composite_break_freshness_fix.md  | 200 +++++++++
 .../Private/Composite/MHCompositeActor.cpp         |  23 +-
 .../Composite/MHCompositeLevelSubsystem.cpp        |  16 +-
 .../Composite/MHCompositePlacementEvents.cpp       |  28 +-
 .../Private/Composite/MHCompositePreviewCache.cpp  |  56 ++-
 .../Public/Composite/MHCompositeActor.h            |   4 +
 .../Public/Composite/MHCompositePreviewCache.h     |   2 +
 .../Private/MHCompositeBreakFreshnessTest.cpp      | 477 +++++++++++++++++++++
 8 files changed, 798 insertions(+), 8 deletions(-)

commit 21624a7e0ebbb7a3efbc476ece23a483d94e15ae
Author:     helmdubo <56157514+helmdubo@users.noreply.github.com>
AuthorDate: Fri Aug 28 14:50:56 2026 +0400
Commit:     helmdubo <56157514+helmdubo@users.noreply.github.com>
CommitDate: Fri Aug 28 14:50:56 2026 +0400

    Harden composite preview invalidation and build dependency validation

 docs/receipts/ue_composite_preview_fix.md          | 214 ++++++++++++
 .../Private/Composite/MHCompositeActor.cpp         |  85 ++++-
 .../Composite/MHCompositeLevelSubsystem.cpp        |  66 +++-
 .../Composite/MHCompositePlacementCompiler.cpp     | 138 +++++++-
 .../Composite/MHCompositePlacementEvents.cpp       |   8 +-
 .../Private/Composite/MHCompositePreviewCache.cpp  | 239 ++++++++++++++
 .../Private/Composite/MHCompositeResolvedPlan.cpp  |  89 +++--
 .../Private/Composite/MHCompositeRuntimeBridge.cpp |   2 +-
 .../Private/Geometry/MHFbxSceneTranslator.cpp      |  19 ++
 .../Private/Index/MHProjectResourceIndex.cpp       |  96 ++++++
 .../Private/Material/MHMaterialImporter.cpp        |   6 +
 .../Private/MimirCompositeEditorModule.cpp         |   3 +
 .../Private/Source/MHSourceImporter.cpp            | 173 ++++++++--
 .../Private/Texture/MHTextureImporter.cpp          |   2 +
 .../Public/Composite/MHCompositeActor.h            |  16 +-
 .../Composite/MHCompositePlacementCompiler.h       |   6 +-
 .../Public/Composite/MHCompositePreviewCache.h     |  23 ++
 .../Public/Composite/MHCompositeResolvedPlan.h     |  22 +-
 .../Public/Index/MHProjectResourceIndex.h          |   3 +
 .../Public/Source/MHSourceImporter.h               |  27 +-
 .../Public/StaticMesh/MHStaticMeshImportData.h     |   4 +-
 .../Private/Random/MHRandomStream.cpp              |  28 +-
 .../Public/Random/MHRandomStream.h                 |   5 +
 .../MimirCompositeTests.Build.cs                   |   2 +
 .../MHCompositeAppliedPlanAdmissionTest.cpp        |   9 +-
 .../Private/MHCompositeBuildPreflightTest.cpp      | 293 ++++++++++++++++
 .../Private/MHCompositePlacementTest.cpp           |   2 +-
 .../Private/MHCompositePreviewRegressionTest.cpp   | 333 +++++++++++++++++++
 .../Private/MHCompositeSeedTest.cpp                |  10 +-
 .../Private/MHCompositeV5Test.cpp                  |  12 +-
 .../Private/MHFbxTransportParityTest.cpp           | 367 +++++++++++++++++++++
 .../Private/MHStaticMeshImporterTest.cpp           |  10 +
 .../Private/MHWatcherScopeTest.cpp                 | 288 ++++++++++++++++
 33 files changed, 2486 insertions(+), 114 deletions(-)
```
