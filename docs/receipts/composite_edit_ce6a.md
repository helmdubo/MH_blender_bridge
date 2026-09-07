# CE-6a (Composite Edit Mode) — lifecycle guards: сессия кончается с размещением, миром и редактором

Статус: **REVIEW** (близнец). Первая часть карточки CE-6 спецификации
(«world/host deletion, shutdown, zero leftover delegates/proxies/leases после
повторных циклов», A28). Cutover (флаг → default on) и снятие legacy-пути
— CE-6b после полевой приёмки owner'ом; перф-метрики — CE-6c.

## 1. Что сделано

- `UMHCompositeLevelSubsystem::Initialize/Deinitialize`: подписки на
  `FWorldDelegates::OnWorldCleanup` и `GEngine->OnLevelActorDeleted`;
  cleanup мира редактируемого размещения или удаление самого размещения →
  `ResetEditSession` (сессия, проекция, lease, режим — всё закрывается;
  восстанавливать нечего). `Deinitialize` (выход из редактора) снимает
  подписки и закрывает открытую сессию.
- PIE: `PreBeginPIE` → `RequestCancel` (Save/Discard/Stay) — паритет с
  `ULevelInstanceEditorMode::OnPreBeginPIE` (тоже `ExitModeCommand`, без
  отмены PIE); при «Stay» PIE стартует, проекция — editor-only/transient,
  в PIE-мир не дублируется. Задокументировано как принятое поведение
  движка, не как gap.
- Save/cook: projection-актор `RF_Transient`, `bIsEditorOnlyActor`, hidden
  in game, вне World Outliner, без actor package; компоненты `RF_Transient`
  (проверено тестом).
- Найдено тестом циклов: `UMHCompositeEditProjection::Close` уничтожал
  projection-актор, пока тот был выделен (рамка с CE-3d) — движок не снимает
  выделение с pending-kill актора («SelectActor: invalid flags»), в selection
  set копились висячие element-ссылки, и после GC разрушение мира падало
  на `Assertion failed: RegisteredElementType` (`TypedElementRegistry.h:592`;
  red-прогон и первый green: 1 из 3 / 7 из 36 тестов до краша). Теперь
  `Close` сначала снимает выделение с компонентов проекции и с актора.

## 2. Тесты (red `af6a09f`)

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Composite.EditMode.Lifecycle.RepeatedCyclesLeaveNothingBehind` | 20 циклов Begin (вложенный/корневой попеременно, с правкой через раз) → Cancel: в каждом цикле ровно один projection-актор; после — нет сессии, режима, projection-акторов, инстансы A на местах, B не двигался, undo-история пуста, после GC ни одного объекта `UMHCompositeEditSession` |
| `…Lifecycle.RootDeletionAndWorldCleanupEndTheSession` | `DestroyActor(A)` в сессии → сессия/режим/проекция закрыты; сессия на B, `DestroyWorld` → сессия и режим закрыты |
| `…Lifecycle.ProjectionNeverSavesOrCooks` | projection-актор transient, editor-only, hidden, вне Outliner, без external package; компоненты transient |

## 3. Гейты

| Gate | Результат |
|---|---|
| RED (`af6a09f`) | `CE6A_RED_TEST.log`: 1 из 3 завершён, затем краш на разрушении мира (висячие element-ссылки выделенного projection-актора — сама находка среза, см. §1) |
| GREEN non-unity/no-PCH build | `CE6A_GREEN_BUILD2.log`: Succeeded |
| `Mimir.V5.Composite.EditMode` | `CE6A_GREEN_TEST3.log`: 36/0 (первый green `CE6A_GREEN_TEST.log`: краш на 7-м тесте, см. §1; `CE6A_GREEN_TEST2.log`: Lifecycle 3/0) |
| полный NullRHI suite | `CE6A_FULL.log`: `Success=268 Fail=0 (265 + 3)` |
| force-unity | `CE6A_FORCE_UNITY.log`: Succeeded |
| `BuildPlugin -StrictIncludes` | `CE6A_STRICT.log`: ExitCode=0 (Success) |
| `git diff --check`, `check_normative_docs.py` | чисто / OK |

## 4. Изменённые файлы

Editor (2): `Public|Private/Composite/MHCompositeLevelSubsystem.{h,cpp}`.
Tests: `MHCompositeEditLifecycleTest.cpp` (новый). Docs:
`docs/RECIPE_EXECUTION_STATUS.md`, эта квитанция.

## 5. Вопросы

- Конфликт с активным Level Instance Edit: `IsCompatibleWith` режима не
  исключает `EditMode.LevelInstance`; произвольный WP/LI edit nesting спец
  не обещает — полевой сценарий owner'а решит, нужен ли отказ входа.
- CE-6b (после полевой приёмки): `bCompositeEditModeV2` → default on,
  legacy-путь (хэндлы, `Tick`, `SetPlacementEditMode`, `EditingDocument/
  EditingGraph` актора, глобальный Enter-роутинг) — снятие с переносом
  legacy-тестов на сессию.
- CE-6c: метрики (cold/warm Enter/Exit, 100 Updates, peak components,
  память после 50 циклов) на хосте, пороги по baseline.
