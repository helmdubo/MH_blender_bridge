# R6-U2 (Recipe Model v2.1) — Save Unique: Bake Current Result

Статус: **REVIEW** (близнец). Второй вариант Save Unique: копия редактируемого
определения — запечённый резидентный результат этого размещения.

## 1. Что сделано

- `EMHCompositeUniqueVariant {Procedural, BakeCurrentResult}` — второй
  параметр `DescribeSaveUnique` / `SaveEditAsUnique` (R6-U1 = Procedural).
- Bake (`BakeScopeDocument`): по резидентному плану сессии (draft, после
  `Tick(0)`) все листья (`Plan.Leaves`) с `Origin` под
  `<invocation>><definition>:` и видом Mesh/Actor → плоский документ из
  конкретных узлов Mesh/Actor: `Resource` листа, `Name` = DisplayName,
  transform = `Leaf.WorldMatrix × inverse(Invocation.WorldMatrix)` (проверка
  представимости `MHIsRepresentableTransformMatrix`, shear → отказ). Random,
  группы, вложенные композиты, inline placement и профили в копии
  отсутствуют — перебрасываться нечему; порядок узлов = порядок листьев
  плана. Пустой результат (нет Mesh/Actor листьев) — отказ до записи.
- Appearance: потоки ключуются boundary-путём (§3); под boundary корня
  (обычный случай) запечённые листья получают те же каналы. Если внутри
  запекаемого определения объявлен свой boundary (`bAppearanceSeedBoundary`),
  его путь меняется вместе с именем копии — `DescribeSaveUnique` / save
  предупреждают явно.
- `DescribeSaveUnique(Bake)`: для копии редактируемого определения re-roll
  предупреждения нет; копии цепочки (ForThisPlacement) — как в R6-U1.
- UI: Composite Actions и Composite Outliner при вложенной сессии — «Make
  Child Unique in This Definition (Bake Current Result)» и «Make Unique for
  This Placement (Bake Current Result)» рядом с procedural-вариантами.

## 2. Тесты (red `__RED__`)

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Composite.EditContext.BakeCurrentResultKeepsResolvedLeaves` | root_rnd `[mesh A][composite child_rnd(random{C,A} @ z=40, group(10,0,0){mesh C @ z=5})]` на отдельном размещении P: procedural-план предупреждает о random внутри child_rnd, bake-план — нет; save (ForThisPlacement, Bake): child-копия = по одному plain Mesh-узлу на резидентный лист под вызовом (ресурс выбранной опции и mesh C, transform = мир листа × inverse(мир вызова)), без Random/Options/Children/профилей; P на root-копии, `CallContext` = исходный root; тот же набор мировых позиций листьев до и после; A/B на общем root не тронуты |
| R6-U1 тесты | переведены на `EMHCompositeUniqueVariant::Procedural`, поведение прежнее |

## 3. Гейты

| Gate | Результат |
|---|---|
| RED (`__RED__`) | `R6U2_RED_TEST.log`: __ |
| GREEN non-unity/no-PCH build | `R6U2_GREEN_BUILD.log`: __ |
| `Mimir.V5.Composite.EditContext` | `R6U2_GREEN_TEST.log`: __ |
| полный NullRHI suite | `R6U2_GREEN_FULL.log`: __ |
| force-unity | `R6U2_FORCE_UNITY.log`: __ |
| `BuildPlugin -StrictIncludes` | `R6U2_STRICT.log`: __ |
| `git diff --check`, `check_normative_docs.py` | __ |

## 4. Изменённые файлы

Editor: `Public|Private/Composite/MHCompositeLevelSubsystem.{h,cpp}`,
`Private/UI/MHSourceToolMenus.{h,cpp}`, `Private/UI/MHCompositeOutliner.cpp`.
Tests: `MHCompositeEditContextTest.cpp` (fixture `Spawn(Transform, Asset)`,
helper `AllLeafWorldLocations`). Docs: `docs/RECIPE_EXECUTION_STATUS.md`, эта
квитанция.

## 5. Вопросы

Открытых нет. Семья R6-D/R6-U закрыта; R6-O (persistent instance overrides)
опционален и последний; далее R7-0. Полевая проверка owner — все срезы R6.
