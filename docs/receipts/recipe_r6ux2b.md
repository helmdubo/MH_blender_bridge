# R6-UX2b (Recipe Model v2.1) — одна кнопка «Save As Unique Copy...»

Статус: **REVIEW** (близнец). Полевой отзыв owner 2026-09-06: «зачем мне
столько вариантов?» — четыре пункта Make Unique (две области × bake) в двух
меню заменены одной кнопкой с объясняющим диалогом.

## 1. Что сделано

- `PromptSaveUniqueOptions` (MHSourceToolMenus.cpp): модальный диалог «Save As
  Unique Copy» — две radio-кнопки области, описанные в терминах этого
  размещения («In this definition: `<parent>` will invoke the copy instead of
  `<edited>`. All N placement(s) of `<parent>` follow.» / «For this placement
  only: `<root>` and the chain down to `<edited>` are copied; only this actor
  switches to the new root…»), чекбокс «Bake current result» с объяснением
  (draws frozen / re-roll under the new name), подсказка о следующем шаге
  (имена копий, немедленная запись файлов), Cancel / Continue. Далее — прежний
  путь `ExecuteSaveUnique(scope, variant)` (Adopt-модал на каждую копию,
  подтверждение перезаписи для области «в этом определении»).
- `MHExecuteSaveUniqueCopyInteractive()`; Composite Actions и Composite Outliner
  при вложенной сессии: `Apply Shared Definition`, `Save As Unique Copy...`,
  `Cancel Edit Contents (Esc)` — три пункта вместо шести.
- Статусная строка Outliner: «right-click for Apply Shared Definition, Save As
  Unique Copy, Cancel Edit Contents».

## 2. Тесты

UI-only срез (Slate-диалог и пункты меню); логика областей/bake и её тесты —
R6-U1/U2 без изменений (`Mimir.V5.Composite.EditContext` 10/0). Red-фазы нет —
проверяется в поле.

## 3. Гейты

| Gate | Результат |
|---|---|
| non-unity/no-PCH build | `R6UX2B_BUILD.log`: __ |
| `Mimir.V5.Composite.EditContext` | `R6UX2B_TEST.log`: __ |
| полный NullRHI suite | `R6UX2B_FULL.log`: __ |
| force-unity | `R6UX2B_FORCE_UNITY.log`: __ |
| `BuildPlugin -StrictIncludes` | `R6UX2B_STRICT.log`: __ |
| `git diff --check`, `check_normative_docs.py` | __ |

## 4. Изменённые файлы

Editor: `Private/UI/MHSourceToolMenus.{h,cpp}`, `Private/UI/MHCompositeOutliner.cpp`.
Docs: `docs/RECIPE_EXECUTION_STATUS.md`, эта квитанция.

## 5. Вопросы

Открытых нет. Семья UX по отзыву owner закрыта (UX1, UX2a, UX2b); дальше —
полевая проверка owner.
