# R6-D1a (Recipe Model v2.1) — инстансы пула не редактируются напрямую

Статус: **REVIEW** (близнец). Полевой дефект owner 2026-09-06: сдвинутый
gizmo инстанс внутри подкомпозита оставался на месте после Cancel, а сдвиг
родителя «возвращал» его — правка шла мимо модели, штатный менеджер ISM
двигал сырой инстанс. Аудит §4/§5: «отсутствие прямого редактирования stock
ISM instance в обход состояния root».

## 1. Что сделано

- `AMHInstancePoolActor` реализует `ISMInstanceManagerProvider` и
  `ISMInstanceManager` для своих бакетов: `CanEdit/CanMove/CanDelete/
  CanDuplicate = false`, `SetSMInstanceTransform` отказывает, чтение transform
  работает, `NotifySMInstanceSelectionChanged` — no-op (подсветка — owner'а,
  R5b-2a). Инстанс пула — рендер плана размещения, правки идут через
  модель (R6-D1b — хэндлы узлов, R6-D2 — публикация).
- Composite Outliner: клик по строке пулового листа выделяет композит-актор и
  фиксирует лист (`SelectPlacementLeafByNodePath`), а не SM-instance element —
  gizmo больше не получает сырой инстанс.

## 2. Тесты (red `e1cb08e`)

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Composite.Pool.InstancesRejectDirectEditing` | `FSMInstanceManager` инстанса пула: не editable/movable/deletable/duplicable; transform читается; `SetSMInstanceTransform` отказывает и инстанс не сдвинулся; хэндл жив |

## 3. Гейты

| Gate | Результат |
|---|---|
| RED (`e1cb08e`) | `R6D1A_RED_TEST.log`: Fail |
| GREEN non-unity/no-PCH build | `R6D1A_GREEN_BUILD.log`: Succeeded |
| `Mimir.V5.Composite.Pool` | `R6D1A_GREEN_TEST.log`: 14/0 |
| полный NullRHI suite | `R6D1A_GREEN_FULL.log`: `Success=222 Fail=0` (221 + 1) |
| force-unity | `R6D1A_FORCE_UNITY.log`: Succeeded |
| `BuildPlugin -StrictIncludes` | `R6D1A_STRICT.log`: ExitCode=0 (Success) |
| `git diff --check`, `check_normative_docs.py` | чисто / OK |

## 4. Изменённые файлы

Editor: `Public|Private/Composite/MHInstancePool.{h,cpp}`,
`Private/UI/MHCompositeOutliner.cpp`. Tests: `MHInstancePoolTest.cpp`. Docs:
`docs/RECIPE_EXECUTION_STATUS.md`, эта квитанция.

## 5. Вопросы

Открытых нет. Следующий — R6-D1b: хэндлы узлов draft под effective-родителем.
