# CE-4b2 (Composite Edit Mode) — Composite Outliner показывает draft, drag & drop, меню узлов

Статус: **REVIEW** (близнец). Вторая часть карточки CE-4b спецификации
(«Outliner отображение только через согласованный API») и решение owner
(г): в режиме композит расширяется новыми узлами (empty/composite/mesh/
actor). Details узла, random-опции/веса и placement-профиль — CE-4b3.

## 1. Что сделано

- `FMHCompositeOutlinerModel` под CE-backend: `BuildFromActor` берёт
  открытую сессию этого размещения из подсистемы (`SetEditSession`);
  строки редактируемого occurrence (корень или вложенный вызов — по
  префиксу пути) строятся из draft'а сессии (`BuildNodeRows` над
  `TConstArrayView<FMHCompositeAssetNode>`; `DraftFor(asset, prefix)`),
  каждая такая строка несёт `DraftNodeId`; остальные строки — из ассетов,
  как раньше. Overlay и хэндлы для строк occurrence берутся из плана и
  компонентов проекции (`MergeProjectionOverlay`, вызывается из
  `RefreshOverlay`): узел, добавленный в сессии, не имеет строки размещения —
  только проекцию — и всё равно получает overlay и компонент
  (`FindForComponent` работает для projection-компонентов). Слияние
  повторяется в конце `BuildFromActor`: запечатанное размещение всё ещё
  несёт строки (ISM-бакеты) для редактируемого occurrence и без этого
  перекрывало привязку проекции у вложенных строк (первый green-прогон
  `CE4B2_GREEN_TEST.log`: 28/1).
- Freshness: `FMHCompositeOutlinerFreshness` несёт `DraftSerial` (change
  serial draft'а сессии на этом размещении); любая команда меняет ключ →
  панель перестраивает дерево на своём тике, как при смене seed.
- Описание добавления (`Public/UI/MHCompositeOutlinerEditActions.h`):
  `MHOutlinerAddParentFor(row, draft)` — в группу как ребёнок, рядом с
  любой другой строкой как sibling, в корень без строки;
  `MHDescribeOutlinerAssetAdd(asset, row, draft, request, error)` —
  managed static mesh (по receipt `UMHStaticMeshImportData::LogicalName`) →
  mesh-узел, `UMHCompositeAsset` → composite-узел, остальное отклоняется с
  причиной.
- Панель: drag & drop `FAssetDragDropOp` на строки draft'а (`OnCanAcceptDrop`
  /`OnAcceptDrop`): onto группы — внутрь, above/below любой строки — sibling
  на этом месте (`ReparentNode` после `AddNode`); несколько ассетов — одна
  транзакция «Add Composite Nodes», отклонённые — в Message Log.
  Контекстное меню строк draft'а: **Add Empty Node** (group; подсказка про
  drag & drop для mesh/composite), **Duplicate Node**, **Delete Node**; после
  команды — rebuild и выделение нового узла через режим (gizmo на нём).
  Actor-узлы: только по капабилити-контракту R7-0 (ресурс без проверки
  класса), в панели пока не предлагаются.

## 2. Тесты (red `53137e8`)

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Composite.EditMode.Structure.OutlinerShowsTheDraftAndBindsTheProjection` | до сессии строки ассета без session-id; в сессии строки occurrence несут id draft'а, привязаны к projection-компонентам (`PlacementComponent`, `FindForComponent`), с overlay; строки вне occurrence — ассетные; `AddNode` меняет freshness-ключ, перестроенная модель показывает новую строку `nodes[1]/children[1]` с ресурсом/kind/id, компонентом и overlay, у группы `AuthoredChildCount == 2`; после Cancel строки нет |
| `…Structure.OutlinerDropDescribesTheCommand` | родитель добавления: группа → внутрь, корневой лист → корень, лист в группе → группа, без строки → корень; managed mesh → mesh-узел с ресурсом под группой, composite-ассет → composite-узел в корне; чужой объект/null — отказ |

## 3. Гейты

| Gate | Результат |
|---|---|
| RED (`53137e8`) | `CE4B2_RED_TEST.log`: Fail ×2 |
| GREEN non-unity/no-PCH build | `CE4B2_GREEN_BUILD.log`: Succeeded |
| `Mimir.V5.Composite.EditMode` | `CE4B2_GREEN_TEST2.log`: 29/0 (первый прогон `CE4B2_GREEN_TEST.log`: 28/1, см. §1) |
| полный NullRHI suite | `CE4B2_FULL.log`: `Success=261 Fail=0 (259 + 2)` |
| force-unity | `CE4B2_FORCE_UNITY.log`: Succeeded |
| `BuildPlugin -StrictIncludes` | `CE4B2_STRICT.log`: ExitCode=0 (Success) |
| `git diff --check`, `check_normative_docs.py` | чисто / OK |

## 4. Изменённые файлы

Editor (5): `Public|Private/UI/MHCompositeOutlinerModel.{h,cpp}`,
`Private/UI/MHCompositeOutliner.cpp`,
`Public|Private/UI/MHCompositeOutlinerEditActions.{h,cpp}` (новые). Tests:
`MHCompositeOutlinerDraftTest.cpp` (новый). Docs:
`docs/RECIPE_EXECUTION_STATUS.md`, эта квитанция.

## 5. Вопросы

- Полевая проверка owner'ом: правый клик по строке в сессии → Add Empty /
  Duplicate / Delete; перетащить managed mesh или composite из Content
  Browser на строку; Ctrl+Z; Save.
- Add Actor / GameObj из панели — после R7-0 (какой класс/ресурс считается
  managed).
- CE-4b3: Details узла (имя, ресурс, трансформ), random-узел (опции, веса,
  добавление через меню), placement-профиль.
