# R6-UX1 (Recipe Model v2.1) — Edit Contents: захват хэндлов, рамка, дерево

Статус: **REVIEW** (близнец). Полевой отзыв owner 2026-09-06: в сессии Edit
Contents нельзя было сдвинуть ни один узел подкомпозита (gizmo оставался в
центре родителя, выбрать нечего), дерево Outliner схлопывалось до root при
входе в сессию, редактируемый подкомпозит никак не выделялся в сцене.

## 1. Что сделано

- Хэндлы узлов scope — `UBillboardComponent` (спрайт `S_TargetPoint`,
  screen-size, hidden in game): кликабельны во viewport при выделенном
  композит-акторе, gizmo двигает узел; `SyncEditScopeHandles` ведёт
  параллельный массив путей `EditScopeHandlePaths`.
- `AMHCompositeActor::FindSessionHandleForNodePath(path)`: для scope-сессии —
  хэндл верхнего предка узла внутри редактируемого определения (точное
  совпадение или префикс `/`, `>`); для корневой сессии — top-level хэндл
  `<prefix>:nodes[i]` (prefix — имя root или `CallContext.StreamNamespace`);
  вне сессии/вне поддерева — null.
- Рамка: `GetEditScopeBounds()` (позиции хэндлов + мировые AABB листьев с
  `NodePath` под `<invocation>><def>:` — инстансы пула через
  `GetInstanceTransform` × AABB меша, собственные компоненты через `Bounds`) →
  `UBoxComponent` `MH_ScopeFrame` (wireframe, оранжевый, `bSelectable=false`,
  без коллизии, hidden in game), пересчитывается после каждой материализации
  сессии, уничтожается вместе с хэндлами.
- Composite Outliner: клик по любой строке редактируемого поддерева в сессии
  захватывает её хэндл (`SelectHandle`: выделение актора + компонента), строки
  вне поддерева не меняют выделение; `RefreshModel` сохраняет раскрытие дерева
  (пути раскрытых строк, родители раскрываются первыми, ленивые composite-строки
  подгружаются); Edit Contents раскрывает и показывает редактируемый
  подкомпозит и захватывает его первый хэндл (`FocusEditScope`); строка
  вызова подсвечена оранжевым, строки вне scope приглушены; статусная строка
  объясняет захват.

## 2. Тесты (red `a0fd1f4`)

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Composite.EditContext.ScopeHandlesAreGrabbableAndFramed` | вне сессии — нет хэндла и рамки; в nested-сессии хэндл — `UBillboardComponent`, путь узла ребёнка и его потомка → тот же хэндл, путь вне scope и сам вызов → null; `GetEditScopeBounds` валиден и содержит mesh C; рамка — компонент актора, `bSelectable=false`, extent/центр = bounds; Cancel убирает рамку и хэндлы; корневая сессия: `root:nodes[0]` → top-level[0], путь вложенного листа → top-level[1], рамки нет |

Дерево/подсветка Outliner — Slate, проверяется в поле.

## 3. Гейты

| Gate | Результат |
|---|---|
| RED (`a0fd1f4`) | `R6UX1_RED_TEST.log`: Fail |
| GREEN non-unity/no-PCH build | `R6UX1_GREEN_BUILD.log`: Succeeded |
| `Mimir.V5.Composite.EditContext` | `R6UX1_GREEN_TEST.log`: 9/0 |
| полный NullRHI suite | `R6UX1_GREEN_FULL.log`: `Success=230 Fail=0` (229 + 1) |
| force-unity | `R6UX1_FORCE_UNITY.log`: Succeeded |
| `BuildPlugin -StrictIncludes` | `R6UX1_STRICT.log`: ExitCode=0 (Success) |
| `git diff --check`, `check_normative_docs.py` | чисто / OK |

## 4. Изменённые файлы

Editor: `Public|Private/Composite/MHCompositeActor.{h,cpp}`,
`Private/UI/MHCompositeOutliner.cpp`. Tests: `MHCompositeEditContextTest.cpp`.
Docs: `docs/RECIPE_EXECUTION_STATUS.md`, эта квитанция.

## 5. Вопросы

Открытых нет. Следующий — R6-UX2: Esc/Enter в сессии и одна кнопка
«Save As Unique Copy...» с диалогом вместо четырёх пунктов меню.
