# R5-F (Recipe Model v2.1) — исправления жизненного цикла по аудиту 2026-09-05

Статус: **REVIEW** (близнец). Срез по результатам внешнего аудита
(`E:\MH_bridge_R5_R7_review_plan_5e9c55b.md`, §2.1/2.3/2.4/2.6); каждая
находка подтверждена по коду близнецом до правки.

## 1. Что сделано

- **§2.1 Рассинхрон после миграции бакета (дефект R5b-1).** В
  `AMHCompositeActor::ReconcileEndpoint` первый вызов `RefreshPooledRows()`
  обновлял все строки разом, после чего у остальных строк `Before ==
  Row.Component`, `LeafPlacementComponents` оставался со старым компонентом, а
  при первом листе на незатронутом меше миграция вообще не отражалась
  (`bMigrated == false`). Следующий move актора давал
  `MH_E_PLACEMENT_STATE_DESYNC` и полный rebuild. Теперь один проход: каждая
  строка резолвит свой хэндл и синхронно обновляет обе проекции; getter не
  участвует в поддержании инварианта.
- **§2.3 Undo при активной edit-сессии (дефект до R5).** `PostEditUndo`
  завершает transient-сессию (флаги, editing graph/document, tick) до
  `ClearDerivedComponents`/`RebuildComposite`; иначе gate `bPlacementEditMode`
  блокировал восстановление и сессия тикала над пустым представлением.
- **§2.4 `MigrateBucket` терял политику бакета.** Пересобирается только
  меш-зависимая часть дескриптора (section policies, доступность коллизии
  без body setup); collision/render/mobility/visibility/appearance layout/
  override-материалы остаются бакетными. Тот же дескриптор после миграции
  по-прежнему адресует тот же бакет.
- **§2.6 Game View.** Pool-актор больше не `bIsEditorOnlyActor` и не
  `HiddenInGame` — иначе Game View (G) скрывал все пуловые инстансы.
  Transient/DuplicateTransient-флаги по-прежнему держат его вне save, cook
  и PIE. **Полевая проверка owner: G на портфолио.**

## 2. Тесты (red `c5177ff`)

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Composite.Pool.ReimportThenMovePreservesMapping` | два размещения (одно с мешем B первым листом), реимпорт с дельтой дескриптора A: оба актора отмечают миграцию (`PreviewRevision`), leaf-массив совпадает со строками; последующий move — без desync и без rebuild |
| `Mimir.V5.Composite.Pool.UndoDuringEditRestoresPreview` | move в транзакции → Edit Mode → Undo: сессия завершена, позиция и preview восстановлены, 3 живых инстанса; новая сессия стартует и завершается штатно |
| `Mimir.V5.Composite.Pool.MigrationPreservesPolicy` | бакет с нестандартными cast shadow / ray tracing / mobility: после `ReconcileMesh` (descriptor) новый компонент сохраняет политику, тот же дескриптор попадает в тот же бакет, второй бакет не создаётся |
| `Mimir.V5.Composite.Pool.PoolActorVisibleInGameView` | pool-актор не hidden-in-game и не editor-only, компонент не editor-only, флаги `RF_Transient | RF_DuplicateTransient` |

## 3. Гейты

| Gate | Результат |
|---|---|
| RED (`c5177ff`) | `R5F_RED_TEST.log`: четыре теста Fail |
| GREEN non-unity/no-PCH build | `R5F_GREEN_BUILD.log`: Succeeded |
| `Mimir.V5.Composite.Pool` | `R5F_GREEN_TEST.log`: 11/0 |
| полный NullRHI suite | `R5F_GREEN_FULL.log`: `Success=216 Fail=0` (212 + 4) |
| `BuildPlugin -StrictIncludes` | `R5F_STRICT.log`: ExitCode=0 (Success) |
| force-unity | `R5F_FORCE_UNITY.log`: Succeeded |
| `git diff --check`, `check_normative_docs.py` | чисто / OK |

## 4. Изменённые файлы

Editor: `Private/Composite/MHCompositeActor.cpp`,
`Private/Composite/MHInstancePool.cpp`. Tests:
`MHCompositePoolMaterializationTest.cpp`, `MHInstancePoolTest.cpp`. Docs:
`docs/RECIPE_EXECUTION_STATUS.md`, эта квитанция.

## 5. Вопросы

Открытых нет. Остальные пункты аудита (§2.5 метрики bulk/индексы — замер в
R8; выбор во вьюпорте — R5b-2; перестройка R6 — программа) — вне среза.
