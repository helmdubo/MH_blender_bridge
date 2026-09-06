# R6-D2 (Recipe Model v2.1) — публикация общего определения (Apply Shared Definition)

Статус: **MERGED** (близнец, #130). Завершает семью R6-D: draft вложенного
определения (R6-D0), хэндлы (R6-D1b) → источник ребёнка и все его размещения.

## 1. Что сделано

- `UMHCompositeLevelSubsystem::CommitEditComposite` для вложенной сессии →
  `CommitNestedEditComposite`: flush `Tick(0)` → `GetEditedCompositeDocument`
  (документ вложенного определения) → canonical preflight → снимок текущего
  документа ребёнка → выход из edit-mode, `ResetEditSession`, `ResetTransaction`
  (граница источника, как у корневого Commit) → `MHApplyCompositeV5(child)` →
  `MHPublishCompositeV5(child)` (атомарная запись `<child>.composite` в Source
  Root, сохранение пакета, `MHNotifyGeneratedResourceChanged` → targeted
  `ReconcileRecipe` у каждого размещения, вызывающего это определение) →
  `RebuildComposite` корня.
- Ошибка apply/publish: `RestoreDefinition` — реимпорт ребёнка из
  авторитетного источника, если файл есть; иначе возврат снимка документа в
  память; в обоих случаях уведомление потребителей. Ошибка публикации
  возвращается вызывающему; сессия закрыта.
- Решение owner (а), 2026-09-06: ревизионного контроля с Blender нет — файл
  перезаписывается как есть; последующий экспорт из Blender перезапишет его.
- UI: Composite Actions при вложенной сессии — «Apply Shared Definition»
  (подтверждение: имя `<child>.composite` и число размещений) и «Cancel Edit
  Contents»; Composite Outliner — те же два пункта в контекстном меню, строка
  статуса: «Drag the handles… Apply Shared Definition publishes to N
  placement(s)… Cancel Edit Contents discards».
- Test seam `SetCommitPublisherForTests` — контракт задокументирован: заменяет
  `MHPublishCompositeV5`, актив уже применён, seam обязан уведомить потребителей
  (`MHNotifyCompositeAssetChanged`) как настоящий publisher.

## 2. Тесты (red `576ad1f`)

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Composite.EditContext.ApplySharedDefinitionUpdatesConsumers` | Begin nested у A, сдвиг хэндла +100 X, Commit через seam: публикуется именно ребёнок (не корень), Undo очищен до границы источника, сессия и edit-mode закрыты, хэндлы удалены, документ ребёнка несёт `Nodes[0].TranslationCm = edited world × inverse(parent world)`, байты ребёнка изменились, корень не тронут; A и B (второе размещение) рендерят mesh C на +100 X |
| `…EditContext.ApplySharedDefinitionFailureKeepsDefinition` | seam отказывает (`MH_E_TEST_PUBLISH_REFUSED`): Commit false с этой ошибкой, сессия закрыта, байты ребёнка прежние, A и B рендерят mesh C на исходных местах |

## 3. Гейты

| Gate | Результат |
|---|---|
| RED (`576ad1f`) | `R6D2_RED_TEST.log`: Fail ×2 (оба новых теста; D0/D1b — Success) |
| GREEN non-unity/no-PCH build | `R6D2_GREEN_BUILD.log`: Succeeded |
| `Mimir.V5.Composite.EditContext` | `R6D2_GREEN_TEST.log`: 4/0 |
| полный NullRHI suite | `R6D2_GREEN_FULL.log`: `Success=225 Fail=0` (223 + 2) |
| force-unity | `R6D2_FORCE_UNITY.log`: Succeeded |
| `BuildPlugin -StrictIncludes` | `R6D2_STRICT.log`: ExitCode=0 (Success) |
| `git diff --check`, `check_normative_docs.py` | чисто / OK |

## 4. Изменённые файлы

Editor: `Public|Private/Composite/MHCompositeLevelSubsystem.{h,cpp}`,
`Private/UI/MHSourceToolMenus.{h,cpp}`, `Private/UI/MHCompositeOutliner.cpp`.
Tests: `MHCompositeEditContextTest.cpp`. Docs: `docs/RECIPE_EXECUTION_STATUS.md`,
эта квитанция.

## 5. Вопросы

Открытых нет. Публикация через `MHPublishCompositeV5` уведомляет потребителей
сама (targeted `ReconcileRecipe`), корень дополнительно `RebuildComposite` —
как и у корневого Commit; замер — R8. Следующий — R6-U (Save Unique / Make
Unique с явной областью).
