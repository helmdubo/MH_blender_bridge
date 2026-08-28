# Shared composite preview cache — кандидат на отдельное рассмотрение

Дата: 2026-08-28. Статус: **CANDIDATE / НЕ РАТИФИЦИРОВАН**.

Это предложение по измерениям, границам оптимизаций и их acceptance, а не
норматив Source Protocol, разрешение на реализацию или новый активный срез.
Номер и объём следующей работы назначает owner отдельно. Документы 10/11/12
этим текстом не изменяются.

Текущий `v5/s6.0-preview-defects`, начатый от `7b46df5`, **не содержит shared
cache**. Его исправления подтверждённых дефектов rendering, hierarchy и Build
preflight не требуют кэша. Также вне S6.0 оставлены idempotent assignment,
nonblocking pending-mesh, watcher-import scope и per-operation lookup.
Ни одну из этих пяти оптимизаций нельзя включать обратно под видом
необходимого сопутствующего исправления без отдельного согласования.

## 1. Что является доказательством, а что ещё не измерено

Ветка `codex/fix-ue-composite-preview` сохраняется как **evidence-only**:

| Коммит | Содержание и граница использования |
|---|---|
| [21624a7](https://github.com/helmdubo/MH_blender_bridge/commit/21624a7e0ebbb7a3efbc476ece23a483d94e15ae) | Первоначальный bundle исправлений и оптимизаций, включая shared cache; не готовый состав будущего среза. |
| [6aeac4b](https://github.com/helmdubo/MH_blender_bridge/commit/6aeac4bf9d541c1e0280f6f4440c295eb51060ab) | Исправление refresh после invalidation и batch Rebuild; именно этот production snapshot использован в feedback-прогоне ниже. |
| [a0da2e4](https://github.com/helmdubo/MH_blender_bridge/commit/a0da2e4) | Docs-only публикация результатов независимых probe и границ доказательств. |

Цепочка: `7b46df5 → 21624a7 → 6aeac4b → a0da2e4`. Эти три коммита не являются
предками текущей S6.0; их код не переносится целиком или автоматически.
[Исторический review response](https://github.com/helmdubo/MH_blender_bridge/blob/a0da2e4/docs/receipts/ue_composite_preview_review_response.md)
содержит прямое сравнение трёх baseline-дефектов на `7b46df5` и `6aeac4b`.
Необходимость persistent shared cache из этих трёх RED-тестов не следует.

Для первоначального `21624a7` не было сохранённого RED baseline чистого main;
последующие зелёные Automation не являются измерением выигрыша в задержке.
Сопоставимых фазовых timings на owner-сцене пока нет. Причины и доли полного
пользовательского лага, FPS-выигрыш и естественный бесконечный источник событий
не считаются установленными.

### 1.1 Обязательный RED: feedback в одном Tick

Прочитаны исходник и отчёт отдельного прогона. Побайтная копия исходника
сохранена [в репозитории как evidence](evidence/MHReviewFeedbackProbe.cpp.txt),
чтобы воспроизведение не зависело от локального диска исполнителя.
Это **не собираемый тест S6.0**: API cache в этом срезе отсутствует.
Для исторического воспроизведения копия добавляется как `.cpp` только в
`MimirCompositeTests/Private` отдельного worktree на `6aeac4b`, затем запускается
`Automation RunTests Mimir.Audit.CacheFeedback` в изолированном host.
Ожидаемый результат там — один FAIL и один PASS; в будущий кандидат тест
переносится без ослабления assertions.

Локальные исходные артефакты:

- [MHReviewFeedbackProbe.cpp](E:/MimirComposite_ReviewFixed_Source_20260828/ue/MimirComposite/Source/MimirCompositeTests/Private/MHReviewFeedbackProbe.cpp:163),
  SHA256 `C960D42F48D6887B95B1DCADB36B54AF3D6877D5BE010F2C28B12DCF8F999AF1`.
- [feedback-probe/index.json](E:/MimirComposite_ReviewFixed_Host_20260828/Reports/feedback-probe/index.json),
  SHA256 `F54EE8F638D8A424EA6E330A46B78C923CEC76786EF447510B9DAD9372F76699`;
  `reportCreatedOn = 2026.08.28-14.00.14`.
- [feedback-probe.log](E:/MimirComposite_ReviewFixed_Host_20260828/feedback-probe.log).

`Mimir.Audit.CacheFeedback.OneRefreshPerTick` — **FAIL**. Hook live-tag
admission корневого ассета (`Caller=Uncategorized`) инъецирует максимум три
`OnAssetUpdatedOnDisk`. `FAssetData` заранее создан вне hook, поэтому это не
рекурсивный сбор тегов. Startup/watcher importer приостановлен test-only gate;
fixture не записывает source или Content и использует уже готовый mesh.

```text
FEEDBACK first_tick attempts=4 injections=3 cap=3 serial_before=5530 serial_after=5533 current=1 needs_refresh=0
Expected one Tick graph refreshes: 1; actual: 4.
Expected injections in the first Tick: 1; actual: 3.
FEEDBACK settled additional_ticks=0 attempts=4 injections=3 serial=5533 current=1 needs_refresh=0
```

Подтверждены четыре admission/rebuild attempts внутри **одного** вызова
`FTSTicker::Tick`, а не четыре разных кадра. После исчерпания cap две тихие
итерации не создают новые попытки. Это ограниченный injected proof усиления
feedback; естественный бесконечный цикл в owner-сцене **не доказан**.

В проверенном коде `PreviewQueueRefresh` сбрасывает ticker handle в начале
callback. Invalidation, пришедшая при admission, получает возможность поставить
новый zero-delay callback, который исполняется в том же Tick. Само название
«deferred» или использование `AddTicker` не доказывает перенос на следующий Tick.

### 1.2 Новая missing K: PASS, не найденная дыра

Соседний `Mimir.Audit.CacheFeedback.NewMissingKeyTargetedRecovery` — **PASS**
(Success с ожидаемыми предупреждениями отсутствующего ассета). В том же отчёте:

```text
RECOVERY failed: retained=1 current=0 seed=100 serial=5521
RECOVERY targeted_notify: ticks=0 serial_before=5522 serial_after=5523 current=1 seed=100 error=<empty>
```

`FAppliedPlanBuilder::Load` сохраняет K в `Dependencies` **до** `Lookup.Load`
([код evidence snapshot](https://github.com/helmdubo/MH_blender_bridge/blob/6aeac4b/ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositeResolvedPlan.cpp#L196)).
Актор сохраняет обнаруженные зависимости и после отказа. После появления
managed K targeted `MHNotifyGeneratedResourceChanged(K)` восстанавливает план
без единого ticker-вызова; Seed и basis сохранены, leaf и SelectedDependencies
содержат K. Отложенный global refresh не мог замаскировать этот результат.

Итого этот отчёт содержит один PASS с предупреждениями и один FAIL, а не
принятый lifecycle кэша. Сохранение missing K и targeted recovery — защитное
acceptance уже работающего поведения; их нельзя описывать как необходимую
починку или заменять ожиданием глобального скана.

## 2. Сначала instrumentation, потом выбор оптимизации

До любой из пяти оптимизаций нужен отдельный измерительный baseline на
принятом main/S6.0. Инструментирование само не меняет порядок admission,
загрузок, rebuild, signature, мутаций или уведомлений. Результаты — opt-in
Unreal Insights/агрегированный отчёт в `Saved`, не новый файл в Source Root,
не теги и не история в SQLite. Лог на каждый leaf по умолчанию не добавляется.

Обязательные фазовые spans:

| Имя | Измеряемая граница |
|---|---|
| `BuildAppliedGraph` | Сбор полного applied source graph, closure и admission receipts. |
| `ResolveCompositePlan` | Единственный резолвер: graph + placement Seed → plan. |
| `LoadEndpoints` | Поиск/загрузка mesh и actor-class endpoints перед материализацией плана. |
| `WaitStaticMeshCompilation` | Реальное синхронное ожидание компиляции; отдельно — wall-time от регистрации pending до готовности при будущем async-пути. |
| `CompilePlacement` | Подготовка и применение компонентной дельты; родительский span для вложенных операций. |
| `RegisterComponents` | Регистрация вновь созданных компонентов. |
| `DestroyRetiredComponents` | Удаление компонентов предыдущего preview, не вошедших в новый результат. |

Записываются inclusive/exclusive длительности и вложенность: ожидание внутри
`BuildAppliedGraph`/`LoadEndpoints` и регистрация внутри `CompilePlacement`
не суммируются второй раз в общий итог. Async latency не выдаётся за блокировку
game thread. В отчёт включается end-to-end wall-time операции.

Минимальные счётчики: operation/root/actor, причина rebuild, номер Tick,
число admissions и flush, invalidation epoch/reason, queued/pending actors,
размер source closure и resolved leaves, число теговых запросов и загруженных
UObjects, compiling meshes, созданные/сохранённые/удалённые компоненты.
Позже — hit/miss/eviction и память cache. Счётчики не входят в протокольные
байты, closure hash, `ResolvedSignature` или applied state.

Сценарии до/после — одинаковые данные и настройки: первый cold placement,
повторный warm placement того же root, несколько root с общей зависимостью,
Build из mesh + nested composite, move/Seed change, targeted dependency change,
burst watcher-событий и quiet ticks. Отдельно измеряются startup/manual full
scan и повторная операция: их время нельзя приписывать одному placement.
Фиксируются SHA, Engine/build mode, hardware, asset/closure/component counts,
число повторов, median/p95 и разброс. Числовой бюджет owner выбирает после
baseline; этот кандидат не выдумывает миллисекунды или обещанный процент.

## 3. Пять независимых кандидатов

### 3.1 Idempotent assignment

Измерить число повторных assignment от actor factory/editor callbacks.
Пропуск допустим лишь при доказанно неизменных входах и **текущем** успешно
принятом preview. Одного равенства указателя `CompositeAsset` недостаточно:
могли измениться Seed, closure, settings или результат предыдущего admission.
Явный Rebuild, отказ, pending, отозванный plan и local edits не маскируются
fast path. Принятие этой оптимизации не требует shared cache.

Acceptance: повторное действительно тождественное assignment не запускает
лишние resolve/load/component operations; изменения входов и rejected state
не пропускаются. Move не пересэмплирует random и не меняет Seed/signature.

### 3.2 Per-operation lookup

Измерить повторные Asset Registry/live-tag queries на одну операцию.
Предлагаемый scope snapshot/map — **одна операция**, без persistent cache.
Он обязан сохранять все competing claims на ResourceKey: чужой путь,
неправильный класс и malformed receipt не позволяют выбрать «правильный»
canonical path победителем. Class/path filter не является доказательством
единственности. Изменение claims при загрузке требует ревалидации/отказа,
а не допуска устаревшего snapshot.

Acceptance: число повторных запросов уменьшается на измеренной closure;
duplicate/ambiguous/unmanaged отказы прежние, в том числе claim вне generated
folders и claim, появившийся во время загрузки. Missing K сохраняется до
неудачного lookup. API и оптимизация не вводятся вместе с новым индексом.

### 3.3 Nonblocking pending-mesh

Сначала отделить время загрузки от `FinishCompilation`. Preview может
возвращать отдельное pending-состояние **до** чтения locked mesh fields/live
tags и до компонентной мутации. Pending — не успешный graph/cache entry и не
fresh plan; старое отображение можно сохранить, но нельзя выдавать его Break
как текущий результат. Nonblocking-проверка нужна и на endpoints после cache
hit: mesh мог выгрузиться и снова начать компилироваться.

Retry после compile completion — по weak handles и после выхода из callback,
без рекурсивного `FinishCompilation`; actor/world destruction и shutdown
должны отменять очередь безопасно. Runtime/cook не получают editor services
или новую nonblocking-политику автоматически.

Acceptance: cold compiling mesh не блокирует preview на ожидании и не
создаёт частичных компонентов; completion восстанавливает правильный plan;
GC/reload после hit, несколько pending meshes, закрытие world/editor и
настоящая ошибка receipt проверяются отдельно. Scheduler проходит §4.

### 3.4 Watcher-import scope

Измерить, сколько unrelated `NO_CHANGE` items доходит до загрузок/логов при
одном изменённом пути. Кандидат affected scope вычисляется **до** исполнения
import policies: старые и новые keys изменённых путей, их reverse dependents
и необходимые forward closures. Старые edges/keys нужны до destructive
upsert удаления или rename; пустой scope означает skip, никогда `All`.

Глобальная проверка identity/целостности source не ослабляется. Startup и
явный full scan остаются полными. Обход охватывает невыбранные options и
placement profiles, а не текущий Seed. Source-only `.placement` не получает
phantom CREATE. Нет новой схемы SQLite, cross-process suppression, marker
файлов или подмены внешних Blender-событий self-publish событиями UE.

Acceptance: unrelated NO_CHANGE mesh не загружается и не попадает в план
локального импорта; add/remove/restore/rename, changed profile, nonselected
option, mixed batch и empty batch дают правильный набор. Targeted notify
успешных импортов обновляет заинтересованные placements; промежуточный
неполный результат не допускается как fresh.

### 3.5 Shared definition cache — только после измерений выше

Кандидат разделяет между placements **seed-free applied graph**, а не
канонический «вид ассета» и не план, уже разрешённый чужим Seed. Единственный
резолвер и `FMHResolvedCompositePlan` сохраняются; `mh.random_stream:1`,
`mh.random_resolver:2`, golden bytes и подпись не меняются. Thumbnail не
возвращается в scope. Source closure по всем options не заменяется selected
dependencies одного placement.

Перед реализацией требуется отдельная карта validity/invalidation inputs:
полное замыкание, six-tag/duplicate claims, UObject lifetime, mesh material
slots, material/texture state, ActorClassRegistry, влияющие settings,
инлайненные profile bytes и private AppliedSourceHash. Root SourceHash,
canonical path или root pointer по отдельности не достаточны. Это список
того, что нужно доказать, а не уже утверждённый формат cache key.

Cache остаётся восстановимым in-memory derived state. Он не меняет чистую
проекцию индекса и importer-owned profile freshness по 10 §13.4.1;
AppliedSourceHash не переносится в теги, SQLite или hash композита. Кэшируются
только успешные admissions; admission, затронутый relevant-key или global
invalidation, не получает fresh lease. Ошибка сохраняет обнаруженные, включая
missing, зависимости.
Нужны явные lifetime/eviction/память и teardown правила до реализации.

Acceptance shared cache дополнительно: одинаковый root/closure/Seed даёт те
же choices/samples/transforms/SelectedDependencies/signature, что fresh path;
разные placements независимы; profile/claim/settings/material/texture changes
и новая duplicate claim отзывают старый допуск; invalidation одного actor в
batch не оставляет уже обновлённого соседа со stale plan. Выигрыш требуется
показать поверх четырёх более узких оптимизаций, а не предположить.

## 4. Scheduler: обязательная защита от подтверждённого feedback

Если shared cache/очередь refresh будет принята, исходный
`Mimir.Audit.CacheFeedback.OneRefreshPerTick` становится обязательным RED→GREEN
тестом. Нельзя ослабить assertions или убрать tag-hook ради зелёного результата.

- Invalidation сохраняет epoch/eviction/pending **всегда**, в том числе во
  время rebuild. `if (bRebuilding) return` с потерей invalidation запрещён:
  событие может представлять настоящую смену receipt/claim.
- За один `FTSTicker::Tick` выполняется максимум один deferred flush и не
  более одной попытки для одного actor в его snapshot. Новые события внутри
  flush остаются pending до **следующего Tick**, а не нового zero-delay
  callback в той же итерации. Конкретный механизм ещё не выбран.
- Релевантный admission epoch, изменившийся при сборке, не разрешает пометить
  промежуточный результат current. При непрерывных событиях система остаётся fail-closed,
  но возвращает управление editor; после конечного burst делает clean pass.
- Break не лечит stale plan сам и не игнорирует freshness; upstream refresh
  восстанавливает допуск самостоятельно. Quiet ticks не выполняют polling
  rebuild. Shutdown удаляет делегаты и очереди без доступа к мёртвым UObject.

На неизменённой cap=3 fixture первый Tick должен дать `attempts=1`,
`injections=1`, возросший serial и сохранённую потребность в refresh; остальные
инъекции переносятся на следующие Tick. После чистой попытки — текущий plan,
прежняя signature и отсутствие работы в двух quiet ticks. Конкретные абсолютные
serial 5530/5533 — историческое наблюдение, не новые golden constants.

`NewMissingKeyTargetedRecovery` остаётся PASS **без Tick**: этот test проверяет
адресное восстановление, а не deferred feedback scheduling. Требование одного
deferred flush не даёт права молча заменить существующий targeted путь
зависимостью от global refresh.

## 5. Предлагаемый порядок допуска

1. Owner подтверждает только измерительную работу; сохраняются baseline SHA,
   fixtures, instrumentation overhead и фазовые результаты без оптимизаций.
2. По измерениям выбирается минимальная независимая оптимизация. Каждая из
   четырёх узких имеет отдельный diff/сравнение и может быть отклонена без
   shared cache. Их наличие не является разрешением на пятый пункт.
3. Shared cache получает отдельное архитектурное решение с complete validity
   map, scheduler/lifetime proof и бюджетом памяти. Evidence-only commits
   используются для сравнения, не как готовый merge bundle.
4. На будущем C++ candidate обязательны оба unity-гейта 09 §5 и полный
   `Automation RunTests Mimir`, плюс соответствующие RED→GREEN/guard probes.
   При затрагивании общей runtime-границы повторяются Editor/PIE/packaged
   parity и packaged smoke; их нельзя заменить editor cache-тестом.
5. Квитанция разделяет baseline/after, measured/injected/inferred, correctness
   и performance. Неизмеренный owner-сценарий явно остаётся NOT RUN. Новые
   коды/протокольные решения из этого кандидата не следуют; реальная дыра
   действующего контракта идёт обычным OPEN/STOP процессом.

Этот документ создан чтением сохранённых источников и отчётов. Его подготовка
не включает правок production/runtime, normative docs, `reference/`, `golden/`
или Engine; UE build/Automation для этого документа заново не запускались.
Он не объявляет ни feedback-дефект исправленным, ни proposed cache принятым.
