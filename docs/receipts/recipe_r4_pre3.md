# R4-pre-3 (Recipe Model v2.1) — контекст вызова: Break вложенного композита воспроизводит результат

Статус: **MERGED** (близнец; единственная правка resolver в срезе — по матрице
делегирования только близнец). Закрывает OPEN-R4P-1 по решению owner
2026-09-04 («вариант близнеца») после аудита
`docs/reference_notes/dagor_composit_ue5_audit_20260904.md` §7.A.

## 1. Дефект

Потоки resolver'а ключуются `Seed + hash(NodePath)`, appearance — boundary
path. Ребёнок-композит, вынесенный Break'ом в самостоятельный актор, резолвил
`child:nodes[i]` вместо `<root>:nodes[k]>child:nodes[i]` и получал другие
draws: при равных весах родитель выбирал `mesh_b`, ребёнок — `mesh_a`;
appearance-каналы тоже расходились. Red-коммит `5980ce3` воспроизводит ровно
пример аудита (seed 0).

## 2. Решение

- Runtime resolver: `FMHResolveCallContext { NodePathPrefix; AppearanceBoundaryPath }`
  и перегрузки `MHResolveCompositeLayout`/`MHResolveCompositePlan` с
  контекстом. Пустой контекст = имя корневого композита — это и есть прежний
  путь; контекст-free перегрузки делегируют с пустым контекстом. Обход,
  draws и математика не изменены; `RecipeShadowParity`,
  `StreamTraceAndSignatureParity` и golden-тесты зелёные.
- Reference-резолвер `tools/mh_random_reference.py`: `resolve_composite(...,
  node_path_prefix=, appearance_boundary=)`; тест
  `test_call_context_reproduces_parent_subtree` (44 passed).
- Актор: персистентный `FMHCompositeCallContext CallContext { Version=1,
  StreamNamespace, AppearanceBoundary }` (docs/16 §2.10), `Get/SetCallContext`;
  preview (`MHMaterializeLayout` с контекстом), edit-сессия
  (`MHResolvePreviewGraph` с контекстом), proof (`BuildProofNow`) и
  runtime-транспорт (`FMHRuntimeCompositeInput::CallContextNodePathPrefix/
  CallContextAppearanceBoundary`, `MHRuntimeInputCallContext`, runtime-актор)
  резолвят через один и тот же контекст — ни одна плоскость не расходится.
- Break: `FMHBreakSpawnSpec::CallContext` — `StreamNamespace` = NodePath узла
  родителя (для random — `…/options[j]`) + `>` + ресурс; `AppearanceBoundary`
  = boundary первого листа поддерева в родителе (иначе имя корня);
  контекст ставится **до** `SetCompositeAsset`.

## 3. Тесты

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Random.CallContextReproducesSubtree` | resolver: child с контекстом == поддерево родителя (решения, raw draw, appearance, world через узел родителя) для seed 0 и 17; без контекста — прежний namespace |
| `Mimir.V5.Composite.Break.NestedCompositeReproducesResult` | Break: ребёнок получает контекст, его preview-, proof-план и runtime input воспроизводят лист родителя (ресурс, world-матрица, 4 канала appearance) |
| `tests/test_random_reference.py::test_call_context_reproduces_parent_subtree` | reference-резолвер, числа аудита (seed 0: `mesh_b` в родителе, `mesh_a` самостоятельно) |

## 4. Гейты

| Gate | Результат |
|---|---|
| RED (`5980ce3`) | `R4PRE3_RED_TEST.log`: оба теста Fail на namespace/boundary/ресурсе/каналах |
| GREEN non-unity/no-PCH build | `R4PRE3_GREEN_BUILD.log`: Succeeded |
| полный NullRHI suite | `R4PRE3_GREEN_FULL.log`: `Success=201 Fail=0` (199 + 2) |
| `pytest tests/test_random_reference.py tests/test_appearance_reference.py` | 44 passed |
| `BuildPlugin -StrictIncludes` | `R4PRE3_STRICT.log`: ExitCode=0 (Success) |
| force-unity | `R4PRE3_FORCE_UNITY.log`: Succeeded |
| `git diff --check`, `check_normative_docs.py` | чисто / OK |

## 5. Изменённые файлы

Runtime: `Public|Private/Random/MHRandomStream.{h,cpp}`,
`Public|Private/Composite/MHRuntimeCompositeInput.{h,cpp}`,
`Private/Composite/MHRuntimeCompositeActor.cpp`. Editor:
`Public|Private/Composite/MHCompiledRecipe.{h,cpp}`,
`Public|Private/Composite/MHMaterializeLayout.{h,cpp}`,
`Public|Private/Composite/MHCompositeActor.{h,cpp}`,
`Private/Composite/MHProofCache.cpp`,
`Private/Composite/MHCompositeRuntimeBridge.cpp`,
`Private/Composite/MHCompositeLevelSubsystem.cpp`. Tests:
`MHCallContextTest.cpp` (новый). Docs: `docs/16_recipe_model.md` §2.10, §9;
`tools/mh_random_reference.py`, `tests/test_random_reference.py`;
`docs/RECIPE_EXECUTION_STATUS.md`.

## 6. Вопросы

Открытых нет. Вне среза: Build не заполняет контекст (ребёнок, собранный
Build'ом в новый родитель, по-прежнему перебрасывает варианты — Build
предупреждает об этом с R4-pre-2, решение owner «предупреждать, не
отказывать»); пользовательская очистка контекста в UI не предусмотрена
(свойство только для чтения в Details).
