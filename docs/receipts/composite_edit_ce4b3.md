# CE-4b3 (Composite Edit Mode) — имя, ресурс и random-опции узла, редактирование в Details

Статус: **REVIEW** (близнец). Завершает карточку CE-4b спецификации
(«random option/weight editing», «replacement managed resource», A14).
Placement-профиль узла и числовое редактирование трансформа в Details —
не в этом срезе (gizmo покрывает трансформ; профиль — OPEN до CE-5/CE-6).

## 1. Что сделано

- `UMHCompositeEditDocument`: `SetNodeName`, `SetNodeResource` (только
  mesh/actor/composite/gameobj, канонический `[a-z0-9_]+`),
  `AddRandomNode(parent, name, local, options)`, `SetNodeOptions(id,
  options)` (только random), статическая `ValidateOptions` — правила
  canonical writer'а: непустой список, конечные неотрицательные веса и хотя
  бы один положительный, empty-опция без ресурса, остальные — с
  каноническим. Валидация до `Modify()`, Undo восстанавливает.
- `UMHCompositeEditSession` — обёртки с обновлением проекции: смена
  ресурса меняет меш SMC (`ResolveMeshForPreview`), замена опций —
  выбранный лист (`…/options[k]`).
- Composite Outliner (под флагом, строки draft'а): секция **Edit** в
  Details — `Name` и `Resource` (`SEditableTextBox`, commit по Enter/потере
  фокуса), для random — список опций с весами (`SSpinBox`), **Remove**,
  **Add Empty Option**; каждая правка — одна транзакция, выполняется на
  следующем тике (rebuild заменяет виджет, доставивший событие), ошибки
  грамматики — в Message Log; меню **Add Random Node** (одна empty-опция
  w1); drop managed mesh/composite **на random-строку** добавляет опции
  (вес 1), на group — по-прежнему узлы.

## 2. Тесты (red `fc9a9bd`)

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Composite.EditMode.Structure.MetadataAndRandomCommandsFollowTheGrammar` | rename → имя в draft'е; resource → mesh A в draft'е и в проекции; отказы (ресурс у группы, неканонический, неизвестный узел, опции у не-random, пустые опции, нулевые веса, empty с ресурсом, mesh без ресурса, отрицательный вес, random без опций) не меняют ревизию; `AddRandomNode` в корень с двумя опциями → `nodes[4]`, хэндл в проекции; `SetNodeOptions` одной mesh-опцией → в draft'е одна опция, в проекции `options[0]` — mesh A; Undo ×4 → имя/ресурс/проекция прежние, 4 узла, clean |

## 3. Гейты

| Gate | Результат |
|---|---|
| RED (`fc9a9bd`) | `CE4B3_RED_TEST.log`: Fail ×1 (`CE4B3_RED_TEST2.log`; первый red `1d2a221`/`CE4B3_RED_TEST.log` упал в самом тесте на null-узле после отклонённого add — ужесточён в `fc9a9bd`) |
| GREEN non-unity/no-PCH build | `CE4B3_GREEN_BUILD3.log`: Succeeded |
| `Mimir.V5.Composite.EditMode` | `CE4B3_GREEN_TEST3.log`: 30/0 (первый прогон `CE4B3_GREEN_TEST.log`: 29/1 — ошибка ожидания в тесте: новый корень — `nodes[3]`, не `nodes[4]`) |
| полный NullRHI suite | `CE4B3_FULL.log`: `Success=262 Fail=0 (261 + 1)` |
| force-unity | `CE4B3_FORCE_UNITY.log`: Succeeded |
| `BuildPlugin -StrictIncludes` | `CE4B3_STRICT.log`: ExitCode=0 (Success) |
| `git diff --check`, `check_normative_docs.py` | чисто / OK |

## 4. Изменённые файлы

Editor (5): `Public|Private/Editing/MHCompositeEditDocument.{h,cpp}`,
`Public|Private/Editing/MHCompositeEditSession.{h,cpp}`,
`Private/UI/MHCompositeOutliner.cpp`. Tests:
`MHCompositeEditProceduralTest.cpp` (новый). Docs:
`docs/RECIPE_EXECUTION_STATUS.md`, эта квитанция.

## 5. Вопросы

- Полевая проверка owner'ом: Details строки в сессии (имя, ресурс, веса),
  Add Random Node, drop на random-строку, Ctrl+Z, Save.
- Placement-профиль узла (`Profile`/inline placement) не редактируется в
  режиме; числовой трансформ в Details — по запросу owner.
- Дальше CE-5: publish recovery (NoExternalChange / SourceCommitted /
  PartialBatch), draft не теряется при recoverable ошибке публикации.
