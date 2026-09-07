# CE-5b (Composite Edit Mode) — Save As Unique без потери сессии, PartialBatch

Статус: **REVIEW** (близнец). Завершает карточку CE-5 спецификации (§10.3,
A25, A27) для CE-backend сессии. Следующий — CE-6 (lifecycle, cutover,
полевая приёмка).

## 1. Что сделано

- `SaveEditAsUnique` под CE-backend сессией не закрывает её до создания
  копий: граница source (`ResetEditSession` + `ResetTransaction`) переходится
  только после успеха — для `ForThisPlacement` перед переносом размещения на
  уникальный корень (`SetCompositeAsset` последним), для `InParentDefinition`
  после публикации перепривязанного родителя. Legacy-путь (флаг выключен)
  по-прежнему закрывает сессию заранее.
- Исход батча в `LastPublishOutcome`: ни одной копии — **NoExternalChange**
  (warning `MH_W_NO_EXTERNAL_CHANGE`); часть копий создана, дальше сбой (в
  т.ч. отказ публикации родителя после созданной копии) — **PartialBatch**
  (созданные перечислены поимённо, как в R6-U1, плюс warning
  `MH_W_PARTIAL_BATCH`); успех — **Succeeded**. При любой неудаче сессия,
  draft, режим и проекция живы (проекция обновлена), shared-определение не
  тронуто; повтор с новыми именами возможен.

## 2. Тесты (red `e7a4ccb`)

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Composite.EditMode.Publish.UniqueFailureKeepsTheSessionAndNamesTheOrphans` | creator-seam отказывает на первой копии → false, NoExternalChange, та же сессия (epoch), режим жив, draft dirty, shared child byte-for-byte как до; seam создаёт первую копию и отказывает на второй → PartialBatch, имя созданной копии в warnings, сессия и dirty draft живы, child не тронут; retry с рабочим seam → Succeeded, сессия закрыта, child не тронут (размещение ушло на уникальный корень) |

## 3. Гейты

| Gate | Результат |
|---|---|
| RED (`e7a4ccb`) | `CE5B_RED_TEST.log`: Fail ×1 (`CE5B_RED_TEST2.log`; первый red `c82f463` не собрался — include) |
| GREEN non-unity/no-PCH build | `CE5B_GREEN_BUILD2.log`: Succeeded (первый red/green не собрались: в тесте не было include `Composite/MHCompositeImporter.h`) |
| `Mimir.V5.Composite.EditMode` | `CE5B_GREEN_TEST2.log`: 44/0 (EditContext+EditMode) |
| полный NullRHI suite | `CE5B_FULL.log`: `Success=265 Fail=0 (264 + 1)` |
| force-unity | `CE5B_FORCE_UNITY.log`: Succeeded |
| `BuildPlugin -StrictIncludes` | `CE5B_STRICT.log`: ExitCode=0 (Success) |
| `git diff --check`, `check_normative_docs.py` | чисто / OK |

## 4. Изменённые файлы

Editor (1): `Private/Composite/MHCompositeLevelSubsystem.cpp`. Tests:
`MHCompositeEditUniqueFailureTest.cpp` (новый). Docs:
`docs/RECIPE_EXECUTION_STATUS.md`, эта квитанция.

## 5. Вопросы

- Сироты partial-батча не удаляются автоматически (spec: «не делать
  transaction rollback файлов»); их список — в warnings и Message Log.
- `ExecuteSaveUnique` (UI) уже показывает warnings; отдельный текст исхода,
  как у Commit (CE-5a), — при полевой проверке, если owner попросит.
- CE-6: флаг `bCompositeEditModeV2` → default on, снятие legacy-пути
  (хэндлы/`Tick`/`SetPlacementEditMode`), save map / PIE / level change /
  shutdown (A28), полевые сценарии owner.
