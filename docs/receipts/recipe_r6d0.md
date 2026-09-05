# R6-D0 (Recipe Model v2.1) — контекст вложенного редактирования

Статус: **REVIEW** (близнец). Первый срез семьи R6 (docs/16 §2.7, порядок
D0 → D1 → D2 → U → O после аудита 2026-09-05): draft общего определения
подкомпозита открывается через корневое размещение, с адресом вызова и
effective transform родителя как контекстом; источник не трогается; Cancel
сбрасывает draft. Gizmo — R6-D1, публикация и targeted refresh потребителей —
R6-D2.

## 1. Что сделано

- `UMHCompositeLevelSubsystem::BeginEditNestedComposite(Root, InvocationNodePath)`:
  узел резидентного плана корня по NodePath должен быть `Composite`; его
  ресурс → managed-ассет ребёнка через реестр endpoint'ов (identity
  admission); `MHExtractCompositeV5(child)` → draft (`GetEditingDraft()`).
  Сессия: `EditingActor = root`, `EditingAsset = child`,
  `EditingInvocationPath`, `EditingParentWorld = Node.WorldMatrix × basis
  корня`. Никаких хэндлов/извлечений в размещении (R6-D1), никакой записи.
  Отказы: не composite-узел (`MH_E_COMPOSITE_GRAMMAR`), нет плана/ошибка
  размещения, вторая сессия поверх активной, ребёнок без ассета.
- `FMHCompositeEditContext` / `GetEditContext()`: `EditedLogicalName`,
  `EditedSourceRelativePath`, `InvocationPath`, `EffectiveParentWorld`,
  `RootPlacement`, `ConsumerPlacements` (живые размещения, чей recipe-граф
  содержит определение — `DependsOnResource`), `SaveScope =
  SharedDefinition`. Root-сессия заполняет тот же контекст (пустой
  `InvocationPath`, basis корня).
- `CommitEditComposite` для вложенной сессии — явный отказ до R6-D2;
  `CancelEditComposite` перестраивает root только для root-сессии (вложенный
  draft не трогал представление). `GetEditingCompositeLogicalName/
  SourceRelativePath` отвечают за редактируемое определение.
- **UI (Composite Outliner):** контекстное меню строки вложенного композита —
  «Edit Contents...»; в статусной строке при активной сессии:
  `Editing: <child> | Context: <root> -> <invocation path> | Saves: shared
  definition (N placements)`. Ошибки — в Message Log «Mimir».

## 2. Тесты (red `e574e93`)

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Composite.EditContext.NestedInvocationOpensDraft` | root `[mesh A][composite child]` в двух размещениях: mesh-лист отвергается с `MH_E_`; вложенный вызов открывает контекст — child как редактируемое определение и его source path, `InvocationPath`, `RootPlacement`, `EffectiveParentWorld = WorldMatrix узла × basis`, `ConsumerPlacements == 2`, scope = shared, draft = документ ребёнка; открытие не перестраивает и не сдвигает preview корня; вторая сессия отвергается; Cancel: сессии нет, контекст пуст, канонические байты ребёнка не изменились, root не перестроен |

## 3. Гейты

| Gate | Результат |
|---|---|
| RED (`e574e93`) | `R6D0_RED_TEST.log`: Fail |
| GREEN non-unity/no-PCH build | `R6D0_GREEN_BUILD2.log`: Succeeded |
| `Mimir.V5.Composite.EditContext` | `R6D0_GREEN_TEST2.log`: 1/0 |
| полный NullRHI suite | `R6D0_GREEN_FULL.log`: `Success=221 Fail=0` (220 + 1) |
| force-unity | `R6D0_FORCE_UNITY.log`: Succeeded |
| `BuildPlugin -StrictIncludes` | `R6D0_STRICT.log`: ExitCode=0 (Success) |
| `git diff --check`, `check_normative_docs.py` | чисто / OK |

## 4. Изменённые файлы

Editor: `Public|Private/Composite/MHCompositeLevelSubsystem.{h,cpp}`,
`Private/UI/MHCompositeOutliner.cpp`. Tests:
`MHCompositeEditContextTest.cpp` (новый). Docs:
`docs/RECIPE_EXECUTION_STATUS.md`, эта квитанция.

## 5. Вопросы и следующий срез

Открытых нет. **R6-D1**: хэндлы узлов draft под `EffectiveParentWorld`
(`EditedLocal = EditedWorld × inverse(ParentEffectiveWorld)`), transform
admission до `FTransform`, multiselect родитель+потомок — один раз, одна
транзакция на drag, Cancel восстанавливает; transform-only drag без
`RemoveOwner/Add`. Полевой тест owner для D0: правый клик по вложенному
композиту в Composite Outliner → Edit Contents → статусная строка с контекстом
→ Cancel; источник не меняется.
