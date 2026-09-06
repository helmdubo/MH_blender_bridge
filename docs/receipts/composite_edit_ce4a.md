# CE-4a (Composite Edit Mode) — event-driven transforms и Undo внутри сессии

Статус: **REVIEW** (близнец). Карточка CE-4a спецификации
(`docs/reference_notes/MH_Composite_Edit_Mode_Spec_19b7515.md` CE-ADR-3,
A07, A11) и решение owner (в): граница Undo по BPP. Структурные команды
draft'а (добавление узлов, Details) — CE-4b.

## 1. Что сделано

- Жест gizmo на компоненте узла проекции идёт через
  `ILegacyEdModeViewportInterface` режима (`FEditorModeTools::StartTracking/
  InputDelta/EndTracking` вызываются viewport'ом раньше собственной
  логики): `StartTracking` открывает одну транзакцию «Move Composite Node»,
  каждый `InputDelta` пересчитывает мировой трансформ компонента по
  семантике движка (перенос — в мире, поворот — вокруг pivot gizmo, т.е.
  компонента, масштаб — покомпонентно аддитивно) и пишет в draft локальный
  трансформ узла под родителем (`UMHCompositeEditProjection::
  GetParentWorldForComponent`: родитель — resolved-узел плана, для
  top-level узлов определения — occurrence; лист random-узла → сам random-
  узел), проекция следует draft'у; `EndTracking` закрывает транзакцию,
  пустой жест (клик без перетаскивания) отменяется — записи нет. Движок
  не трогает компонент сам (`InputDelta` возвращает true), актор проекции
  не Modify'ится — в записи только draft.
- Рамка (actor-only выделение, как при входе) проглатывает жест: v1 не
  перемещает occurrence целиком (CE-4b/owner решит, нужен ли «сдвиг всех
  top-level узлов»).
- Undo/Redo внутри сессии: draft восстанавливается транзакцией
  (`RF_Transactional`, CE-1), `UMHCompositeEditDocument::PostEditUndo`
  бьёт `OnRestored`, сессия обновляет проекцию; сессия и режим живы,
  dirty пересчитывается по canonical bytes.
- Граница истории (BPP, контракт §2 «Undo»): `Enter` режима —
  `GEditor->ResetTransaction` (как `ULevelInstanceSubsystem` при входе в
  Edit), `Exit` — ещё раз (шаги сессии не переживают draft); незакрытый
  жест при `Exit` отменяется. `CancelEditComposite` под CE-backend больше
  не открывает транзакцию (в записи нечего держать; reset внутри
  транзакции движок бы «purged» с ошибкой в лог).
- Перевёрнута характеризация CE-0 `UndoInsideSessionEndsIt` — для legacy-
  пути (флаг выключен) она остаётся верной, под флагом Undo сессию не
  закрывает (`…Transform.GizmoGestureWritesTheDraftInOneUndoStep`).

## 2. Тесты (red `81ac182`)

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Composite.EditMode.Transform.GizmoGestureWritesTheDraftInOneUndoStep` | после Begin история пуста; жест из трёх `InputDelta` (+10 Z) на листе → проекция и draft на +30 Z, dirty; Undo → сессия и режим живы, draft и проекция восстановлены, clean; Redo → снова +30; один шаг; лист под группой (10,0,0): world-drag +5 X → локаль +5 X под группой; под повёрнутым occurrence (yaw 90) world-drag +10 X → компонент в мире +10 X, локаль (0,−10,0) |
| `…Transform.ActorDragIsSwallowedAndEmptyGestureLeavesNoStep` | рамка (actor-only) — жест проглочен, draft чист, записи нет; клик без перетаскивания на узле — записи нет; без выделения — жест не наш |

## 3. Гейты

| Gate | Результат |
|---|---|
| RED (`81ac182`) | `CE4A_RED_TEST.log`: Fail ×2 (`CE4A_RED_TEST2.log`; первый red `9aeb332`/`CE4A_RED_TEST.log`: 1 fail + crash — `UndoTransaction` без своего жеста откатил запись Cancel предыдущего теста в уничтоженный мир (`AMHCompositeActor::PostEditUndo`), тест ужесточён в `81ac182`) |
| GREEN non-unity/no-PCH build | `CE4A_GREEN_BUILD2.log`: Succeeded |
| `Mimir.V5.Composite.EditMode` | `CE4A_GREEN_TEST2.log`: 25/0 (и `CE4A_GREEN_TEST.log` до ужесточения red-теста: 25/0) |
| полный NullRHI suite | `CE4A_FULL.log`: `Success=257 Fail=0 (255 + 2)` |
| force-unity | `CE4A_FORCE_UNITY.log`: Succeeded |
| `BuildPlugin -StrictIncludes` | `CE4A_STRICT.log`: ExitCode=0 (Success) |
| `git diff --check`, `check_normative_docs.py` | чисто / OK |

## 4. Изменённые файлы

Editor (6): `Public|Private/Editing/MHCompositeEditorMode.{h,cpp}`,
`Public|Private/Editing/MHCompositeEditProjection.{h,cpp}`,
`Public|Private/Editing/MHCompositeEditDocument.{h,cpp}` — плюс
`Private/Editing/MHCompositeEditSession.cpp` (bind `OnRestored`, 5 строк) и
`Private/Composite/MHCompositeLevelSubsystem.cpp` (Cancel без транзакции,
6 строк): 8 файлов, из них два — точечные правки; разделить без потери
атомарности (жест ↔ Undo ↔ граница истории) не вышло. Tests:
`MHCompositeEditModeTransformTest.cpp` (новый). Docs:
`docs/RECIPE_EXECUTION_STATUS.md`, эта квитанция.

## 5. Вопросы

- Полевая проверка owner'ом: W/E/R на узле проекции, Ctrl+Z внутри
  сессии, Ctrl+Z до входа (истории нет), Save/Cancel после Undo.
- Масштаб: аддитивно к мировому масштабу компонента (как у движка для
  компонента); под масштабированным родителем локаль пересчитывается через
  `GetRelativeTransform` — при shear/сингулярной рамке (A10) отказа пока
  нет, это CE-4b вместе с reparent.
- Snapping: `InputDelta` получает уже «снапнутые» дельты viewport'а;
  собственных правил нет.
- CE-4b: структурные команды (добавить empty/composite/mesh/actor, drag &
  drop из Content Browser, delete/duplicate/reparent), Details узла,
  Outliner следует draft'у.
