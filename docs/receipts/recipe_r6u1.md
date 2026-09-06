# R6-U1 (Recipe Model v2.1) — Save Unique, procedural variant

Статус: **REVIEW** (близнец). Из сессии Edit Contents (R6-D0…D2) draft
вложенного определения сохраняется как новое уникальное определение с явно
выбранной областью действия.

## 1. Что сделано

- `EMHCompositeUniqueScope {InParentDefinition, ForThisPlacement}`,
  `FMHCompositeSaveUniquePlan {Copies, OverwrittenDefinition, Warnings}`,
  `UMHCompositeLevelSubsystem::DescribeSaveUnique` / `SaveEditAsUnique`.
- Цепочка вызова разбирается из `EditingInvocationPath`
  (`root:nodes[k]>child:nodes[i]/children[j]…`): каждый сегмент — определение
  и слот (селектор узла/опции), который вызывает следующее. Слоты и документы
  цепочки проверяются до границы источника; цели: канонические имена, не из
  цепочки, без дублей, число = числу копий.
- «Make Child Unique in This Definition»: копия редактируемого определения
  (документ draft'а) → вызывающее определение перенаправляется на копию и
  публикуется через `PublishDefinition` (общий путь с R6-D2, уведомление →
  все его размещения следуют); исходное дочернее определение не тронуто.
- «Make Unique for This Placement»: копии innermost-first — редактируемое
  определение, затем каждое определение цепочки с перенаправленным слотом, до
  root; только это размещение переключается на новый root
  (`SetCompositeAsset`); если `CallContext` размещения пуст, он получает
  `StreamNamespace`/`AppearanceBoundary` = имя исходного root — потоки уровня
  root не перебрасываются (второй явный писатель `CallContext` после Break;
  docs/16 §2.10).
- Random внутри копий под новым именем перебрасывается (потоки ключуются
  путём узла): `DescribeSaveUnique` предупреждает про каждую такую копию
  (`MHCompositeDocumentHasRandomization`: random-узлы/опции, inline placement,
  профиль); root-копия с сохранённым контекстом не предупреждает. Bake
  Current Result — R6-U2.
- `CreateManagedComposite` — вынесенный из Build путь создания управляемого
  композита (валидация цели, уникальность имени в source root, claims,
  зависимости, probe, пакет, `MHPublishCompositeV5` с adopt-целью, импорт);
  Build использует его же. Seam `SetDefinitionCreatorForTests`.
- UI: Composite Actions и Composite Outliner при вложенной сессии — «Make
  Child Unique in This Definition», «Make Unique for This Placement»; имена
  копий запрашиваются модалом Adopt (папка + каноническое имя, по одному на
  копию, подсказка `<name>_unique`); для первой области — подтверждение
  перезаписи вызывающего определения; предупреждения о re-roll — в
  Message Log.

## 2. Тесты (red `eb132b5`)

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Composite.EditContext.MakeUniqueForPlacementSwitchesOnlyThisPlacement` | план: копии [child, root], без перезаписи, без предупреждений; save: создано 2 определения innermost-first (seam), A вызывает root-копию, root-копия вызывает child-копию в том же слоте, child-копия несёт отредактированный узел, `CallContext.StreamNamespace` A = исходный root, общие child/root не изменились, A рендерит +100, B прежний и на общем root |
| `…EditContext.MakeUniqueInDefinitionRewiresSharedParent` | план: копия [child], перезапись root; save: публикуется именно root (seam), root вызывает child-копию, исходный child не тронут, A остаётся на root, контекст пуст, A и B рендерят +100 |
| `…EditContext.SaveUniqueValidatesTargetsAndWarnsOnRandom` | `MHCompositeDocumentHasRandomization` (plain/false, random/true, вложенный профиль/true); без сессии describe отказывает; неверное число целей, неканоническое имя (`MH_E_NONCANONICAL_RESOURCE_NAME`), имя из цепочки, дубли — отказ до создания чего-либо, сессия жива |

## 3. Гейты

| Gate | Результат |
|---|---|
| RED (`eb132b5`) | `R6U1_RED_TEST2.log`: Fail ×3 (три новых теста; D0/D1b/D2 — Success) |
| GREEN non-unity/no-PCH build | `R6U1_GREEN_BUILD.log`: Succeeded |
| `Mimir.V5.Composite.EditContext` | `R6U1_GREEN_TEST.log`: 7/0 |
| полный NullRHI suite | `R6U1_GREEN_FULL.log`: `Success=228 Fail=0 (225 + 3)` |
| force-unity | `R6U1_FORCE_UNITY.log`: Succeeded |
| `BuildPlugin -StrictIncludes` | `R6U1_STRICT.log`: ExitCode=0 (Success) |
| `git diff --check`, `check_normative_docs.py` | чисто / OK |

## 4. Изменённые файлы

Editor: `Public|Private/Composite/MHCompositeLevelSubsystem.{h,cpp}`,
`Public/Composite/MHCompositeActor.h` (комментарий `CallContext`),
`Private/UI/MHSourceToolMenus.{h,cpp}`, `Private/UI/MHCompositeOutliner.cpp`.
Tests: `MHCompositeEditContextTest.cpp`. Docs: `docs/16_recipe_model.md`
(§2.7 разбивка R6-U1/U2, §2.10 писатели `CallContext`),
`docs/RECIPE_EXECUTION_STATUS.md`, эта квитанция.

## 5. Вопросы

Открытых нет. Настоящий путь создания (`CreateManagedComposite`) общий с
Build и, как и Build, проверяется в поле (в автоматике — seam). Следующий —
R6-U2 (Bake Current Result), затем R6-O (опц.) / R7-0.
