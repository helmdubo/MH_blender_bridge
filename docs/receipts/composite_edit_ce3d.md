# CE-3d (Composite Edit Mode) — переход между определениями, breadcrumbs, вход «рамкой»

Статус: **REVIEW** (близнец). Завершает карточку CE-3 спецификации
(`docs/reference_notes/MH_Composite_Edit_Mode_Spec_19b7515.md` §5.2, A04,
контракт «Навигация» в `docs/contracts/composite_edit_ce0.md`). Дальше —
CE-4a (event-driven transforms, Undo внутри сессии).

## 1. Что сделано

- Одна writable-сессия (как `ULevelInstanceSubsystem::EditLevelInstanceInternal`):
  `UMHCompositeEditorMode::RequestSwitch(path)` разрешает текущую сессию и
  открывает целевое определение того же размещения (пустой путь — корень).
  Чистая сессия закрывается молча; dirty — вопрос «Save changes to <edited>
  before editing <target>?» **Yes** (interactive commit с обычным
  подтверждением перезаписи; отклонённая/неудачная публикация оставляет
  сессию) / **No** (discard) / **Cancel** (остаться). Переход в текущую
  область — no-op. Test-hook `SetSwitchConfirmForTests`.
- Breadcrumbs панели: `Composite Edit | root > child > current*` —
  `BreadcrumbTargets(context)` даёт пары (label, invocation path) от корня
  до текущей области; все crumb'ы, кроме последнего, — кнопки
  (`SimpleButton`), переход отложен на следующий тик (switch закрывает
  toolkit, из которого пришёл клик). Цепочка фиксирована на время сессии:
  switch перезаходит в режим и пересобирает overlay; текущий crumb
  динамический (`*` при dirty).
- Точки входа: Composite Outliner при открытой сессии под флагом показывает
  «Edit Contents...» на любой composite-ссылке, кроме текущей области
  (→ `RequestSwitch` + фокус строки); «Edit MH Composite» в Composite Actions
  при открытой сессии на этом размещении → `RequestSwitch("")`.
- Вход «рамкой» (запрос owner 2026-09-06): `Enter` режима делает
  projection-актор единственным выделением (outline движка обводит все
  спроецированные узлы, компонентов не выбрано); строка Outliner или клик
  затем хватают узел. `FocusEditScope` под флагом больше не хватает первый
  узел сам.
- `DeactivateForSession` теперь `FEditorModeTools::DestroyMode` вместо
  `DeactivateMode`: движок откладывает `Exit` до своего тика
  (`PendingDeactivateModes`), и сессия, открытая в том же тике (switch,
  Cancel+Begin в тесте), «возобновляла» ожидающий режим без `Enter` — без
  рамки и без overlay (первый green-прогон `CE3D_GREEN_TEST.log`: 22/1 на
  корневом входе). `DestroyMode` делает `Exit` сразу; следующая сессия
  получает свежий объект режима (`CreateEditorModeWithToolsOwner`).
- Pivot projection-актора = трансформ occurrence (для вложенной сессии —
  мировой трансформ invocation-узла, для корня — размещения): gizmo
  выделенной «рамки» стоит на самом подкомпозите, а не в центре родителя
  (жалоба owner на скриншоте). Компоненты absolute — pivot геометрии не
  несёт; перемещение всего актора draft пока не меняет (CE-4a решает:
  запрет или сдвиг всех top-level узлов).

## 2. Тесты (red `86e1561`)

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Composite.EditMode.Navigation.SwitchAsksThenOpensTheTarget` | crumbs для вложенной сессии: (root, "") и (child, path); switch в ту же область — no-op без смены epoch; чистый switch на другой вызов без вопроса, режим активен; dirty + Cancel → остались, dirty сохранён; dirty + No → корень открыт, child не тронут; dirty root + Yes → одно подтверждение перезаписи, опубликован root, открыт второй вызов, root несёт правку |
| `…Navigation.EnterSelectsTheOccurrenceAndPivotsThere` | после Begin вложенной: projection-актор выделен эксклюзивно, компонентов нет, location актора = мировая точка invocation-узла; корневая сессия: выделен, pivot = трансформ размещения |

## 3. Гейты

| Gate | Результат |
|---|---|
| RED (`86e1561`) | `CE3D_RED_TEST.log`: Fail ×2 (`CE3D_RED_TEST2.log`; первый прогон не применил red-патч — устаревший якорь `struct HHitProxy`) |
| GREEN non-unity/no-PCH build | `CE3D_GREEN_BUILD.log`: Succeeded |
| `Mimir.V5.Composite.EditMode` | `CE3D_GREEN_TEST2.log`: 23/0 (первый прогон `CE3D_GREEN_TEST.log`: 22/1 — resume режима без `Enter`, см. §1) |
| полный NullRHI suite | `CE3D_FULL.log`: `Success=255 Fail=0 (253 + 2)` |
| force-unity | `CE3D_FORCE_UNITY.log`: Succeeded |
| `BuildPlugin -StrictIncludes` | `CE3D_STRICT.log`: ExitCode=0 (Success) |
| `git diff --check`, `check_normative_docs.py` | чисто / OK |

## 4. Изменённые файлы

Editor (5): `Public|Private/Editing/MHCompositeEditorMode.{h,cpp}`,
`Private/Editing/MHCompositeEditProjection.cpp`,
`Private/UI/MHCompositeOutliner.cpp`, `Private/UI/MHSourceToolMenus.cpp`.
Tests: `MHCompositeEditModeNavigationTest.cpp` (новый). Docs:
`docs/RECIPE_EXECUTION_STATUS.md`, эта квитанция.

## 5. Вопросы

- Полевая проверка owner'ом с `bCompositeEditModeV2`: рамка при входе,
  crumb-кнопки, вопрос при переходе с изменениями, pivot на подкомпозите.
- CE-4a: перетаскивание компонента узла gizmo → `SetNodeTransform` draft'а
  (одна транзакция на жест, `InputDelta/EndTracking`), Undo внутри сессии,
  BPP-style `ResetTransaction` на входе/выходе; судьба перетаскивания самого
  projection-актора.
