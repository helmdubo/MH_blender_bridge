# R6-D1b (Recipe Model v2.1) — хэндлы узлов подкомпозита под effective-родителем

Статус: **MERGED** (близнец, #129). Редактирование узлов вложенного определения
через корневое размещение: докончивает R6-D0 (контекст) и R6-D1a (запрет
прямого редактирования инстансов). Публикация — R6-D2.

## 1. Что сделано

- `AMHCompositeActor::SetEditScope(InvocationNodePath)`: сессия Placement
  Edit получает scope — вызываемое определение (`EditScopeComposite` из
  узла резидентного плана) и его effective-родитель в пространстве актора
  (`EditScopeParentLocal = Invocation.WorldMatrix`).
- `SyncEditScopeHandles()`: для каждого узла плана с родителем-вызовом —
  хэндл `USceneComponent` (тег `MHScope.Handle:i`, не «MH.» — retirement
  компилятора их не трогает; жизнь — у сессии) в мире `Node.WorldMatrix ×
  basis`. Пересоздаются/переставляются после каждой материализации сессии,
  уничтожаются на выходе из edit-mode, при Undo (R5-F) и `ClearDerivedComponents`.
- `SetPlacementEditMode(true)` под scope: `EditingDocument` — документ
  вызываемого определения (через реестр endpoint'ов), `LastEditHandleTransforms`
  — scope-хэндлы; `GetEditedCompositeDocument` валидируется по ним.
- `Tick`: `EditedLocal = HandleWorld × inverse(EditScopeParentLocal ×
  LastEditBasis)` → `EditingGraph->Composites[child].Nodes[i]` и draft;
  дальше прежний путь — `MHResolvePreviewGraph` на редактируемом графе (все
  вызовы этого определения внутри размещения следуют draft'у — семантика
  общего определения) → компиляция → `SyncEditScopeHandles`. Другие
  размещения того же определения не меняются до публикации (D2). Cancel →
  `SetPlacementEditMode(false)` → rebuild из неизменённого рецепта.
- `BeginEditNestedComposite` (subsystem) вводит корень в edit-mode под scope
  в транзакции («Edit MH Composite Contents»).
- Переписанное ожидание D0: открытие контекста теперь входит в edit-mode (одна
  материализация с хэндлами) — асserts «не перестраивает / не двигает
  revision» заменены на «входит в edit-mode под scope, план на месте».

## 2. Тесты (red `c870e13`)

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Composite.EditContext.NestedHandlesEditDraftUnderParent` | root `[mesh A][composite child(mesh C @ z=40)]` с basis (translate + yaw 90°), два размещения: после Begin — edit-mode, один scope-хэндл у корня в мире `child.node × invocation × basis`; сдвиг хэндла +100 X, `Tick` → draft `Nodes[0].TranslationCm = edited world × inverse(parent world)`, инстанс mesh C у A сдвинут на +100 X, живых инстансов у A столько же, B нетронут; Cancel — edit-mode снят, хэндлы удалены, mesh C на исходном месте, байты ребёнка не изменились |
| `…EditContext.NestedInvocationOpensDraft` (D0, переписан) | вход в edit-mode под scope вместо «не перестраивает» |

## 3. Гейты

| Gate | Результат |
|---|---|
| RED (`c870e13`) | `R6D1B_RED_TEST.log`: Fail ×2 (оба теста EditContext) |
| GREEN non-unity/no-PCH build | `R6D1B_GREEN_BUILD2.log`: Succeeded |
| `Mimir.V5.Composite.EditContext` | `R6D1B_GREEN_TEST4.log`: 2/0 |
| полный NullRHI suite | `R6D1B_GREEN_FULL.log`: `Success=223 Fail=0` (222 + 1) |
| force-unity | `R6D1B_FORCE_UNITY.log`: Succeeded |
| `BuildPlugin -StrictIncludes` | `R6D1B_STRICT.log`: ExitCode=0 (Success) |
| `git diff --check`, `check_normative_docs.py` | чисто / OK |

## 4. Изменённые файлы

Editor: `Public|Private/Composite/MHCompositeActor.{h,cpp}`,
`Private/Composite/MHCompositeLevelSubsystem.cpp`. Tests:
`MHCompositeEditContextTest.cpp`. Docs: `docs/RECIPE_EXECUTION_STATUS.md`,
эта квитанция.

## 5. Вопросы

Открытых нет. Первый зелёный прогон `R6D1B_GREEN_TEST2` завершился без
результатов (редактор вышел молча, без crash-папки); повтор того же бинарника
(`R6D1B_GREEN_TEST3/4`) — 2/0 стабильно, полный suite — 223/0. Воспроизвести не
удалось; если повторится в поле — смотреть `Saved/Crashes` и `-ForceLogFlush`.
Вне среза (замер R8): drag сейчас идёт через полную
материализацию сессии (`RemoveOwner`+`Add`), не через reseed-diff пула —
корректно, но не минимально. Multiselect родитель+потомок не возникает:
хэндлы — только узлы верхнего уровня вызываемого определения; более
глубокий уровень — ещё один Edit Contents. Следующий — R6-D2 (Apply Shared
Definition: публикация draft в `.composite` ребёнка, обновление всех
размещений).
