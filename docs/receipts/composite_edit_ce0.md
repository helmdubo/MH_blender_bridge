# CE-0 (Composite Edit Mode) — контракт API 5.7.4 и характеризационный fixture

Статус: **REVIEW** (близнец). Карточка CE-0 спецификации
`docs/reference_notes/MH_Composite_Edit_Mode_Spec_19b7515.md`.

## 1. Что сделано

- `docs/contracts/composite_edit_ce0.md` — решения owner 2026-09-06 и таблица
  верифицированного на локальной UE 5.7.4 API с file+line (в PR CE-pre #138,
  дополнено здесь результатами fixture).
- `MimirCompositeTests/Private/MHCompositeEditFixture.h` — эталонный fixture
  `FCompositeEditFixture`: root размещён дважды (A без поворота, B yaw 45),
  вызывает child дважды (yaw 90 и без поворота); child = mesh C, inline group
  с mesh C, random {mesh A | empty}; чужой ISM того же меша с двумя инстансами.
  Помощники: `Invocation(actor, i)`, `LeafWorldsUnder(actor, prefix)`,
  `ForeignWorlds()`, `SameLocations`.
- `MHCompositeEditModeCharacterizationTest.cpp` — четыре теста, фиксирующие
  **сегодняшнее** поведение пути, который CE заменяет (с пометкой, какой срез
  их переворачивает).

## 2. Тесты (характеризация, без red-фазы: они зелёные по определению)

| Тест | Что фиксирует | Переворачивает |
|---|---|---|
| `Mimir.V5.Composite.EditMode.Characterization.FixtureResolvesBothInvocations` | оба размещения рендерят оба вызова child (2–3 листа каждый), чужой ISM держит 2 инстанса | — (инвариант) |
| `…InlineDescendantResolvesToTopAncestorHandle` | grouped mesh и выбранная random-опция резолвятся в хэндл верхнего предка | CE-3 |
| `…DraftPreviewFollowsEveryInvocationInThisPlacement` | правка во втором вызове двигает и первый вызов того же child внутри размещения (+100 Y при yaw 90); другое размещение и чужой ISM не двигаются; Cancel всё возвращает | CE-2 (локальный preview выбранного occurrence, CE-ADR-4); инварианты про B и чужой ISM остаются |
| `…UndoInsideSessionEndsIt` | Undo транзакции с правкой хэндла закрывает сессию и возвращает sealed-размещение (R5-F) | CE-4a |

Queued Apply A → Cancel A → Begin B закрыт срезом CE-pre
(`EditContext.QueuedApplyIgnoresLaterSession`).

## 3. Гейты

| Gate | Результат |
|---|---|
| non-unity/no-PCH build | `CE0_BUILD.log`: Succeeded |
| `Mimir.V5.Composite.EditMode` | `CE0_TEST.log`: 4/0 |
| полный NullRHI suite | `CE0_FULL.log`: __ |
| force-unity | `CE0_FORCE_UNITY.log`: __ |
| `BuildPlugin -StrictIncludes` | `CE0_STRICT.log`: __ |
| `git diff --check`, `check_normative_docs.py` | __ |

## 4. Изменённые файлы

Tests: `MHCompositeEditFixture.h`, `MHCompositeEditModeCharacterizationTest.cpp`
(новые). Docs: `docs/contracts/composite_edit_ce0.md`,
`docs/RECIPE_EXECUTION_STATUS.md`, эта квитанция.

## 5. Вопросы

Визуальная часть spike (сохранение карты, PIE, dimming через
`EditingLevelInstance`) переносится на первый срез с проекцией (CE-2) и
проверяется в редакторе глазами owner; headless это не доказывает.
Следующий — CE-1: `UMHCompositeEditSession` + транзакционный draft.
