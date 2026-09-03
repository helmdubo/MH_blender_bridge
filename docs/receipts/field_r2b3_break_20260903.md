# Полевой тест owner после R2b-3 + R2c (2026-09-03): Break композита

Статус: **три дефекта Break, зафиксированы; reseed/undo/reimport — без
регрессий**. Установка: портфолио, плагин `e504748` (= PR #90 head, содержит
R2c #88).

## 1. Оценка owner

> Reseed, undo, reimport работают нормально, не сломались.

Break: (1) очень долгий; (2) снимает не верхний слой, как в Dagor, а разбирает
композит **полностью** в плоский набор статик-мешей; (3) после Break → Undo в
сцене два меша, клик по любому выделяет оба, move/rotate/scale двигает только
один; число бакетов в акторе утроилось относительно оригинального composite.

## 2. Диагноз по коду (близнец)

| # | Симптом | Причина в коде | Куда относится |
|---|---|---|---|
| 1 | долгий Break | `UMHCompositeLevelSubsystem::BreakComposites`: на актор — `UMHProofCacheSubsystem::BuildProofNow` (applied graph полного closure + `MHCheckGeneratedAssetClaims` tag-запрос на **каждый** ключ closure + полный resolve), затем `GEditor->AddActor` **на каждый лист** плана (сотни `AStaticMeshActor` в одной транзакции: спавн, Modify, hit-proxy, selection). Сложность O(листьев) по спавну + O(closure) по Asset Registry | R4-pre (Break) |
| 2 | плоский разбор вместо верхнего слоя | `MHCollectBreakSpecs` итерирует `Plan.Leaves` (все глубины). В Dagor «split» снимает только верхний уровень: дочерние композиты остаются композитами, random-узел → выбранный вариант как самостоятельная сущность | R4-pre (Break), после ресёрча Dagor |
| 3 | дубли и «двойное выделение» после Undo | Break внутри `FScopedTransaction`: спавнит актор на лист, затем `EditorDestroyActor` композита. Undo восстанавливает актор **вместе с его plan-view компонентами** (созданы с `RF_Transactional`, `PlanViewFlags`), а `PostEditUndo → RebuildComposite` строит компоненты заново; массивы учёта (`DerivedComponents`, `TopLevel…`, `LeafMaterializations`) — `Transient/DuplicateTransient`, после Undo не знают о восстановленных компонентах → старые не ретируются, новые добавляются: 2 меша на лист, бакеты ×3 (восстановленные + первый rebuild + ещё один rebuild от `OnConstruction`). «Клик выделяет оба» — восстановленные компоненты без строк materialization → hit-proxy на актор целиком | OPEN-R-1 / R4 (тот же корень, что Undo 15,7 с из первого полевого теста) |

Общий корень (2) и (3): Break построен как «materialize всё → спавн всего»,
Undo — как «restore + rebuild» без согласования транзакционности plan-view
компонентов. Оба решаются в R4 (пулы/хэндлы: компоненты пула не транзакционны,
актор восстанавливает материализацию из состояния в `PostEditUndo`; Break —
операция над верхним слоем рецепта, не над листьями).

## 3. Решения

- Ресёрч исходников Dagor (daEditorX: создание композита из выбранных сущностей;
  split/break композита; сиды дочерних сущностей; undo) — внешний агент,
  контракт `docs/contracts/research_dagor_composite_ops.md`; результат —
  reference note, без кода.
- Правки Break/Undo — срез R4-pre после ресёрча (близнец), до пулов R4 или
  внутри R4 — по результату ресёрча.
