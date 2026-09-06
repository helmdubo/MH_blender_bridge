# CE-1 (Composite Edit Mode) — единый владелец сессии и транзакционный draft

Статус: **REVIEW** (близнец). Карточка CE-1 спецификации
`docs/reference_notes/MH_Composite_Edit_Mode_Spec_19b7515.md`, контракт
`docs/contracts/composite_edit_ce0.md`.

## 1. Что сделано

- `Editing/MHCompositeEditDocument` — транзакционный draft одного определения.
  Дерево хранится в той же reflected плоской форме, что у управляемого ассета
  (`FMHCompositeAssetNode`, pre-order с parent index) плюс параллельный массив
  session-local `FGuid` узлов и inlined-профили; `Modify()` кладёт весь draft
  в нативную транзакцию, Undo/Redo восстанавливают содержимое, порядок,
  options/empty/нулевые веса, provenance-поля и id. Typed-представление
  (`FMHCompositeDocument`) строится по требованию и кэшируется по change
  serial, который растёт и на `PostEditUndo`. Селекторы v5
  (`nodes[i]/children[j]`) ↔ индекс; опции узлами не являются. Команда v1:
  `SetNodeTransform(id, local)`.
- `Editing/MHCompositeEditSession` — единственный владелец сессии: `SessionId`,
  `Epoch` (= epoch subsystem), состояние (`EditingClean/EditingDirty/Closed`),
  корневое размещение, редактируемый ассет, путь вызова, замороженные
  seed/appearance seed/`CallContext`, неизменный original (документ + canonical
  bytes), draft (`RF_Transactional`, outer = сессия). `IsDirty` — сравнение
  canonical bytes draft'а с original (кэш по change serial). Закрытая сессия
  отказывает командам.
- `UMHCompositeLevelSubsystem` — фасад: `UPROPERTY(Transient)` сильная ссылка
  на сессию, `GetEditSession()`, `OpenEditSession` в обоих Begin (корневом и
  вложенном), `Close` + сброс в `ResetEditSession`. `GetEditingDraft()` читает
  draft сессии; поле `EditingDocument` стало его typed-view (mutable кэш), а
  не вторым writable документом. Мост до CE-4a:
  `SyncDraftFromLegacyEdit` зеркалит правки actor-хэндлов (top-level узлы) в
  draft при чтении.
- `MHCompositeProtocol`: `MHFlattenCompositeDocument` / `MHUnflattenCompositeNodes`
  вынесены из apply/extract ассета и разделены с draft'ом (DECIDED: добавление
  двух функций, существующий API не менялся).

## 2. Тесты (red `197542a`)

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Composite.EditMode.Document.RoundTripKeepsEveryField` | 4 узла pre-order, уникальные id, селекторы ↔ индексы (опция не узел), extract byte-identical образцу с options/empty/нулевым весом/PlaceType/boundary |
| `…Document.TransformCommandIsUndoable` | неизвестный id отклоняется; команда в `FScopedTransaction` двигает узел, растёт revision/serial; Undo возвращает байты, id и revision; Redo повторяет |
| `…Session.SubsystemOwnsOneSession` | до Begin сессии нет; после — открыта/чиста, id, epoch = subsystem, размещение/ассет/путь/замороженный контекст, original = байты ребёнка, draft = original (4 узла); правка legacy-хэндла видна через `GetEditingDraft()` и draft сессии, сессия dirty; после Cancel subsystem без сессии, старая закрыта и отказывает команде («closed»), источник не тронут; корневая сессия — тоже сессия с original = root |
| `…Session.CommandIsUndoableWithoutClosing` | команда сессии в транзакции → dirty; Undo → сессия жива, draft чист |

## 3. Гейты

| Gate | Результат |
|---|---|
| RED (`197542a`) | `CE1_RED_TEST3.log`: Fail (RoundTrip; прогон остановился на первом fail: пустая транзакция stub-команды заставила Undo откатить более раннюю транзакцию сессии; в green оба undo-теста ужесточены `if (!bMoved) return false;`) |
| GREEN non-unity/no-PCH build | `CE1_GREEN_BUILD3.log`: Succeeded |
| `Mimir.V5.Composite.EditMode` | `CE1_GREEN_TEST3.log`: 8/0 |
| полный NullRHI suite | `CE1_FULL2.log`: `Success=240 Fail=0` (236 + 4); первый прогон `CE1_FULL.log` упал на 98-м тесте: повисшая ссылка на путь вызова из заменённого плана в `BeginEditNestedComposite`, исправлено (`41cb02f`) |
| force-unity | `CE1_FORCE_UNITY2.log`: Succeeded |
| `BuildPlugin -StrictIncludes` | `CE1_STRICT2.log`: ExitCode=0 (Success) |
| `git diff --check`, `check_normative_docs.py` | чисто / OK |

## 4. Изменённые файлы

Editor: `Public|Private/Editing/MHCompositeEditDocument.{h,cpp}`,
`Public|Private/Editing/MHCompositeEditSession.{h,cpp}` (новые),
`Public|Private/Composite/MHCompositeLevelSubsystem.{h,cpp}`,
`Public|Private/Composite/MHCompositeProtocol.{h,cpp}`. Tests:
`MHCompositeEditDocumentTest.cpp`, `MHCompositeEditSessionTest.cpp` (новые).
Docs: `docs/RECIPE_EXECUTION_STATUS.md`, эта квитанция.

## 5. Вопросы

`MHCompositeEditCommands.{h,cpp}` из карточки не создан: в v1 команда одна
(`SetNodeTransform`) и живёт в сессии; отдельный файл появится с CE-4b, когда
команд станет несколько. Следующий — CE-2: проекция (компоненты одного
transient projection-актора) и suppression lease в пуле.
