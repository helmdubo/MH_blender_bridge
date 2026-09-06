# CE-4b1 (Composite Edit Mode) — структурные команды draft'а

Статус: **REVIEW** (близнец). Первая часть карточки CE-4b спецификации
(`docs/reference_notes/MH_Composite_Edit_Mode_Spec_19b7515.md`, A09) и
решения owner (г): в режиме можно расширять композит новыми узлами. Здесь —
модель и команды (document/session); показ draft'а в Composite Outliner с
контекстным меню и drag & drop — CE-4b2, Details узла, random-опции/веса и
placement-профиль — CE-4b3.

## 1. Что сделано

- `UMHCompositeEditDocument` — структурные команды над pre-order массивом
  reflected-узлов с session-id: `AddNode(parent, kind, resource, name,
  local)` (последний ребёнок родителя или новый корень), `DeleteNode`
  (с поддеревом), `DuplicateNode` (копия поддерева сразу после оригинала под
  тем же родителем, свежие id), `ReparentNode(id, parent, siblingIndex)`
  (перенос поддерева; тот же родитель = reorder), запросы `GetParentId`,
  `GetChildIds`. Все команды: сначала валидация, потом `Modify()` — Undo
  восстанавливает порядок, id и метаданные одним махом (reflected массивы
  в записи, CE-1).
- Валидация по грамматике протокола (те же правила, что в canonical
  writer): детей принимает только group; mesh/actor/composite/gameobj
  требуют канонический ресурс `[a-z0-9_]+`, group ресурс запрещён; random
  здесь не создаётся (нужны опции — CE-4b3); цикл (родитель внутри
  поддерева) и перенос под лист отклоняются до изменений — ревизия и dirty
  не меняются.
- Инварианты pre-order: `SubtreeEnd` (поддерево — непрерывный диапазон,
  чьи родители лежат внутри), `ExtractBlock`/`InsertBlock` с относительными
  родителями и сдвигом индексов родителей у последующих узлов; селекторы
  (`nodes[i]/children[j]`) остаются производными от массива, id — ключ
  идентичности (Outliner/проекция/Details смотрят по id).
- `UMHCompositeEditSession` — обёртки с обновлением проекции после
  команды; `ReparentNode(..., bKeepWorld)`: мировой трансформ узла берётся
  с его projection-компонента, новая локаль — относительно компонента
  нового родителя или occurrence (pivot projection-актора для нового
  корня). Проекция: новый mesh-узел — новый SMC с тем же мешем, удалённый —
  компонент retired (identity по origin, CE-2b).

## 2. Тесты (red `682948a`)

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Composite.EditMode.Structure.AddDeleteDuplicateReparentKeepIdsAndOrder` | add mesh под группу → `nodes[1]/children[1]`, parent, random-узел сохранил id (индекс 4), спроецирован как SMC и → новый id; delete grouped → 4 узла, id пропал, added стал `children[0]`, второй компонент retired; duplicate группы → 6 узлов, копия `nodes[2]`, random → `nodes[3]`, у копии один ребёнок со свежим id, копия спроецирована, оригиналы с прежними id; reparent plain под копию с keep-world → `nodes[1]/children[0]`, мировая точка та же, локаль изменилась; Undo ×4 → 4 узла, id в исходном порядке, grouped назад, added исчез из draft'а и проекции, clean; Redo ×4 → 6 узлов, plain под копией |
| `…Structure.CommandsRefuseInvalidStructure` | ребёнок под mesh, mesh без ресурса, group с ресурсом, random без опций, неизвестный родитель, цикл, перенос под лист, под себя, delete/duplicate неизвестного — всё отклонено, ревизия/число узлов/clean не изменились; reorder (тот же родитель, index 0) — группа первая, её ребёнок следом, plain третий, dirty |

## 3. Гейты

| Gate | Результат |
|---|---|
| RED (`682948a`) | `CE4B1_RED_TEST.log`: Fail ×2 |
| GREEN non-unity/no-PCH build | `CE4B1_GREEN_BUILD.log`: Succeeded |
| `Mimir.V5.Composite.EditMode` | `CE4B1_GREEN_TEST.log`: 27/0 |
| полный NullRHI suite | `CE4B1_FULL.log`: `Success=259 Fail=0 (257 + 2)` |
| force-unity | `CE4B1_FORCE_UNITY.log`: Succeeded |
| `BuildPlugin -StrictIncludes` | `CE4B1_STRICT.log`: ExitCode=0 (Success) |
| `git diff --check`, `check_normative_docs.py` | чисто / OK |

## 4. Изменённые файлы

Editor (4): `Public|Private/Editing/MHCompositeEditDocument.{h,cpp}`,
`Public|Private/Editing/MHCompositeEditSession.{h,cpp}`. Tests:
`MHCompositeEditStructureTest.cpp` (новый). Docs:
`docs/RECIPE_EXECUTION_STATUS.md`, эта квитанция.

## 5. Вопросы

- Структурное изменение может «перебросить» path-derived RNG (spec CE-4b):
  ожидания задаёт неизменный резолвер, а не прежняя картинка — dup группы
  с random-узлом внутри получит другой выбор опции; принято.
- Actor/GameObj-узлы добавляются с каноническим ресурсом без проверки
  существования актора/класса — capability-контракт R7-0.
- CE-4b2: Composite Outliner под флагом показывает draft (сейчас — ассет
  + план размещения), контекстное меню Add Empty/Composite/Mesh/Actor,
  Delete, Duplicate, drag & drop из Content Browser (mesh/composite ассет →
  `AddNode`), выделение нового узла.
