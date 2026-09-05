# R5b-2a (Recipe Model v2.1) — подсветка инстансов owner'а и bounds размещения

Статус: **REVIEW** (близнец). Первая половина R5b-2 (selection-seam
вьюпорта): визуальная обратная связь и bounds. Вторая половина (R5b-2b) —
адаптер выбора typed elements (клик по инстансу пула → owner-композит) и его
регистрация в редакторе.

## 1. Что сделано

- **Подсветка только owner'а.** `UMHInstancePoolSubsystem::SetOwnerSelected
  (Owner, bSelected)` помечает выбранными (`SelectInstance`) только живые
  инстансы этого owner'а на общем ISM; `IsOwnerSelected`. Состояние живёт в
  пуле (`SelectedOwners`) и переигрывается при `AddInstanceToComponent` —
  поэтому переживает Hide/Show и миграцию бакета. Пул сам зеркалит
  редакторское выделение акторов: подписка на `USelection::SelectObjectEvent`
  и `SelectionChangedEvent` в `Initialize`, снятие в `Deinitialize`.
  Штатное «выделен компонент → подсвечен весь ISM» для пула непригодно —
  бакет общий для всех owner'ов.
- **Bounds размещения.** `GetOwnerBounds(Owner)` — world-bounds меша под
  матрицей каждого живого инстанса; `AMHCompositeActor::
  GetComponentsBoundingBox` добавляет их к собственным компонентам, так что
  `F`/focus и любые bounds-операции редактора видят всё размещение, хотя
  пуловые листья — не компоненты актора.

## 2. Тесты (red `d75e9d8`)

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Composite.Pool.OwnerSelectionHighlightsOnlyOwner` | `SetOwnerSelected(A)` подсвечивает инстансы A и не B на одном бакете; подсветка переживает Hide/Show и `ReconcileMesh` (descriptor); переключение на B; `GetOwnerBounds(A)` покрывает инстансы A и не B, после `RemoveOwner` — невалиден |
| `Mimir.V5.Composite.Pool.SelectedPlacementHighlightsAndBounds` | два размещения: `GetComponentsBoundingBox` актора без собственных примитивов валиден, покрывает все листья A и не B; `GEditor->SelectActor(A)` подсвечивает только инстансы A, перенос выделения на B, `SelectNone` снимает всё |

## 3. Гейты

| Gate | Результат |
|---|---|
| RED (`d75e9d8`) | `R5B2A_RED_TEST.log`: два теста Fail |
| GREEN non-unity/no-PCH build | `R5B2A_GREEN_BUILD3.log`: Succeeded |
| `Mimir.V5.Composite.Pool` | `R5B2A_GREEN_TEST3.log`: 13/0 |
| полный NullRHI suite | `R5B2A_GREEN_FULL.log`: `Success=218 Fail=0` (216 + 2) |
| force-unity | `R5B2A_FORCE_UNITY.log`: Succeeded |
| `BuildPlugin -StrictIncludes` | `R5B2A_STRICT.log`: ExitCode=0 (Success) |
| `git diff --check`, `check_normative_docs.py` | чисто / OK |

## 4. Изменённые файлы

Editor: `Public|Private/Composite/MHInstancePool.{h,cpp}`,
`Public|Private/Composite/MHCompositeActor.{h,cpp}`. Tests:
`MHInstancePoolTest.cpp`, `MHCompositePoolMaterializationTest.cpp`. Docs:
`docs/RECIPE_EXECUTION_STATUS.md`, эта квитанция.

## 5. Вопросы

Открытых нет. Полевая проверка owner (после R5b-2b): выделение композита в
Level Outliner подсвечивает только его инстансы; `F` кадрирует размещение.
